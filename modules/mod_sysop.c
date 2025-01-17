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
 * \brief Sysop console
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#include "include/bbs.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h> /* use isprint */
#include <poll.h>
#include <pthread.h>
#include <sys/un.h>	/* use struct sockaddr_un */

#include "include/node.h"
#include "include/pty.h"
#include "include/module.h"
#include "include/term.h"
#include "include/mail.h"
#include "include/history.h"
#include "include/utils.h" /* use bbs_dump_threads */
#include "include/startup.h"
#include "include/alertpipe.h"
#include "include/cli.h"

extern int option_nofork;

#define my_set_stdout_logging bbs_cli_set_stdout_logging

/* Since we now support remote consoles, bbs_printf is not logical to use in this module */
#ifdef bbs_printf
#undef bbs_printf
#endif
#define bbs_printf __Do_not_use_bbs_printf_use_bbs_dprintf
/* Use the macro to suppress unused macro warning with -Wunused-macros */
#ifdef bbs_printf
#endif

static int console_alertpipe[2];
static int unloading = 0;

static void show_copyright(int fd, int footer)
{
	bbs_dprintf(fd,
	BBS_TAGLINE ", " BBS_COPYRIGHT "\n"
	BBS_SHORTNAME " comes with ABSOLUTELY NO WARRANTY; for details type '/warranty'\n"
	"This is free software, and you are welcome to redistribute it\n"
	"under certain conditions; type '/copyright' for details.\n");
	if (footer) {
		bbs_dprintf(fd, "====================================================================\n");
	}
}

static void show_license(int fd)
{
	bbs_dprintf(fd,
	BBS_SHORTNAME " is free software; you can redistribute it and/or modify\n"
	"it under the terms of the GNU General Public License version 2 as\n"
	"published by the Free Software Foundation.\n\n"
	"This program also contains components licensed under other licenses.\n"
	"They include:\n\n"
	"This program is distributed in the hope that it will be useful,\n"
	"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
	"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
	"GNU General Public License for more details.\n\n"
	"You should have received a copy of the GNU General Public License\n"
	"along with this program; if not, write to the Free Software\n"
	"Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA\n");
}

static void show_warranty(int fd)
{
	bbs_dprintf(fd, "                            NO WARRANTY\n"
	"BECAUSE THE PROGRAM IS LICENSED FREE OF CHARGE, THERE IS NO WARRANTY\n"
	"FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW.  EXCEPT WHEN\n"
	"OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES\n"
	"PROVIDE THE PROGRAM \"AS IS\" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED\n"
	"OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF\n"
	"MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE ENTIRE RISK AS\n"
	"TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU.  SHOULD THE\n"
	"PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING,\n"
	"REPAIR OR CORRECTION.\n\n"
	"IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING\n"
	"WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MAY MODIFY AND/OR\n"
	"REDISTRIBUTE THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES,\n"
	"INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING\n"
	"OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED\n"
	"TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY\n"
	"YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER\n"
	"PROGRAMS), EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE\n"
	"POSSIBILITY OF SUCH DAMAGES.\n");
}

struct sysop_console {
	int sfd;
	int fdin;
	int fdout;
	pthread_t thread;
	unsigned int remote:1;
	unsigned int dead:1;
	unsigned int log:1;
	RWLIST_ENTRY(sysop_console) entry;
};

static RWLIST_HEAD_STATIC(consoles, sysop_console);

static int cli_testemail(struct bbs_cli_args *a)
{
	UNUSED(a);
	return bbs_mail(0, NULL, NULL, NULL, "Test Email", "This is a test email.\r\n\t--LBBS");
}

static int cli_mtrim(struct bbs_cli_args *a)
{
	size_t released = bbs_malloc_trim();
	bbs_dprintf(a->fdout, "%lu bytes released\n", released);
	return 0;
}

static int cli_assert(struct bbs_cli_args *a)
{
	/* Development testing only: this command is not listed */
	char *tmp = NULL;
	UNUSED(a);
	bbs_assert_exists(tmp);
	return 0;
}

static int cli_copyright(struct bbs_cli_args *a)
{
	show_copyright(a->fdout, 0);
	return 0;
}

static int cli_license(struct bbs_cli_args *a)
{
	show_license(a->fdout);
	return 0;
}

static int cli_warranty(struct bbs_cli_args *a)
{
	show_warranty(a->fdout);
	return 0;
}

static int sysop_command(struct sysop_console *console, const char *s)
{
	int res;

	my_set_stdout_logging(console->fdout, console->log);
	res = bbs_cli_exec(console->fdin, console->fdout, s);
	my_set_stdout_logging(console->fdout, console->log); /* Reset, in case a CLI command changed it */

	if (res && errno == ENOENT) {
		bbs_dprintf(console->fdout, "ERROR: Invalid command: '%s'. Press '?' for help.\n", s);
	}

	return res;
}

static void console_cleanup(struct sysop_console *console)
{
	bbs_assert(console->remote);
	RWLIST_WRLOCK(&consoles);
	RWLIST_REMOVE(&consoles, console, entry);
	/* If unloading, these have already been closed */
	if (!console->dead) {
		bbs_remove_logging_fd(console->fdout);
		bbs_socket_close(&console->fdin);
		bbs_socket_close(&console->fdout);
		bbs_socket_close(&console->sfd);
	}
	free(console);
	RWLIST_UNLOCK(&consoles);
}

static void print_time(int fdout)
{
	char timebuf[40];
	time_t now;
	struct tm nowdate;

	now = time(NULL);
	localtime_r(&now, &nowdate);
	strftime(timebuf, sizeof(timebuf), "%a %b %e %Y %I:%M:%S %P %Z", &nowdate);
	bbs_dprintf(fdout, "%s\n", timebuf);
}

static void *sysop_handler(void *varg)
{
	char buf[1];
	char cmdbuf[256];
	int res;
	struct pollfd pfds[2];
	char titlebuf[84];
	int sysopfdin, sysopfdout;
	const char *histentry;
	struct sysop_console *console = varg;

	sysopfdin = console->fdin;
	sysopfdout = console->fdout;

	console->log = 1; /* Logging to console enabled by default */
	if (console->remote) {
		bbs_add_logging_fd(sysopfdout);
	}

	/* Keep it short but descriptive, for a user to differentiate sysop consoles on multiple systems, as well as foreground vs remote. */
	snprintf(titlebuf, sizeof(titlebuf), "%s%s%s", console->remote ? "Sysop" : "LBBS", S_COR(bbs_hostname(), "@", ""), S_IF(bbs_hostname()));
	bbs_dprintf(sysopfdout, TERM_TITLE_FMT, titlebuf);

	/* Disable input buffering so we can read a character as soon as it's typed */
	if (bbs_unbuffer_input(sysopfdin, 0)) {
		bbs_error("Failed to unbuffer fd %d, sysop console will be unavailable\n", sysopfdin);
		/* If this fails, the foreground console is just not going to work properly.
		 * For example, supervisorctl doesn't seem to have a TTY/PTY available.
		 * Just use screen or tmux? */
		goto cleanup;
	}

	pfds[0].fd = sysopfdin;
	pfds[0].events = POLLIN | POLLPRI | POLLERR | POLLHUP | POLLNVAL;

	pfds[1].fd = console_alertpipe[0];
	pfds[1].events = POLLIN;

	show_copyright(sysopfdout, 1);

	histentry = NULL; /* initiailization must be after pthread_cleanup_push to avoid "variable might be clobbered" warning */
	for (;;) {
		pfds[0].revents = pfds[1].revents = 0;
		res = poll(pfds, 2, -1);
		if (console->dead) {
			bbs_debug(3, "Console %d/%d has been instructed to exit\n", sysopfdin, sysopfdout);
			break;
		}
		if (res < 0) {
			if (errno != EINTR) {
				bbs_debug(3, "poll returned %d: %s\n", res, strerror(errno));
				break;
			}
			continue;
		}
		if (pfds[1].revents) {
			my_set_stdout_logging(sysopfdout, console->log);
			bbs_buffer_input(sysopfdin, 1);
			break;
		} else if (pfds[0].revents & POLLIN) {
			ssize_t bytes_read = read(sysopfdin, buf, sizeof(buf));
			if (bytes_read <= 0) {
				bbs_debug(5, "read returned %ld\n", bytes_read);
				break;
			}
			switch (tolower(buf[0])) {
				case '?':
				case 'h':
					bbs_dprintf(sysopfdout, " == Quick Commands ==\n");
					bbs_dprintf(sysopfdout, "? - Show help\n");
					bbs_dprintf(sysopfdout, "c - Clear screen\n");
					bbs_dprintf(sysopfdout, "h - Show help\n");
					bbs_dprintf(sysopfdout, "l - Enable/disable logging to this console\n");
					bbs_dprintf(sysopfdout, "n - List active nodes\n");
					bbs_dprintf(sysopfdout, "q - Shut down the BBS (with confirmation)\n");
					bbs_dprintf(sysopfdout, "s - Show BBS system status\n");
					bbs_dprintf(sysopfdout, "t - Show BBS system time\n");
					bbs_dprintf(sysopfdout, "u - Show list of users\n");
					bbs_dprintf(sysopfdout, "UP -> Previous command\n");
					bbs_dprintf(sysopfdout, "DN -> More recent command\n");
					bbs_cli_exec(sysopfdin, sysopfdout, "help");
					break;
				case 'c':
					bbs_dprintf(sysopfdout, TERM_CLEAR); /* TERM_CLEAR doesn't end in a newline, so normally, flush output, but bbs_printf does this for us. */
					bbs_dprintf(sysopfdout, "\033[3J"); /* Clear scrollback buffer */
					break;
				case 'l':
					SET_BITFIELD(console->log, !console->log); /* Save the new log setting */
					my_set_stdout_logging(sysopfdout, console->log); /* Make it take effect immediately */
					bbs_dprintf(sysopfdout, "Logging is now %s for %s console\n", console->log ? "enabled" : "disabled", console->remote ? "this remote" : "the foreground");
					break;
				case 'n':
					bbs_cli_exec(sysopfdin, sysopfdout, "nodes");
					break;
				case 's':
					bbs_view_settings(sysopfdout);
					break;
				case 't':
					print_time(sysopfdout);
					break;
				case 'u':
					bbs_cli_exec(sysopfdin, sysopfdout, "users");
					break;
				case 'q':
					{
						int do_quit = 0;
						my_set_stdout_logging(sysopfdout, 0); /* Disable logging so other stuff isn't trying to write to STDOUT at the same time. */
						bbs_dprintf(sysopfdout, "\n%sReally shut down the BBS? [YN] %s", COLOR(COLOR_RED), COLOR_RESET);
						res = poll(pfds, console->remote ? 1 : 2, 10000);
						if (res < 0) {
							if (errno != EINTR) {
								bbs_error("poll returned %d: %s\n", res, strerror(errno));
							}
						} else if (res == 0) {
							bbs_dprintf(sysopfdout, "\nShutdown attempt expired\n");
						} else if (pfds[1].revents) {
							/* alertpipe had activity in the meantime */
							my_set_stdout_logging(sysopfdout, console->log);
							bbs_buffer_input(sysopfdin, 1);
							goto cleanup;
						} else {
							bytes_read = read(sysopfdin, buf, 1);
							if (bytes_read <= 0) {
								bbs_debug(5, "read returned %ld\n", bytes_read);
							} else if (buf[0] == 'y' || buf[0] == 'Y') {
								do_quit = 1;
							}
						}
						bbs_dprintf(sysopfdout, "\n");
						if (do_quit) {
							bbs_cli_exec(sysopfdin, sysopfdout, "shutdown");
						}
					}
					break;
				case KEY_ESC:
					res = bbs_read_escseq(sysopfdin);
					switch (res) {
						case KEY_UP:
							histentry = bbs_history_older();
							if (histentry) {
								bbs_dprintf(sysopfdout, "\r/%s", histentry);
							}
							break;
						case KEY_DOWN:
							histentry = bbs_history_newer();
							if (histentry) {
								bbs_dprintf(sysopfdout, "\r/%s", histentry);
							}
							break;
						case KEY_ESC:
							bbs_history_reset();
							histentry = NULL;
							break;
						default:
							/* Ignore */
							break;
					}
					break;
				case '\n':
					if (histentry) {
						bbs_dprintf(sysopfdout, "\n"); /* Print new line since we had history on the line */
						safe_strncpy(cmdbuf, histentry, sizeof(cmdbuf));
						bbs_history_add(cmdbuf);
						bbs_history_reset();
						histentry = NULL;
						my_set_stdout_logging(sysopfdout, 0); /* Disable logging so other stuff isn't trying to write to STDOUT at the same time. */
						bbs_buffer_input(sysopfdin, 1);
						res = sysop_command(console, cmdbuf);
						bbs_unbuffer_input(sysopfdin, 0);
						my_set_stdout_logging(sysopfdout, console->log); /* If running in foreground, re-enable STDOUT logging */
					} else {
						bbs_dprintf(sysopfdout, "\n"); /* Print newline for convenience */
					}
					break;
				case '/':
					bbs_dprintf(sysopfdout, "/");
					my_set_stdout_logging(sysopfdout, 0); /* Disable logging so other stuff isn't trying to write to STDOUT at the same time. */
					bbs_buffer_input(sysopfdin, 1);
					res = poll(pfds, console->remote ? 1 : 2, 300000);
					if (res < 0) {
						if (errno != EINTR) {
							bbs_error("poll returned %d: %s\n", res, strerror(errno));
						}
					} else if (res == 0) {
						bbs_dprintf(sysopfdout, "\nCommand expired\n");
					} else if (pfds[1].revents) {
						my_set_stdout_logging(sysopfdout, console->log);
						bbs_buffer_input(sysopfdin, 1);
						goto cleanup;
					} else {
						bytes_read = read(sysopfdin, cmdbuf, sizeof(cmdbuf) - 1);
						if (bytes_read <= 0) {
							bbs_debug(5, "read returned %ld\n", bytes_read);
						} else {
							cmdbuf[bytes_read] = '\0'; /* Safe, since size - 1 above */
							bbs_term_line(cmdbuf);
							/* Save in history */
							bbs_history_add(cmdbuf);
							res = sysop_command(console, cmdbuf);
						}
					}
					bbs_unbuffer_input(sysopfdin, 0);
					my_set_stdout_logging(sysopfdout, console->log); /* If running in foreground, re-enable STDOUT logging */
					break;
				default:
					if (isprint(buf[0])) {
						bbs_debug(5, "Received character %d (%c) on sysop console\n", buf[0], buf[0]);
					} else {
						bbs_debug(5, "Received character %d on sysop console\n", buf[0]);
					}
					bbs_dprintf(sysopfdout, "Invalid command '%c'. Press '?' for help.\n", isprint(buf[0]) ? buf[0] : ' ');
					break;
			}
		} else {
			if (!(pfds[0].revents & BBS_POLL_QUIT)) {
				bbs_error("poll returned %d, but no POLLIN?\n", res);
			}
			break;
		}
	}

cleanup:
	bbs_debug(2, "Sysop console (fd %d/%d) thread exiting\n", sysopfdin, sysopfdout);
	if (console->remote) {
		console_cleanup(console);
	}
	return NULL;
}

static int launch_sysop_console(int remote, int sfd, int fdin, int fdout)
{
	int res = 0;
	struct sysop_console *console;

	console = calloc(1, sizeof(*console));
	if (ALLOC_FAILURE(console)) {
		return -1;
	}

	console->sfd = sfd; /* Socket file descriptor */
	console->fdin = fdin; /* PTY */
	console->fdout = fdout;
	SET_BITFIELD(console->remote, remote);

	RWLIST_WRLOCK(&consoles);
	RWLIST_INSERT_TAIL(&consoles, console, entry);
	/* Note there is no SIGINT handler for remote consoles,
	 * so ^C will just exit the remote console without killing the BBS. */
	if (remote) {
		bbs_pthread_create_detached(&console->thread, NULL, sysop_handler, console);
	} else {
		bbs_pthread_create(&console->thread, NULL, sysop_handler, console);
	}
	if (res) {
		bbs_error("Failed to create %s sysop thread for %d/%d\n", remote ? "remote" : "foreground", fdin, fdout);
		RWLIST_REMOVE(&consoles, console, entry);
		free(console);
	}
	RWLIST_UNLOCK(&consoles);
	return res;
}

static int uds_socket = -1; /*!< UDS socket for allowing incoming local UNIX connections */
static pthread_t uds_thread;

static void *remote_sysop_listener(void *unused)
{
	struct sockaddr_un sunaddr;
	socklen_t len;
	int sfd;
	struct pollfd pfd;

	UNUSED(unused);

	pfd.fd = uds_socket;
	pfd.events = POLLIN;

	for (;;) {
		int aslave;
		int res = poll(&pfd, 1, -1); /* Wait forever for an incoming connection. */
		pthread_testcancel();
		if (res < 0) {
			if (errno != EINTR) {
				bbs_warning("poll returned error: %s\n", strerror(errno));
				break;
			}
			continue;
		}
		if (!pfd.revents) {
			continue; /* Shouldn't happen? */
		}
		if (unloading) {
			break;
		}
		len = sizeof(sunaddr);
		sfd = accept(uds_socket, (struct sockaddr *) &sunaddr, &len);
		if (sfd < 0) {
			if (errno != EINTR) {
				bbs_debug(1, "accept returned %d: %s\n", sfd, strerror(errno));
				break;
			}
			continue;
		}
		bbs_verb(4, "Accepting new remote sysop connection\n");
		/* Now, we need to create a pseudoterminal for the UNIX socket, the sysop thread needs a PTY. */
		aslave = bbs_spawn_pty_master(sfd);
		if (aslave == -1) {
			close(sfd);
			continue;
		}
		bbs_unbuffer_input(aslave, 0); /* Disable canonical mode and echo on this PTY slave */
		bbs_dprintf(aslave, TERM_CLEAR); /* Clear the screen on connect */
		launch_sysop_console(1, sfd, aslave, aslave); /* Launch sysop console for this connection */
	}
	return NULL;
}

static int cli_consoles(struct bbs_cli_args *a)
{
	struct sysop_console *console;

	bbs_dprintf(a->fdout, "%1s %5s %5s %4s %3s %s\n", "R", "FD IN", "FD OUT", "Dead", "Log", "Thread");
	RWLIST_RDLOCK(&consoles);
	RWLIST_TRAVERSE(&consoles, console, entry) {
		bbs_dprintf(a->fdout, "%1s %5d %5d %4s %3s %16lu\n", console->remote ? "*" : "", console->fdin, console->fdout, BBS_YN(console->dead), BBS_YN(console->log), console->thread);
	}
	RWLIST_UNLOCK(&consoles);

	return 0;
}

static struct bbs_cli_entry cli_commands_sysop[] = {
	BBS_CLI_COMMAND(cli_consoles, "consoles", 1, "List all sysop console sessions", NULL),
	/* General */
	BBS_CLI_COMMAND(cli_testemail, "testemail", 1, "Send test email to sysop", NULL),
	BBS_CLI_COMMAND(cli_mtrim, "mtrim", 1, "Manually release free memory at the top of the heap", NULL),
	BBS_CLI_COMMAND(cli_assert, "assert", 1, "Manually trigger an assertion (WARNING: May abort BBS)", NULL),
	BBS_CLI_COMMAND(cli_copyright, "copyright", 1, "Show copyright notice", NULL),
	BBS_CLI_COMMAND(cli_license, "license", 1, "Show license notice", NULL),
	BBS_CLI_COMMAND(cli_warranty, "warranty", 1, "Show warranty notice", NULL),
};

#define BBS_SYSOP_SOCKET DIRCAT(DIRCAT("/var/run", BBS_NAME), "sysop.sock")

static int unload_module(void)
{
	struct sysop_console *console;

	bbs_cli_unregister_multiple(cli_commands_sysop);
	unloading = 1;
	bbs_alertpipe_write(console_alertpipe);

	if (uds_socket != -1) {
		bbs_socket_thread_shutdown(&uds_socket, uds_thread);
		unlink(BBS_SYSOP_SOCKET);
	}

	/* Close all the consoles. */
	RWLIST_RDLOCK(&consoles);
	RWLIST_TRAVERSE_SAFE_BEGIN(&consoles, console, entry) {
		bbs_debug(3, "Instructing %s sysop console %d/%d to exit\n", console->remote ? "remote" : "foreground", console->fdin, console->fdout);
		console->dead = 1;
		if (console->remote) {
			bbs_remove_logging_fd(console->fdout); /* Must do before bbs_socket_close since that sets fd to -1 */
			/* Should cause the console thread to exit */
			bbs_socket_close(&console->fdout);
			bbs_socket_close(&console->fdin);
			bbs_socket_close(&console->sfd);
		} else {
			RWLIST_REMOVE_CURRENT(entry);
			bbs_pthread_join(console->thread, NULL);
			free(console);
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&consoles);

	bbs_alertpipe_read(console_alertpipe);
	bbs_alertpipe_close(console_alertpipe);

	/* This is not pretty, but need to wait until the list is empty - console threads are detached so we have nothing to join,
	 * and we can't make them non-detached because console threads can exit on their own, without anyone to join them. */
	for (;;) {
		int remaining = 0;
		bbs_debug(3, "Waiting for all sysop consoles to exit\n");
		RWLIST_RDLOCK(&consoles);
		RWLIST_TRAVERSE(&consoles, console, entry) {
			if (console->fdin == -1 && console->fdout == -1) {
				/* This means the remote console has been shut down, but its thread has not yet exited. */
				bbs_warning("Stale %s console still registered?\n", console->remote ? "remote" : "foreground");
			}
			bbs_debug(3, "%s console %d/%d is still registered\n", console->remote ? "Remote" : "Foreground", console->fdin, console->fdout);
			remaining++;
		}
		RWLIST_UNLOCK(&consoles);
		if (!remaining) {
			break;
		}
		usleep(100000);
	}

	return 0;
}

static int show_copyright_fg(void)
{
	show_copyright(STDOUT_FILENO, 1);
	return 0;
}

static int load_module(void)
{
	if (bbs_alertpipe_create(console_alertpipe)) {
		return -1;
	}
	if (option_nofork) {
		launch_sysop_console(0, STDIN_FILENO, STDIN_FILENO, STDOUT_FILENO);
	} else {
		bbs_debug(3, "BBS not started with foreground console, declining to load foreground sysop console\n");
	}

#pragma GCC diagnostic ignored "-Wsign-conversion"
	/* Start a thread to allow remote sysop console connections */
	if (bbs_make_unix_socket(&uds_socket, BBS_SYSOP_SOCKET, "0600", -1, -1) || bbs_pthread_create(&uds_thread, NULL, remote_sysop_listener, NULL)) {
		if (!option_nofork) {
			/* Nothing major to clean up, we didn't create a foreground console, and the remote handler failed */
			return -1; /* Only fatal if daemonized, since otherwise there would be no sysop consoles at all */
		}
	}
#pragma GCC diagnostic pop

	if (!bbs_is_fully_started() && option_nofork) {
		bbs_register_startup_callback(show_copyright_fg, STARTUP_PRIORITY_DEFAULT);
	}

	bbs_cli_register_multiple(cli_commands_sysop);
	return 0;
}

BBS_MODULE_INFO_STANDARD("Sysop Console");
