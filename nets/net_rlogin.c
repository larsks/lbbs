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
 * \brief RLogin network driver
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#include "include/bbs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h> /* use sockaddr_in */
#include <pthread.h>

#include "include/module.h"
#include "include/node.h"
#include "include/utils.h"
#include "include/config.h"
#include "include/net.h"

static int rlogin_socket = -1; /*!< TCP Socket for allowing incoming network connections */
static pthread_t rlogin_thread;

/*! \brief Default RLogin port is 513 */
#define DEFAULT_RLOGIN_PORT 513

static int rlogin_port = DEFAULT_RLOGIN_PORT;

#define TIOCPKT_WINDOW 0x80

static int send_urgent(int fd)
{
	ssize_t res;
	/* The source of rlogind: https://fossies.org/linux/inetutils/src/rlogind.c
	 * was helpful in figuring out how to properly send the urgent TCP data.
	 * XXX BUGBUG This doesn't work properly yet, but the above source provides some clues.
	 */
	unsigned char oobdata[] = { TIOCPKT_WINDOW };

	/* The MSG_OOB flag to send is what makes this urgent. */
	res = send(fd, oobdata, 1, MSG_OOB);
	if (res < 0) {
		bbs_error("send: %s\n", strerror(errno));
	} else if (res == 1) {
		bbs_debug(5, "Sent urgent TCP data: %x\n", oobdata[0]);
	}
	return res == 1 ? 0 : -1;
}

static int rlogin_handshake(struct bbs_node *node)
{
	int i;
	ssize_t res;
	char buf[128];
	unsigned char buf2[128];
	int on = 1;
	char *s1, *s2, *s3, *s4;

	/* Client sends 4 NUL-terminated bytes.
	 * <null>
	 * client-user-name<null>
	 * server-user-name<null>
	 * terminal-type/speed<null>
	 */
	res = bbs_poll_read(node->fd, SEC_MS(30), buf, sizeof(buf) - 1); /* Session might be interactive, so give enough time for the user to enter credentials */
	if (res <= 0) {
		bbs_warning("Didn't receive connection string\n");
		return -1;
	}
	buf[res] = '\0'; /* Safe - just in case we didn't read a NUL */
	i = bbs_strncount(buf, (size_t) res, '\0');
	if (i != 4) {
		bbs_debug(3, "Got %ld-byte connection string with %d NULs?\n", res, i);
		return -1;
	}
	s1 = buf;
	s2 = s1 + strlen(s1) + 1;
	s3 = s2 + strlen(s2) + 1;
	s4 = s3 + strlen(s3) + 1;
	bbs_debug(3, "Got %ld-byte connection string (%s/%s/%s/%s)\n", res, s1, s2, s3, s4);
	if (SWRITE(node->fd, "\0") != STRLEN("\0")) { /* Send 0-byte to ACK and change to data transfer mode */
		return -1;
	}

	/* XXX Even when done before bind, seems to have no effect */
	/* RFC 6093 SO_OOBINLINE */
	if (setsockopt(node->fd, SOL_SOCKET, SO_OOBINLINE, &on, sizeof(on))) {
		bbs_warning("Failed to set option OOBINLINE\n");
	}

	/* RFC 1282 Get the window size
	 * Server sends 0x80 to get current window size.
	 * This is a single-byte control message. Sent as data, but with TCP "urgent data" pointer pointing to the control byte.
	 * See https://man7.org/linux/man-pages/man7/tcp.7.html
	 * Client responds with 12-byte window change control sequence:
	 * FF FF s s rr cc xp yp
	 */
	if (send_urgent(node->fd)) {
		return -1;
	}

	/*! \todo BUGBUG The window change control stuff is currently broken,
	 * so this currently always fails.
	 * Probably we're not sending the TCP urgent data properly. Dunno. */

	res = bbs_poll_read(node->fd, SEC_MS(2), (char*) buf2, sizeof(buf2) - 1);
	if (res <= 0) {
		bbs_warning("Failed to receive window change control sequence\n");
		/* Just continue */
	} else if (res >= 12) {
		if (buf2[0] == 0xFF && buf2[1] == 0xFF) {
			bbs_debug(3, "Got window change control sequence\n");
			/*! \todo FIXME, parse and then call bbs_node_update_winsize once we actually get the control sequence and can test that */
		}
		/* Problem is now window change control sequence is enabled, and unlike telnet,
		 * we can't shut it off again. Ouch.
		 * Let's hope users don't resize their terminals during the session
		 */
	}

	return 0;
}

static void *rlogin_listener(void *unused)
{
	UNUSED(unused);
	bbs_tcp_comm_listener(rlogin_socket, "RLogin", rlogin_handshake, BBS_MODULE_SELF);
	return NULL;
}

static int load_config(void)
{
	struct bbs_config *cfg = bbs_config_load("net_rlogin.conf", 0);

	if (!cfg) {
		/* Assume defaults if we failed to load the config (e.g. file doesn't exist). */
		return 0;
	}

	rlogin_port = DEFAULT_RLOGIN_PORT;
	bbs_config_val_set_port(cfg, "rlogin", "port", &rlogin_port);

	return 0;
}

static int load_module(void)
{
	if (load_config()) {
		return -1;
	}
	/* If we can't start the TCP listener, decline to load */
	if (bbs_make_tcp_socket(&rlogin_socket, rlogin_port)) {
		return -1;
	}
	bbs_assert(rlogin_socket >= 0);
	if (bbs_pthread_create(&rlogin_thread, NULL, rlogin_listener, NULL)) {
		close(rlogin_socket);
		rlogin_socket = -1;
		return -1;
	}
	bbs_register_network_protocol("RLogin", (unsigned int) rlogin_port);
	return 0;
}

static int unload_module(void)
{
	if (rlogin_socket > -1) {
		bbs_unregister_network_protocol((unsigned int) rlogin_port);
		bbs_socket_thread_shutdown(&rlogin_socket, rlogin_thread);
	} else {
		bbs_error("RLogin socket already closed at unload?\n");
	}
	return 0;
}

BBS_MODULE_INFO_STANDARD("RFC1282 RLogin");
