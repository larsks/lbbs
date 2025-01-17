/*
 * LBBS -- The Lightweight Bulletin Board System
 *
 * Copyright (C) 2023, Naveen Albert
 *
 * Naveen Albert <bbs@phreaknet.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief System and shell stuff
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#include "include/bbs.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <sched.h> /* use clone */
#include <syscall.h>
#include <dirent.h>

#ifdef __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(5,9,0)
#include <linux/close_range.h>
#if !defined(close_range) && defined(SYS_close_range)
/* The Linux API should be available, but glibc might not have a wrapper for it */
#define close_range(min, max, flags) syscall(SYS_close_range, min, max, flags)
#endif /* close_range */
#endif /* LINUX_VERSION_CODE */
#endif /* __linux__ */

#include "include/config.h"
#include "include/node.h"
#include "include/system.h"
#include "include/utils.h"
#include "include/term.h"
#include "include/user.h"
#include "include/transfer.h"

static char hostname[84] = "bbs";
static char templatedir[256] = "./rootfs";
static char rundir[256] = "/tmp/lbbs/rootfs";
static const char *oldrootname = "/.old";
static int maxmemory = 0;
static int maxcpu = 0;
static int minnice = 0;

static int load_config(void)
{
	struct bbs_config *cfg = bbs_config_load("system.conf", 0);

	if (!cfg) {
		return 0;
	}

	bbs_config_val_set_str(cfg, "container", "hostname", hostname, sizeof(hostname));
	bbs_config_val_set_path(cfg, "container", "templatedir", templatedir, sizeof(templatedir));
	bbs_config_val_set_path(cfg, "container", "rundir", rundir, sizeof(rundir));
	if (!s_strlen_zero(rundir)) {
		bbs_verb(3, "Creating %s\n", rundir);
		bbs_ensure_directory_exists_recursive(rundir);
	}
	bbs_config_val_set_int(cfg, "container", "maxmemory", &maxmemory);
	bbs_config_val_set_int(cfg, "container", "maxcpu", &maxcpu);
	if (!bbs_config_val_set_int(cfg, "container", "minnice", &minnice)) {
		if (minnice < -20 || minnice > 20) {
			bbs_error("minnice value '%d' is invalid\n", minnice);
			return -1;
		}
	}

	bbs_config_free(cfg); /* Destroy the config now, rather than waiting until shutdown, since it will NEVER be used again for anything. */
	return 0;
}

int bbs_init_system(void)
{
	return load_config();
}

/* Can be used to debug controlling terminal for child
 * (best done with something like /bin/bash, since it will complain if it can't set the terminal process group
 * Do NOT enable this in production, only for debugging if something is wrong.
 */
/* #define DEBUG_CHILD_TTY */

#ifdef DEBUG_CHILD_TTY
#define CHILD_ERR(fmt, ...) fprintf(stderr, fmt, __VA_ARGS__)
#define CHILD_DEBUG(fmt, ...) fprintf(fdout, fmt, __VA_ARGS__)

static int tcgetsid(int fd)
{
	pid_t sid;

	if (ioctl(fd, TIOCGSID, &sid)) {
		bbs_error("TIOCGSID failed: %s\n", strerror(errno));
		return -1;
	}
	return sid;
}
#endif

static int set_controlling_term(int fd)
{
	int pres;

#ifdef DEBUG_CHILD_TTY
#define CHILD_ERR(fmt, ...) fprintf(stderr, fmt, __VA_ARGS__)
#define CHILD_DEBUG(fmt, ...) fprintf(fdout, fmt, __VA_ARGS__)

	int origsid = getsid(getpid()); /* Should still be the parent's SID */

	CHILD_DEBUG("sid: %d, tcpgrp: %d\n", origsid, tcgetpgrp(fd));
	/* Make this the foreground process for this file descriptor. */
	/* Make the child the session leader, otherwise if we're launching a shell, it'll complain:
	 * bash: cannot set terminal process group (-1): Inappropriate ioctl for device
	 * bash: no job control in this shell
	 */
#endif
	pres = setsid(); /* Don't call setpgrp beforehand to set process group to process ID, since setsid also does this */
#ifdef DEBUG_CHILD_TTY
	if (pres) {
		CHILD_ERR("setsid: %s\n", strerror(errno));
	}
#endif
	pres = ioctl(fd, TIOCSCTTY, 1); /* Actually set the controlling terminal. This is key. */
#ifdef DEBUG_CHILD_TTY
	if (pres) {
		CHILD_ERR("TIOCSCTTY: %s\n", strerror(errno));
	}
	CHILD_DEBUG("tcsid: %d\n", tcgetsid(fd));
#endif
	pres = tcsetpgrp(fd, getpid()); /* Make this the controlling (foreground) process group for this file descriptor */
#ifdef DEBUG_CHILD_TTY
	if (pres) {
		CHILD_ERR("tcsetpgrp: %s\n", strerror(errno));
	}
	CHILD_DEBUG("origsid: %d, sid: %d, tcsid: %d, tcpgrp: %d\n", origsid, getsid(getpid()), tcgetsid(fd), tcgetpgrp(fd));
#endif
	return pres;
}

static void waitpidexit(pid_t pid, const char *filename, int *res)
{
	pid_t w;
	int status;

	/* Wait for the child process to exit. */
	do {
		w = waitpid(pid, &status, WUNTRACED | WCONTINUED);
		if (w == -1) {
			bbs_error("waitpid (%s): %s\n", filename, strerror(errno));
			break;
		}
		if (WIFEXITED(status)) { /* Child terminated normally */
			*res = WEXITSTATUS(status);
			bbs_debug(5, "Process %d (%s) exited, status %d\n", pid, filename, *res);
		} else if (WIFSIGNALED(status)) { /* Child terminated by signal */
			bbs_debug(3, "Process %d (%s) killed, signal %d\n", pid, filename, WTERMSIG(status));
			/* Return 0, and menu exec will return -1 if it detects node shutdown */
			*res = 0;
		} else if (WIFSTOPPED(status)) { /* Child stopped by signal */
			bbs_debug(3, "Process %d (%s) stopped, signal %d\n", pid, filename, WSTOPSIG(status));
			kill(pid, SIGCONT); /* Continue the child */
		} else if (WIFCONTINUED(status)) { /* Child resumed by SIGCONT */
			bbs_debug(3, "Process %d (%s) continued\n", pid, filename);
		} else {
			bbs_debug(3, "Process %d (%s) has status %d\n", pid, filename, status);
		}
	} while (!WIFEXITED(status) && !WIFSIGNALED(status));

	if (*res > 0) {
		/* Sometimes it can be legitimate for programs to exit nonzero, and that's not our fault. */
		switch (*res) {
			/* These are probably due to misconfigurations, and should be raised to the sysop's attention */
			case ENOENT:
			case EPERM:
				bbs_warning("Command failed (%d - %s): %s\n", *res, strerror(*res), filename);
				break;
			default:
				bbs_debug(1, "Command failed (%d - %s): %s\n", *res, strerror(*res), filename);
		}
	} else {
		bbs_debug(4, "Command execution finished (%s): res = %d\n", filename, *res);
	}
	return;
}

static int fdlimit = -1;

/* #define DEBUG_CHILD */

#ifdef DEBUG_CHILD
#define child_debug(level, ...) bbs_debug(level, __VA_ARGS__)
#else
#define child_debug(level, ...) ;
#endif

static int intsort(const void *a, const void *b)
{
	int x = *(const int*) a;
	int y = *(const int*) b;

	return (x > y) - (x < y);
}

#ifndef close_range
#define close_range(min, max, flags) my_close_range(min, max)
static void my_close_range(int start, int end)
{
	int i;
	for (i = start; i <= end; i++) {
		close(i);
	}
}
#endif

static void cleanup_fds(int maxfd, int fdin, int fdout, int exclude)
{
	int minfd = 0;
	int exempt = 0;
	int fds[4];

	/* Don't close these file descriptors */
	if (fdin >= 0) {
		fds[exempt++] = fdin;
	}
	if (fdout >= 0 && fdout != fdin) { /* These are often the same, if so, no need to include twice */
		fds[exempt++] = fdout;
	}
	if (exclude >= 0) {
		fds[exempt++] = exclude;
	}

	/* Sort the file descriptors */
	qsort(fds, (size_t) exempt, sizeof(int), intsort);

#ifdef DEBUG_CHILD
	/* Leave STDIN/STDOUT/STDERR open for debugging messages */
	minfd = STDERR_FILENO + 1;
#endif

	child_debug(5, "Cleaning up file descriptors [%d, %d], %d exempt: %d, %d, %d\n", minfd, maxfd, exempt, fdin, fdout, exclude);

	/* Close all open file descriptors, so the child doesn't inherit any of them, except for node->slavefd
	 * And yes, we close STDIN/STDOUT/STDERR as well since these refer to the sysop console, if there even is one.
	 * The BBS node has nothing to do with that. */

	/* If first is greater than last, close_range will return EINVAL,
	 * so we can just pass in the next excluded fd - 1 for each chunk. */
	if (exempt > 0) {
		close_range(minfd, fds[0] - 1, 0);
		minfd = fds[0] + 1;
		if (exempt > 1) {
			close_range(minfd, fds[1] - 1, 0);
			minfd = fds[1] + 1;
			if (exempt > 2) {
				close_range(minfd, fds[2] - 1, 0);
				minfd = fds[2] + 1;
			}
		}
	}
	close_range(minfd, maxfd, 0);
}

static int exec_pre(int fdin, int fdout, int exclude)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_NOFILE, &rl)) {
		return -1;
	}
	if (fdlimit == -1) {
		/* This is the first time anything is calling exec_pre */
		fdlimit = (int) sysconf(_SC_OPEN_MAX);
		child_debug(4, "Global FD limit is %d file descriptors\n", fdlimit);
	}

	if (fdlimit != (int) rl.rlim_max) {
		/* Prefer rlim_max, but make some noise if they don't match */
		child_debug(1, "SC_OPEN_MAX = %d but rlim_max = %lu?\n", fdlimit, rl.rlim_max);
		fdlimit = MIN(fdlimit, (int) rl.rlim_max);
		child_debug(7, "fdlimit %d\n", fdlimit);
	}

	cleanup_fds(fdlimit - 1, fdin, fdout, exclude);

	/* Assign the appropriate file descriptors */
	if (fdin != -1) {
		dup2(fdin, STDIN_FILENO);
	}
	if (fdout != -1) {
		dup2(fdout, STDOUT_FILENO);
		dup2(fdout, STDERR_FILENO);
	}

	return 0;
}

int bbs_argv_from_str(char **argv, int argc, char *s)
{
	int c = 0;
	int quoted = 0;
	char *start = s;

	ltrim(s);

	/* Parse a string (delimiting on spaces, but also handling quotes) into arguments for argv */
	while (*s) {
		if ((*s == ' ' && !quoted) || (*s == '"' && quoted)) {
			*s = '\0';
			argv[c] = start;
#ifdef EXTRA_DEBUG
			bbs_debug(8, "argv[%d] = %s\n", c, argv[c]);
#endif
			c++;
			s++;
			start = s;
		} else if (!quoted && *s == '"') {
			quoted = 1;
			if (start == s) {
				start++; /* Don't include the begin quote itself in the arg */
			}
			s++;
		} else {
			s++;
		}
		if (c >= argc - 1) { /* Subtract 1, since there MUST be a NULL after the last arg with data */
			bbs_warning("Truncation of arguments occured\n"); /* Sadly we have lost the original arg string since we split it up into the array, so we can't print it out here */
			break;
		}
	}
	if (s > start && c < argc - 1) {
		argv[c] = start;
#ifdef EXTRA_DEBUG
		bbs_debug(8, "argv[%d] = %s\n", c, argv[c]);
#endif
		c++;
	}
	argv[c] = NULL;
	return c;
}

/* Forward declaration */
static int __bbs_execvpe_fd(struct bbs_node *node, int usenode, int fdin, int fdout, const char *filename, char *const argv[], char *const envp[], int isolated);

int bbs_execvp(struct bbs_node *node, const char *filename, char *const argv[])
{
	return __bbs_execvpe_fd(node, 1, -1, -1, filename, argv, NULL, 0);
}

int bbs_execvp_isolated(struct bbs_node *node, const char *filename, char *const argv[])
{
	return __bbs_execvpe_fd(node, 1, -1, -1, filename, argv, NULL, 1);
}

int bbs_execvp_headless(struct bbs_node *node, const char *filename, char *const argv[])
{
	if (!node) {
		bbs_warning("It is not necessary to use %s if node is NULL\n", __func__);
	}
	return __bbs_execvpe_fd(node, 0, -1, -1, filename, argv, NULL, 0);
}

int bbs_execvp_fd(struct bbs_node *node, int fdin, int fdout, const char *filename, char *const argv[])
{
	return __bbs_execvpe_fd(node, 1, fdin, fdout, filename, argv, NULL, 0);
}

int bbs_execvp_fd_headless(struct bbs_node *node, int fdin, int fdout, const char *filename, char *const argv[])
{
	if (!node) {
		bbs_warning("It is not necessary to use %s if node is NULL\n", __func__);
	}
	return __bbs_execvpe_fd(node, 0, fdin, fdout, filename, argv, NULL, 0);
}

int bbs_execvpe_fd_headless(struct bbs_node *node, int fdin, int fdout, const char *filename, char *const argv[], char *const envp[])
{
	if (!node) {
		bbs_warning("It is not necessary to use %s if node is NULL\n", __func__);
	}
	return __bbs_execvpe_fd(node, 0, fdin, fdout, filename, argv, envp, 0);
}

static int update_map(const char *mapping, const char *map_file, int map_len)
{
	int fd;

	fd = open(map_file, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", map_file, strerror(errno));
		return -1;
	}
	if (write(fd, mapping, (size_t) map_len) != map_len) {
		fprintf(stderr, "write failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int proc_setgroups_write(pid_t pid, const char *str, int str_len)
{
	char setgroups_path[PATH_MAX];
	int fd;

	snprintf(setgroups_path, sizeof(setgroups_path), "/proc/%ld/setgroups", (long) pid);
	fd = open(setgroups_path, O_RDWR);
	if (fd < 0) {
		if (errno != ENOENT) {
			fprintf(stderr, "open failed: %s\n", strerror(errno));
			return -1;
		}
		return 0;
	}

	if (write(fd, str, (size_t) str_len) != str_len) {
		fprintf(stderr, "write failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int setup_namespace(pid_t pid)
{
	char map_buf[100];
	char map_path[PATH_MAX];
	char *uid_map = map_buf, *gid_map = map_buf;
	int map_len;

	snprintf(map_path, sizeof(map_path), "/proc/%d/uid_map", pid);
	map_len = snprintf(map_buf, sizeof(map_buf), "0 %ld 1", (long) getuid());
	if (update_map(uid_map, map_path, map_len)) {
		return -1;
	}

	proc_setgroups_write(pid, "deny", strlen("deny"));

	snprintf(map_path, sizeof(map_path), "/proc/%d/gid_map", pid);
	map_len = snprintf(map_buf, sizeof(map_buf), "0 %ld 1", (long) getgid());
    if (update_map(gid_map, map_path, map_len)) {
		return -1;
	}

	return 0;
}

static void temp_container_root(char *buf, size_t len, int pid)
{
	snprintf(buf, len, "%s/%d", rundir, pid);
}

static int clone_container(char *rootdir, size_t rootlen, int pid)
{
	/* templatedir contains a base, template root filesystem.
	 * However, we need to clone certain directories for a functional "container",
	 * since they need to be writable. */

	DIR *dir;
	struct dirent *entry;

	if (!(dir = opendir(templatedir))) {
		bbs_error("Error opening directory - %s: %s\n", templatedir, strerror(errno));
		return -1;
	}

	/* Each session (not just each user) gets its own directory. So use the current PID, not the user ID. */
	temp_container_root(rootdir, rootlen, pid);
	if (!eaccess(rootdir, R_OK) && bbs_delete_directory(rootdir)) {
		/* If it exists, delete it, it must be leftover from a previous session with the same PID.
		 * Can't be an in-use session because that would imply a second process with the same PID. */
		closedir(dir);
		return -1;
	}

	/* Now, make the directory fresh */
	if (mkdir(rootdir, 0700)) {
		bbs_error("mkdir(%s) failed: %s\n", rootdir, strerror(errno));
		closedir(dir);
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) { /* Don't just bail out if errno becomes set, modules could set errno when we load them. */
		char fulldir[PATH_MAX];
		char symlinkdir[PATH_MAX];
		if (entry->d_type != DT_DIR || !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}
		/* A Debian container should have these top-level directories:
		 * /bin
		 * /boot
		 * /dev
		 * /etc
		 * /home
		 * /lib
		 * /lib64
		 * /media
		 * /mnt
		 * /opt
		 * /proc
		 * /root
		 * /run
		 * /sbin
		 * /srv
		 * /sys
		 * /tmp
		 * /usr
		 * /var
		 */

		snprintf(fulldir, sizeof(fulldir), "%s/%s", templatedir, entry->d_name);
		snprintf(symlinkdir, sizeof(symlinkdir), "%s/%s", rootdir, entry->d_name);

		/* We can't bind without a directory existing there already */
		if (mkdir(symlinkdir, 0700)) {
			bbs_error("mkdir(%s) failed: %s\n", symlinkdir, strerror(errno));
			return -1;
		}

		/* Don't symlink these, we'll make fresh copies momentarily */
		if (!strcmp(entry->d_name, "proc") || !strcmp(entry->d_name, "tmp") || !strcmp(entry->d_name, "home")) {
			continue;
		}
		/* MS_REMOUNT is needed for MS_RDONLY to actually take effect for this mountpoint. See mount(2).
		 * However, it's a bit peculiar. MS_REMOUNT can only be used if it's already mounted.
		 * So we have to mount it first without MS_REMOUNT, then mount again with MS_REMOUNT. */
		if (mount(fulldir, symlinkdir, "ext4", MS_BIND | MS_REC | MS_RDONLY, NULL)) {
			bbs_error("mount %s as %s failed: %s\n", fulldir, symlinkdir, strerror(errno));
			return -1;
		}
		if (mount(fulldir, symlinkdir, "ext4", MS_REMOUNT | MS_BIND | MS_REC | MS_RDONLY, NULL)) {
			bbs_error("mount %s as %s failed: %s\n", fulldir, symlinkdir, strerror(errno));
			return -1;
		}
	}
	closedir(dir);

	return 0;
}

static int set_limit(int resource, int value)
{
	rlim_t limit;
	struct rlimit r;

	if (!value) { /* Nothing to set */
		return 0;
	} else if (value < 0) {
		bbs_error("Invalid rlimit value, ignoring: %d\n", value);
		return 0;
	}

	limit = (unsigned long) value;
	memset(&r, 0, sizeof(r));

	if (getrlimit(resource, &r)) {
		bbs_error("getrlimit failed: %s\n", strerror(errno));
		return -1;
	}

	/* Set soft and hard limits */
	if (r.rlim_cur > limit) {
		r.rlim_cur = limit;
	}
	if (r.rlim_max > limit) {
		r.rlim_max = limit;
	}

	if (setrlimit(resource, &r)) {
		bbs_error("setrlimit failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int set_limits(void)
{
	int res = 0;

	/* Control resource consumption inside the container */
	/* Not RLIMIT_DATA, which isn't as encompassing */
	res = set_limit(RLIMIT_AS, 1024 * maxmemory); /* Value is in KB, so convert MB to bytes */
	if (!res) {
		res = set_limit(RLIMIT_CPU, maxcpu);
	}
	if (!res && minnice) {
		res = set_limit(RLIMIT_NICE, 20 - minnice); /* Ceiling = 20 - value, so value = 20 - ceiling */
	}
	return res;
}

/*! \brief Read from a file descriptor until it closes */
static ssize_t full_read(int fd, char *restrict buf, size_t len)
{
	ssize_t total = 0;
	for (;;) {
		ssize_t res = read(fd, buf, len);
		if (res < 0) {
			return res;
		} else if (!res) { /* In the unlikely case that we exhaust the buffer, if len is 0, it will return 0 anyways */
			break;
		}
		total += res;
		buf += res;
		len -= (size_t) res;
	}
	return total;
}

static int __bbs_execvpe_fd(struct bbs_node *node, int usenode, int fdin, int fdout, const char *filename, char *const argv[], char *const envp[], int isolated)
{
	pid_t pid;

	int fd = fdout;
	int res = -1;
	int pfd[2], procpipe[2];
	char fullpath[256] = "", fullterm[32] = "", fulluser[48] = "", homeenv[433] = "HOME=";
	char *parentpath;
#define MYENVP_SIZE 5 /* End with 3 NULLs so we can add up to 2 env vars if needed */
	char *myenvp[MYENVP_SIZE] = { fullpath, fullterm, NULL, NULL, NULL }; /* Last NULL is always the sentinel */

	if (!envp) {
		envp = myenvp;
	}

	parentpath = getenv("PATH"); /* Use $PATH with which BBS was started, for execvpe */
	if (parentpath) {
		snprintf(fullpath, sizeof(fullpath), "PATH=%s", parentpath);
	}

	bbs_debug(6, "node: %p, usenode: %d, fdin: %d, fdout: %d, filename: %s, isolated: %s\n", node, usenode, fdin, fdout, filename, isolated ? "yes" : "no");
	if (node && usenode && (fdin != -1 || fdout != -1)) {
		bbs_warning("fdin/fdout should not be provided if usenode == 1 (node is preferred, fdin/fdout will be ignored)\n");
	}

	/* If we have a node, use its fd for STDIN/STDOUT/STDERR */
	if (node && usenode) {
		/* If a node thread is calling this function, it MUST
		 * pass a handle to node (it MUST NOT pass NULL),
		 * regardless of whether the node is to be used for I/O.
		 * This is because we must be able to kill the child process,
		 * i.e. we must store node->childpid so the child can be killed
		 * if needed. Think of it like "autoservicing" (except way simpler).
		 * This is why the clunky "usenode" param exists.
		 */
		fd = node->slavefd;
		fdin = fdout = fd;
		bbs_assert(isatty(fd));
		/* Don't call tcgetsid here, it will fail */
		bbs_debug(6, "sid: %d, tcpgrp: %d\n", getsid(getpid()), tcgetpgrp(fd));
		safe_strncpy(fullterm, "TERM=xterm", sizeof(fullterm)); /* Many interactive programs will whine if $TERM is not set */
	}
	if (fdout == -1) {
		/* If no node and no output fd, create file descriptors using a temporary pipe */
		if (pipe(pfd)) {
			bbs_error("pipe failed (%s): %s\n", filename, strerror(errno));
			return -1;
		}
	}

	/* If we have flags, we need to use clone(2). Otherwise, just use fork(2) */
	if (isolated) {
		int flags = 0;
		/* We need to do more than fork() allows */
		flags |= SIGCHLD | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWUSER; /* fork() sets SIGCHLD implicitly. */
#if 0
		flags |= CLONE_CLEAR_SIGHAND; /* Don't inherit any signals from parent. */
#else
		flags &= ~CLONE_SIGHAND;
#endif

		if (eaccess(templatedir, R_OK)) {
			bbs_error("rootfs template directory '%s' does not exist\n", templatedir);
			return -1;
		} else if (pipe(procpipe)) {
			bbs_error("pipe failed: %s\n", strerror(errno));
			return -1;
		}

		/* We use the clone syscall directly, rather than the clone(2) glibc function.
		 * The reason for this is clone launches a function for the child,
		 * whereas the raw syscall is similar to fork and continues in the child as well.
		 * This allows us to use the same logic for fork and clone.
		 * Using the syscall is not as portable as using the function,
		 * but our usage here is portable to x86-64, which is pretty much everything anyways.
		 */
		pid = (pid_t) syscall(SYS_clone, flags, NULL, NULL, NULL, 0);
	} else {
		pid = fork(); /* fork has an implicit SIGCHLD */
	}

	if (pid == -1) {
		bbs_error("%s failed (%s): %s\n", isolated ? "clone" : "fork", filename, strerror(errno));
		if (isolated) {
			close(procpipe[0]);
			close(procpipe[1]);
		}
		return -1;
	} else if (pid == 0) { /* Child */
		if (!isolated) {
			/* Immediately install a dummy signal handler for SIGWINCH.
			 * Until we call exec, the child retains the parent's signal handlers.
			 * However, if we have a node, we immediately call bbs_node_update_winsize
			 * to force send a SIGWINCH to the child immediately, to give it its current dimensions.
			 * If we don't block SIGWINCH in the child, then it'll run __sigwinch_handler
			 * from bbs.c, which will print out a log message if parent has option_nofork (bad!)
			 * This still reeks of a race condition, but in practice we're able to block the signal
			 * here ASAP before the parent does the SIGWINCH (but if we didn't do this, we would
			 * send the SIGWINCH before the child executes exec).
			 * It's actually a GOOD thing the SIGWINCH is sent prior to exec, this way the child
			 * process has the dimensions available immediately when the program starts. So this
			 * is probably the best thing to do here.
			 */

			/* SIG_IGN will survive exec, so we must avoid that.
			 * We could install an empty handler since we want this to go away with exec(), or just use SIG_DFL to reset to default.
			 * Normally we should use sigaction over signal, and probably here too, but this signal handler
			 * will only be relevant from the time between fork() and exec(), and my intuition suggests that
			 * signal will execute faster than sigaction. I have not actually verified this though. */
			/* XXX Maybe we should just always use clone() and always pass CLONE_CLEAR_SIGHAND */
			signal(SIGWINCH, SIG_DFL);
			/* Reset other signal handlers */
			signal(SIGTERM, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			signal(SIGPIPE, SIG_DFL);
		} /* else, if CLONE_CLEAR_SIGHAND was provided to clone, then the signal handlers didn't carry over, we're good. */

		if (fdout == -1) {
			close(pfd[0]); /* Close read end of pipe */
			fd = pfd[1]; /* Use write end of pipe */
			fdout = fd;
		}
		exec_pre(fdin, fdout, isolated ? procpipe[0] : -1); /* If we still need procpipe, don't close that */
		if (node && usenode) {
			/* Set controlling terminal, or otherwise shells don't fully work properly. */
			if (set_controlling_term(STDIN_FILENO)) { /* we dup2'd this to the slavefd. This is NOT the parent's STDIN. */
				/* If anything failed, abort.
				 * We can't do any logging in the child.
				 * Exit with errno and the parent will know what happened (although it may be ambiguous which function failed...)
				 */
				_exit(errno);
			}
		}

#define SYSCALL_OR_DIE(func, ...) if (func(__VA_ARGS__) < 0) { fprintf(stderr, #func " failed (ln %d): %s\n", __LINE__, strerror(errno)); _exit(errno); }
#ifndef pivot_root
#define pivot_root(new, old) syscall(SYS_pivot_root, new, old)
#endif

		if (isolated) {
			struct utsname uts;
			char pidbuf[15];
			char oldroot[384 + STRLEN("/.old")], newroot[384];
			char homedir[438];

			if (set_limits()) {
				_exit(errno);
			}

			/* Wait until parent has updated mappings. */
			res = (int) full_read(procpipe[0], pidbuf, sizeof(pidbuf));
			if (res < 1) {
				fprintf(stderr, "read returned %d for fd %d: %s\n", res, procpipe[0], strerror(errno));
				_exit(errno);
			}
			close(procpipe[0]);

			/* Prepare temporary container */
			if (clone_container(newroot, sizeof(newroot), atoi(pidbuf))) {
				_exit(errno);
			}

			/* Instead of showing root@bbs if we're launching a shell, which is just confusing, show the BBS username */
			if (node && envp == myenvp) {
				char *tmp;
				char masterhomedir[256];

				const char *username = bbs_user_is_registered(node->user) ? bbs_username(node->user) : "guest";
				/* Used if /root/.bashrc in rootfs contains this prompt override:
				 * PS1='${debian_chroot:+($debian_chroot)}$BBS_USER@\h:\w\$ '
				 */
				myenvp[2] = fulluser;
				snprintf(fulluser, sizeof(fulluser), "BBS_USER=%s", username);
				/* Make it all lowercase, per *nix conventions */
				username = tmp = fulluser + STRLEN("BBS_USER=");
				while (*tmp) {
					*tmp= (char) tolower(*tmp);
					tmp++;
				}

				if (bbs_user_is_registered(node->user)) {
					/* Make the user's home directory accessible within the container, at /home/${BBS_USERNAME} in the container */
					if (bbs_transfer_home_dir(node->user->id, masterhomedir, sizeof(masterhomedir))) {
						_exit(errno);
					}
					snprintf(homeenv + STRLEN("HOME="), sizeof(homeenv) - STRLEN("HOME="), "/home/%s", username);
					snprintf(homedir, sizeof(homedir), "%s/home/%s", newroot, username);
					SYSCALL_OR_DIE(mkdir, homedir, 0700);
					SYSCALL_OR_DIE(mount, masterhomedir, homedir, "bind", MS_BIND | MS_REC, NULL);

					/* Also set the $HOME var to change the home directory from /root to /home/${BBS_USERNAME} */
					myenvp[3] = homeenv;
					/* However, now that we changed $HOME, bash for example will look for /home/${BBS_USERNAME}/.bashrc, not /root/.bashrc
					 * So if the files in /root do not exist in the user's home directory, copy them there. */
				}
			}

			snprintf(oldroot, sizeof(oldroot), "%s%s", newroot, oldrootname);

			SYSCALL_OR_DIE(sethostname, hostname, strlen(hostname)); /* Change hostname in child's UTS namespace */
			SYSCALL_OR_DIE(uname, &uts);

			/* Set mount points */
			SYSCALL_OR_DIE(mount, newroot, newroot, "bind", MS_BIND | MS_REC, "");
			if (eaccess(oldroot, R_OK)) {
				SYSCALL_OR_DIE(mkdir, oldroot, 0700); /* If ./rootfs/.old doesn't yet exist, create it in the rootfs */
			}
			SYSCALL_OR_DIE(pivot_root, newroot, oldroot);
			SYSCALL_OR_DIE(mount, "proc", "/proc", "proc", 0, NULL);
			SYSCALL_OR_DIE(chdir, "/");
			SYSCALL_OR_DIE(umount2, oldrootname, MNT_DETACH);

			if (node && envp == myenvp) {
				FILE *fp;
				/* cd to the home directory; this way, if this is launching a shell session,
				 * it's a better user experience. Only makes sense to do this after we've changed the root. */
				SYSCALL_OR_DIE(chdir, homeenv + STRLEN("HOME="));
				/* We also have to handle the motd (Message of the Day).
				 * The shell does not display the MOTD, the login program does after login before spawning the shell.
				 * So, if there's an /etc/motd in the container, display its contents before we actually call exec.
				 * Of course, we should only do this if we are actually launching a shell!
				 * Fortunately, /etc/shells contains the list of shells, so we don't need to guess or hardcode a list. */
				fp = fopen("/etc/shells", "r");
				if (fp) {
					char line[64];
					int is_shell = 0;
					while ((fgets(line, sizeof(line), fp))) {
						bbs_strterm(line, '\n');
						if (!strcmp(line, filename)) {
							is_shell = 1;
							break;
						}
					}
					fclose(fp);
					if (is_shell) {
						fp = fopen("/etc/motd", "r");
						if (fp) {
							while ((fgets(line, sizeof(line), fp))) {
								fputs(line, stdout); /* Use fputs instead of puts, since puts adds its own newline */
							}
							fclose(fp);
						}
					}
				}
				/* Other things to keep in mind for shells specifically:
				 * bash will print exit after it closes. This is standard behavior (for bash) whenever exiting
				 * an interactive shell that is not a login shell.
				 * A login shell has a - at the beginning of its progname, e.g. echo $0 will show -bash instead of bash.
				 * It *might* make sense to consider this is a login shell, but it also might not.
				 * It is the first shell session that we're spawning, but it's just a program being launched from the BBS.
				 * So currently, it's not considered a login shell, and that's probably just fine. */
			}
		}

		res = execvpe(filename, argv, envp);
		bbs_assert(res == -1);

		/* For menu exec: handler, we hold a RDLOCK on the menu when we get here, and it was locked in this thread,
		 * helgrind will show an error if exec fails and we fall through here.
		 * See:
		 * - https://stackoverflow.com/questions/45889293/forking-while-holding-a-lock
		 * - https://stackoverflow.com/questions/2620313/how-to-use-pthread-atfork-and-pthread-once-to-reinitialize-mutexes-in-child
		 * Unfortunately, pthread_atfork doesn't really solve the issue.
		 * For now, I just accept that we'll have this helgrind issue when exec fails,
		 * it doesn't really matter since fork should always be followed by exec in the child,
		 * and if it fails, we die anyways.
		 */
		/* Can't use BBS logging in the child.
		 * But the parent will know that we failed since we return errno, and parent can log it. */
		/* Also, use _exit, not exit, since exit will execute the parent's atexit function, etc.
		 * That matters because exec failed, so atexit is still registered. */
		_exit(errno);
	} /* else, parent */

	if (isolated) {
		close(procpipe[0]);
		res = setup_namespace(pid);
		if (!res) {
			char childpid[10];
			/* Also write the child PID, since with CLONE_NEWPID, the child can't use getpid() to get the real child PID */
			size_t pidlen = (size_t) snprintf(childpid, sizeof(childpid), "%d", pid);
			bbs_write(procpipe[1], childpid, pidlen); /* Write to write end of pipe, to signal that UID/GID maps have been updated. */
		}
		close(procpipe[1]);
	}

	if (fd == -1) {
		close(pfd[1]); /* Close write end of pipe */
	}
	/* If terminal is resized by the network comm driver,
	 * and we're executing an external program, then
	 * we should be able to send a SIGWINCH to the child process
	 * to let it know too.
	 * Set a flag so the core knows to send a SIGWINCH.
	 */
	if (node) {
		bbs_node_lock(node);
		node->childpid = pid;
		bbs_node_unlock(node);
		/* Immediately send a SIGWINCH, because we skipped sending SIGWINCHes on window resizing if the node did not have a child process.
		 * Thus, the PTY is not even aware what the window dimensions are presently. We need this or the child will initially have 0x0,
		 * until it gets another SIGWINCH due to a future resize. By doing this, it has its current dimensions from the get go. */
		bbs_node_update_winsize(node, -1, -1); /* Call with -1 as args to simply send a SIGWINCH using existing dimensions. */
	}

	bbs_debug(5, "Waiting for process %d to exit\n", pid);
	waitpidexit(pid, filename, &res);
	if (node) {
		/* If we're being shut down right now, it's likely the child process
		 * was actually killed in node_shutdown, in which case we're not going
		 * to be able to acquire the node lock.
		 * However, we want to set this ASAP to let the node_shutdown thread
		 * that the child has exited, as it's actively polling this value
		 * to check if it has changed to 0... if not, it'll kill it forcefully.
		 * Just blindly set it to 0.
		 */
		node->childpid = 0;
	}
	if (isolated) {
		char rootdir[268];
		/* Clean up the temporary container, if one was created */
		temp_container_root(rootdir, sizeof(rootdir), pid);
		if (!eaccess(rootdir, R_OK) && bbs_delete_directory(rootdir)) {
			bbs_warning("Failed to remove temporary container rootfs: %s\n", rootdir);
		}
	}
	if (fd == -1) {
		if (bbs_poll(pfd[0], 0) == 0) {
			/* The child has exited, so all the data that will ever be in the pipe is already here.
			 * If there's nothing there, then poll with 0 to skip blocking unnecessarily on read for a few seconds. */
			bbs_debug(3, "pipe poll returned 0\n");
		} else {
			for (;;) {
				char buf[1024]; /* Who knows how much data is in the pipe, make it big enough so we're not super fragmented, but not super big */
				ssize_t nbytes = read(pfd[0], buf, sizeof(buf)); /* Read from the pipe. */
				if (nbytes <= 0) { /* read will return 0 when the pipe is empty */
					break; /* End of pipe */
				}
				/* Log the output from the exec, but we do nothing else in particular with it. */
				bbs_debug(6, "exec output: %.*s\n", (int) nbytes, buf);
			}
		}
		close(pfd[0]);
	}
	return res;
}
