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
 * \brief Internet Relay Chat (IRC) Server
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#include "include/bbs.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>

#include "include/tls.h"

#include "include/module.h"
#include "include/config.h"
#include "include/net.h"
#include "include/utils.h"
#include "include/node.h"
#include "include/auth.h"
#include "include/user.h"
#include "include/stringlist.h"
#include "include/base64.h"

#define DEFAULT_IRC_PORT 6667
#define DEFAULT_IRCS_PORT 6697

/* Allow this module to use dprintf */
#undef dprintf

#define IRC_SERVER_VERSION BBS_NAME "-" BBS_VERSION "-irc"
#define BBS_SOURCE_URL "https://github.com/InterLinked1/lbbs"

/*! \brief Clients will be pinged every 2 minutes, and have 2 minutes to respond. */
#define PING_TIME MIN_MS(2)

#define MAX_TOPIC_LENGTH 390
#define MAX_CHANNEL_LENGTH 50
#define MAX_AWAY_LEN 90
#define MAX_CHANNELS 50

#define MAX_NICKLEN 16
#define MAX_HOSTLEN 128
/*! \todo not yet supported */
#define DEF_MAXLIST "b:1"

/* Hostmask stuff */
#define IDENT_PREFIX_FMT "%s!~%s@%s"
#define IDENT_PREFIX_ARGS(user) user->nickname, user->username, user->hostname

#define send_reply(user, fmt, ...) bbs_debug(3, "%p <= " fmt, user, ## __VA_ARGS__); pthread_mutex_lock(&user->lock); dprintf(user->wfd, fmt, ## __VA_ARGS__); pthread_mutex_unlock(&user->lock);
#define send_numeric(user, numeric, fmt, ...) send_reply(user, "%03d %s :" fmt, numeric, user->nickname, ## __VA_ARGS__)
#define send_numeric2(user, numeric, fmt, ...) send_reply(user, "%03d %s " fmt, numeric, user->nickname, ## __VA_ARGS__)
#define send_numeric_broadcast(channel, user, numeric, fmt, ...) channel_broadcast(channel, user, "%03d %s " fmt, numeric, bbs_hostname(), ## __VA_ARGS__)

/*! \note Currently this is a single-server network, so there is no difference in practice between # and & channels. */
/*! \todo Make this IRC network daemon multi-server capable somehow? Perhaps linkable with other servers running the same IRC module? Would also allow sharing state... */
#define IS_CHANNEL_NAME(s) (*s == '#' || *s == '&')
#define VALID_CHANNEL_NAME(s) (!strlen_zero(s) && IS_CHANNEL_NAME(s))

/*! \todo implement ChanServ, to help with persistence of channel data across restarts as well as auto-opping, etc. */

/*! \todo include irc.h from LIRC, so we can use macro names for numerics, at least */
/*! \todo Make MOTD more dynamic? Perhaps read from a file? */
/*! \todo add guest support (low priority, since it obviously won't work if SASL is globally required... and it's easy to register anyways) */
/*! \todo add IRC integration with the BBS, e.g. for a BBS user's friends, ping 'em on IRC on login (for nodes with a PTY only, obviously) */

static int irc_port = DEFAULT_IRC_PORT;
static int ircs_port = DEFAULT_IRCS_PORT;

static pthread_t irc_listener_thread = -1;
static pthread_t irc_ping_thread = -1;

static int irc_enabled = 1, ircs_enabled = 1;
static int irc_socket = -1, ircs_socket = -1;
static int require_sasl = 1;
static int log_channels = 0;

#define PUBLIC_CHANNEL_PREFIX "="
#define PRIVATE_CHANNEL_PREFIX "*"
#define SECRET_CHANNEL_PREFIX "@"

/* ChatZilla/Ambassador interface guide: http://chatzilla.hacksrus.com/intro */
#define PREFIX_FOUNDER "~"
#define PREFIX_ADMIN "&"
#define PREFIX_OP "@"
#define PREFIX_HALFOP "%"
#define PREFIX_VOICE "+"

/* Reference for numeric message strings: https://github.com/solanum-ircd/solanum/blob/main/include/messages.h */
/* Reference for channel modes: https://github.com/solanum-ircd/solanum/blob/main/help/opers/cmode */

static const char *usermodes = "ioZ";
static const char *channelmodes = "jlmnprstzS";
static const char *paramchannelmodes = "qahov";
/* https://modern.ircdocs.horse/#mode-message */
static const char *chanmodes = ",,jl,mnprstzS"; /* I think this is the correct categorization into the A,B,C,D modes... */

/*! \brief Channel modes (apply to all users) */
enum channel_modes {
	CHANNEL_MODE_NONE =					0,
	/*! \todo add invite only (i) */
	/*! \todo implement throttled */
	CHANNEL_MODE_THROTTLED =			(1 << 0), /* j<n:t>: Channel is throttled. Only n users may join each t seconds. */
	CHANNEL_MODE_LIMIT =				(1 << 1), /* l<max>: Channel capacity limited to max. */
	CHANNEL_MODE_MODERATED =			(1 << 2), /* m: Channel moderated: only opped and voiced users can send */
	CHANNEL_MODE_NO_EXTERNAL =			(1 << 3), /* n: No external messages */
	/* See https://www.irchelp.org/misc/ccosmos.html#sec3-5-3 for difference between private and secret.
	 * This implementation makes a distinction between the two, unlike many servers nowadays which treat them identically.
	 * Secret does everything private does, but is even more secret.
	 */
	CHANNEL_MODE_PRIVATE =				(1 << 4), /* p: Private channel: membership is private outside of the channel, but channel shows up in lists */
	CHANNEL_MODE_REGISTERED_ONLY =		(1 << 5), /* r: Registered users only */
	CHANNEL_MODE_SECRET =				(1 << 6), /* s: Secret channel: membership and listing are private */
	CHANNEL_MODE_TOPIC_PROTECTED =		(1 << 7), /* t: Topic protected: only half ops or above can change the topic */
	CHANNEL_MODE_REDUCED_MODERATION =	(1 << 8), /* z: Reduced moderation. Normally blocked messages will be sent to half operators and above. */
	CHANNEL_MODE_TLS_ONLY =				(1 << 9), /* S: Only users connected via TLS may join */
};

/*! \brief Channel "hidden" from queries unless the user is also in it */
#define CHANNEL_HIDDEN (CHANNEL_MODE_PRIVATE | CHANNEL_MODE_SECRET)

/*! \brief Channel modes that apply to users (on a per-user basis) */
enum channel_user_modes {
	CHANNEL_USER_MODE_NONE =	0,
	/* Note that founder and admin don't confer any of the privileges of operator.
	 * Therefore, in most cases, you'll want to assign founder and op (or admin an op),
	 * if you want to assign these top 2 privileges.
	 * Think of these as "enhancements" to op, rather than inherently higher privilege levels,
	 * much the same way Flash Override in AUTOVON is not technically its own priority level.
	 */
	CHANNEL_USER_MODE_FOUNDER =	(1 << 0), /* q: Founder: total and complete control */
	CHANNEL_USER_MODE_ADMIN =	(1 << 1), /* a: Admin/Protected: can only be demoted by founders */
	CHANNEL_USER_MODE_HALFOP =	(1 << 2), /* h: Half operator: can kick users, set most channel modes, grant voice */
	CHANNEL_USER_MODE_OP =		(1 << 3), /* o: Operator: is an op */
	CHANNEL_USER_MODE_VOICE =	(1 << 4), /* v: Voice: has voice */
};

/*! \brief User modes */
enum user_modes {
	USER_MODE_NONE =		0,
	USER_MODE_INVISIBLE =	(1 << 0), /* i: User hidden from global WHO */
	USER_MODE_OPERATOR =	(1 << 1), /* o: Global server operator */
	USER_MODE_SECURE =		(1 << 2), /* Z: Connected via SSL/TLS */
};

/*! \brief A single IRC user */
struct irc_user {
	struct bbs_node *node;			/* Node that is handling this user. 1:1 mapping. */
	int channelcount;				/* Number of channels currently in, for constant-time count access */
	char *username;					/* Client username. Does not change. */
	char *nickname;					/* Client nickname. Can change. */
	char *realname;					/* "Real name", typically the client name */
	char *hostname;					/* Hostname: defaults to IP, but can use a host mask or "cloak" instead */
	enum user_modes modes;			/* User's modes (apply to the user globally, not just a specific channel) */
	int rfd;						/* Read file descriptor */
	int wfd;						/* Write file descriptor */
	int joined;						/* Time joined */
	int lastactive;					/* Time of last JOIN, PART, PRIVMSG, NOTICE, etc. */
	int lastping;					/* Last ping sent */
	int lastpong;					/* Last pong received */
	pthread_mutex_t lock;			/* User lock */
	char *awaymsg;					/* Away message */
	unsigned int away:1;			/* User is currently away (default is 0, i.e. user is here) */
	RWLIST_ENTRY(irc_user) entry;	/* Next user */
	/* Avoid using a flexible struct member since we'll probably strdup both the username and nickname beforehand anyways */
};

static RWLIST_HEAD_STATIC(users, irc_user);	/* Container for all users */

/*! \brief A user in a channel (1:1) */
struct irc_member {
	struct irc_user *user;			/* Reference to user (must be in the users list) */
	enum channel_user_modes modes;	/* User's channel flags (flags for this channel) */
	pthread_mutex_t lock;			/* Member lock */
	RWLIST_ENTRY(irc_member) entry;	/* Next member */
};

RWLIST_HEAD(channel_members, irc_member);

struct irc_channel {
	const char *name;					/* Name of channel */
	unsigned int membercount;			/* Current member count, for constant-time member count access */
	char *topic;						/* Channel topic */
	char *topicsetby;					/* Ident of who set the channel topic */
	unsigned int topicsettime;			/* Epoch time of when the topic was last set */
	struct channel_members members;		/* List of users currently in this channel */
	enum channel_modes modes;			/* Channel modes (non-user specific) */
	unsigned int limit;					/* Limit on number of users in channel (only enforced on joins) */
	FILE *fp;							/* Optional log file to which to log all channel activity */
	RWLIST_ENTRY(irc_channel) entry;	/* Next channel */
	char data[0];						/* Flexible struct member for channel name */
};

static RWLIST_HEAD_STATIC(channels, irc_channel);	/* Container for all channels */

static int authorized_atleast(struct irc_member *member, int atleast)
{
	int auth = 0;

	pthread_mutex_lock(&member->lock);
	switch (atleast) {
		case CHANNEL_USER_MODE_VOICE:
			auth |= member->modes & CHANNEL_USER_MODE_VOICE;
			/* Fall through */
		case CHANNEL_USER_MODE_HALFOP:
			auth |= member->modes & CHANNEL_USER_MODE_HALFOP;
			/* Fall through */
		case CHANNEL_USER_MODE_OP:
			auth |= member->modes & CHANNEL_USER_MODE_OP;
			/* Fall through */
		case CHANNEL_USER_MODE_ADMIN:
			auth |= member->modes & CHANNEL_USER_MODE_ADMIN;
			/* Fall through */
		case CHANNEL_USER_MODE_FOUNDER:
			auth |= member->modes & CHANNEL_USER_MODE_FOUNDER;
			/* Fall through */
		default:
			break;
	}
	pthread_mutex_unlock(&member->lock);

	return auth;
}

#define APPEND_MODE(buf, len, modes, mode, letter) if (modes & mode && (len-- >= 1)) { buf[pos++] = letter; }

static int get_channel_user_modes(char *buf, size_t len, struct irc_member *member)
{
	int pos = 0;

	pthread_mutex_lock(&member->lock);
	if (!member->modes) {
		pthread_mutex_unlock(&member->lock);
		buf[0] = '\0';
		return -1;
	}
	buf[pos++] = '+';
	APPEND_MODE(buf, len, member->modes, CHANNEL_USER_MODE_ADMIN, 'a');
	APPEND_MODE(buf, len, member->modes, CHANNEL_USER_MODE_HALFOP, 'h');
	APPEND_MODE(buf, len, member->modes, CHANNEL_USER_MODE_OP, 'o');
	APPEND_MODE(buf, len, member->modes, CHANNEL_USER_MODE_FOUNDER, 'q');
	APPEND_MODE(buf, len, member->modes, CHANNEL_USER_MODE_VOICE, 'v');
	pthread_mutex_unlock(&member->lock);
	buf[pos] = '\0';
	return 0;
}

static void get_channel_modes(char *buf, size_t len, struct irc_channel *channel)
{
	int pos = 0;
	if (!channel->modes) {
		buf[0] = '\0';
		return;
	}
	buf[pos++] = '+';
	/* Capitals come before lowercase */
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_TLS_ONLY, 'S');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_THROTTLED, 'j');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_LIMIT, 'l');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_MODERATED, 'm');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_NO_EXTERNAL, 'n');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_PRIVATE, 'p');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_REGISTERED_ONLY, 'r');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_SECRET, 's');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_TOPIC_PROTECTED, 't');
	APPEND_MODE(buf, len, channel->modes, CHANNEL_MODE_REDUCED_MODERATION, 'z');
	buf[pos] = '\0';
}

static int get_user_modes(char *buf, size_t len, struct irc_user *user)
{
	int pos = 0;

	pthread_mutex_lock(&user->lock);
	if (!user->modes) {
		pthread_mutex_unlock(&user->lock);
		buf[0] = '\0';
		return -1;
	}
	buf[pos++] = '+';
	APPEND_MODE(buf, len, user->modes, USER_MODE_INVISIBLE, 'i');
	APPEND_MODE(buf, len, user->modes, USER_MODE_OPERATOR, 'o');
	APPEND_MODE(buf, len, user->modes, USER_MODE_SECURE, 'Z');
	pthread_mutex_unlock(&user->lock);
	buf[pos] = '\0';
	return 0;
}

static const char *top_channel_membership_prefix(struct irc_member *member)
{
	/* https://modern.ircdocs.horse/#channel-membership-prefixes */
	if (member->modes & CHANNEL_USER_MODE_FOUNDER) {
		return PREFIX_FOUNDER;
	} else if (member->modes & CHANNEL_USER_MODE_ADMIN) {
		return PREFIX_ADMIN;
	} else if (member->modes & CHANNEL_USER_MODE_OP) {
		return PREFIX_OP;
	} else if (member->modes & CHANNEL_USER_MODE_HALFOP) {
		return PREFIX_HALFOP;
	} else if (member->modes & CHANNEL_USER_MODE_VOICE) {
		return PREFIX_VOICE;
	}
	return "";
}

static void user_free(struct irc_user *user)
{
	pthread_mutex_destroy(&user->lock);
	free_if(user->hostname);
	free_if(user->awaymsg);
	free_if(user->realname);
	free_if(user->username);
	free_if(user->nickname);
	free(user);
}

static void unlink_user(struct irc_user *user)
{
	struct irc_user *u;
	RWLIST_WRLOCK(&users);
	RWLIST_TRAVERSE_SAFE_BEGIN(&users, u, entry) {
		if (u == user) {
			RWLIST_REMOVE_CURRENT(entry);
			/* Caller will free */
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&users);
	if (!u) {
		bbs_error("Didn't find user '%s' in list\n", S_IF(user->nickname));
	}
}

static struct irc_member *get_member(struct irc_user *user, struct irc_channel *channel)
{
	struct irc_member *member;
	RWLIST_RDLOCK(&channel->members);
	RWLIST_TRAVERSE(&channel->members, member, entry) {
		if (member->user == user) {
			break;
		}
	}
	RWLIST_UNLOCK(&channel->members);
	return member;
}

/*! \note This returns a user with no locks */
static struct irc_member *get_member_by_channel_name(struct irc_user *user, const char *channame)
{
	struct irc_channel *channel;
	struct irc_member *member;

	RWLIST_RDLOCK(&channels);
	RWLIST_TRAVERSE(&channels, channel, entry) {
		if (!strcmp(channel->name, channame)) {
			break;
		}
	}
	if (!channel) {
		RWLIST_UNLOCK(&channels);
		bbs_debug(3, "Channel '%s' doesn't exist\n", channame);
		return NULL;
	}
	RWLIST_TRAVERSE(&channel->members, member, entry) {
		if (member->user == user) {
			break;
		}
	}
	RWLIST_UNLOCK(&channels);

	return member;
}

/*! \note This returns a user with no locks */
static struct irc_user *get_user(const char *username)
{
	struct irc_user *user;

	RWLIST_RDLOCK(&users);
	RWLIST_TRAVERSE(&users, user, entry) {
		if (!strcmp(user->username, username)) {
			break;
		}
	}
	RWLIST_UNLOCK(&users);
	return user;
}

static struct irc_member *get_member_by_username(const char *username, const char *channame)
{
	struct irc_user *user = get_user(username);
	if (!user) {
		return NULL;
	}
	return get_member_by_channel_name(user, channame);
}

/*! \note This returns a channel with no locks */
static struct irc_channel *get_channel(const char *channame)
{
	struct irc_channel *channel;

	RWLIST_RDLOCK(&channels);
	RWLIST_TRAVERSE(&channels, channel, entry) {
		if (!strcmp(channel->name, channame)) {
			break;
		}
	}
	RWLIST_UNLOCK(&channels);
	return channel;
}

static int valid_channame(const char *s)
{
	int i = 0;
	while (*s) {
		if (!isalnum(*s) && !(!i && (*s == '#' || *s == '&')) && *s != '-') {
			bbs_debug(3, "Character %d is not valid\n", *s);
			return 0;
		}
		s++;
		i = 1;
	}
	return 1;
}

static void channel_free(struct irc_channel *channel)
{
	bbs_assert(channel->membercount == 0);
	if (channel->fp) {
		fclose(channel->fp);
		channel->fp = NULL;
	}
	free_if(channel->topicsetby);
	free_if(channel->topic);
	free(channel);
}

static void destroy_channels(void)
{
	struct irc_channel *channel;
	struct irc_member *member;

	RWLIST_WRLOCK(&channels);
	while ((channel = RWLIST_REMOVE_HEAD(&channels, entry))) {
		RWLIST_WRLOCK(&channel->members); /* Kick any members still present */
		while ((member = RWLIST_REMOVE_HEAD(&channel->members, entry))) {
			channel->membercount -= 1;
			free(member);
		}
		RWLIST_UNLOCK(&channel->members);
		channel_free(channel);
	}
	RWLIST_UNLOCK(&channels);
}

#define channel_broadcast(channel, user, fmt, ...) __channel_broadcast(1, channel, user, 0, fmt, ## __VA_ARGS__)
#define channel_broadcast_nolock(channel, user, fmt, ...) __channel_broadcast(0, channel, user, 0, fmt, ## __VA_ARGS__)
#define channel_broadcast_selective(channel, user, minmode, fmt, ...) __channel_broadcast(1, channel, user, minmode, fmt, ## __VA_ARGS__)

/*!
 * \brief Send a message to everyone (or almost everyone) in a channel
 * \param channel Channel to which to broadcast
 * \param user A user to which to NOT send the message (typically to prevent echoes of a user's own messages). NULL to really send to everyone.
 * \param fmt printf-style format string
 * \retval 0 on success, -1 on failure
 */
static int __attribute__ ((format (gnu_printf, 5, 6))) __channel_broadcast(int lock, struct irc_channel *channel, struct irc_user *user, enum channel_user_modes minmode, const char *fmt, ...)
{
	struct irc_member *member;
	char *buf;
	int len;
	int sent = 0, skipped = 0;
	va_list ap;

	va_start(ap, fmt);
	len = vasprintf(&buf, fmt, ap);
	va_end(ap);

	if (len < 0) {
		bbs_error("vasprintf failure\n");
		return -1;
	}

	if (lock) {
		RWLIST_RDLOCK(&channel->members);
	}
	RWLIST_TRAVERSE(&channel->members, member, entry) {
		if (user && user == member->user) {
			skipped++;
			continue; /* Skip */
		}
		if (minmode && !authorized_atleast(member, minmode)) {
			skipped++;
			continue; /* Skip those who don't have at least a certain privilege (e.g. for moderating messages only to ops) */
		}
		/* Careful here... we want member->user, not user */
		pthread_mutex_lock(&member->user->lock); /* Serialize writes to this user */
		write(member->user->wfd, buf, len); /* Use write instead of dprintf, because we already have the length, and it's just a simple string now */
		pthread_mutex_unlock(&member->user->lock);
		sent++;
	}
	if (lock) {
		RWLIST_UNLOCK(&channel->members);
	}
	bbs_debug(5, "(%d/%d) <= %s", sent, skipped, buf); /* Log it just once, not for every user to whom we send it. Message ends in CR LF, so don't add one here. */
	if (channel->fp) {
		time_t lognow;
		struct tm logdate;
		char datestr[20];
		/* Calculate our current timestamp, for logging sanity */
		lognow = time(NULL);
		localtime_r(&lognow, &logdate);
		strftime(datestr, sizeof(datestr), "%Y-%m-%d %T", &logdate);
		fprintf(channel->fp, "[%s] %s", datestr, buf); /* Assume it ends in CR LF (it better!) */
	}
	/* It's possible to send to 0 users only if there's only one user in the channel and user is non NULL (don't echo to sender) */
	if (!sent && !user) {
		bbs_warning("Message was broadcast to 0 users in channel %s?\n", channel->name); /* Probably a bug */
	}
	free(buf);
	return 0;
}

static void user_setactive(struct irc_user *user)
{
	pthread_mutex_lock(&user->lock);
	user->lastactive = time(NULL);
	pthread_mutex_unlock(&user->lock);
}

static int privmsg(struct irc_user *user, const char *channame, int notice, const char *message)
{
	struct irc_channel *channel;
	struct irc_member *m;
	enum channel_user_modes minmode = CHANNEL_USER_MODE_NONE;

	user_setactive(user);

	/*! \todo need to respond with appropriate numerics here */
	if (strlen_zero(message)) {
		send_numeric(user, 412, "No text to send\r\n");
		return -1;
	}

	if (strlen(message) >= 510) { /* Include CR LF */
		send_numeric(user, 416, "Input too large\r\n"); /* XXX Not really the right numeric */
		return -1;
	}

	/* It's not our job to filter messages, clients can do that. For example, decimal 1 is legitimate for CTCP commands. */

	if (!IS_CHANNEL_NAME(channame)) {
		struct irc_user *user2 = get_user(channame);
		/* Private message to another user. This is super simple, there's no other overhead or anything involved. */
		if (!user2) {
			send_numeric2(user, 401, "%s :No such nick/channel\r\n", channame);
			return -1;
		}
		pthread_mutex_lock(&user2->lock); /* Serialize writes to this user */
		dprintf(user2->wfd, ":" IDENT_PREFIX_FMT " %s %s :%s\r\n", IDENT_PREFIX_ARGS(user), notice ? "NOTICE" : "PRIVMSG", user2->nickname, message);
		pthread_mutex_unlock(&user2->lock);
		if (user2->away) {
			send_numeric(user, 301, "%s\r\n", S_IF(user2->awaymsg));
		}
		return 0;
	}

	/*! \todo simplify using get_channel, get_member? But then we may have more locking issues... */
	RWLIST_RDLOCK(&channels);
	RWLIST_TRAVERSE(&channels, channel, entry) {
		if (!strcmp(channel->name, channame)) {
			break;
		}
	}
	if (!channel) {
		RWLIST_UNLOCK(&channels);
		send_numeric2(user, 403, "%s :No such channel\r\n", channame);
		return -1;
	}

	/* Check if we're actually in the channel */
	RWLIST_RDLOCK(&channel->members);
	RWLIST_TRAVERSE(&channel->members, m, entry) {
		if (m->user == user) {
			break;
		}
	}
	RWLIST_UNLOCK(&channel->members);
	RWLIST_UNLOCK(&channels);

	if (!m && channel->modes & CHANNEL_MODE_NO_EXTERNAL) {
		send_numeric(user, 442, "You're not on that channel\r\n");
		return -1;
	}

	if (channel->modes & CHANNEL_MODE_MODERATED && !authorized_atleast(m, CHANNEL_USER_MODE_VOICE)) {
		if (channel->modes & CHANNEL_MODE_REDUCED_MODERATION) {
			minmode = CHANNEL_USER_MODE_HALFOP;
		} else {
			send_numeric(user, 489, "You're neither voiced nor a channel operator\r\n"); /* Channel moderated, unable to send */
			return -1;
		}
	}

	/*! \todo By default, don't echo messages to ourself, but could if enabled: https://ircv3.net/specs/extensions/echo-message */
	channel_broadcast_selective(channel, user, minmode, ":" IDENT_PREFIX_FMT " %s %s :%s\r\n", IDENT_PREFIX_ARGS(user), notice ? "NOTICE" : "PRIVMSG", channel->name, message);
	return 0;
}

static int print_channel_mode(struct irc_user *user, struct irc_channel *channel)
{
	char chanmode[16];
	if (!channel) {
		send_numeric2(user, 403, "%s :No such channel\r\n", ""); /* Whoops */
		return -1;
	}
	get_channel_modes(chanmode, sizeof(chanmode), channel);
	if (user) {
		send_reply(user, ":%s MODE %s %s\r\n", bbs_hostname(), channel->name, chanmode);
	} else {
		channel_broadcast(channel, user, ":%s MODE %s %s\r\n", bbs_hostname(), channel->name, chanmode);
	}
	return 0;
}

#if 0
static int print_member_mode(struct irc_member *member, struct irc_channel *channel)
{
	char chanmode[16];
	if (!channel || !member) {
		return -1;
	}
	get_channel_user_modes(chanmode, sizeof(chanmode), member);
	channel_broadcast(channel, NULL, ":%s MODE %s %s %s\r\n", bbs_hostname(), channel->name, chanmode, member->user->nickname);
	return 0;
}
#endif

static int print_user_mode(struct irc_user *user)
{
	char usermode[16];
	if (!user) {
		send_numeric2(user, 401, "%s :No such nick/channel\r\n", ""); /* Whoops */
		return -1;
	}
	get_user_modes(usermode, sizeof(usermode), user);
	send_reply(user, ":%s MODE %s :%s\r\n", user->nickname, user->nickname, usermode);
	return 0;
}

#define SET_MODE(modes, set, mode) \
	if (set && !(modes & mode)) { \
		bbs_debug(6, "Set mode %s\n", #mode); \
		modes |= mode; \
		changed++; \
	} else if (!set && modes & mode) { \
		bbs_debug(6, "Cleared mode %s\n", #mode); \
		modes &= ~mode; \
		changed++; \
	} else { \
		bbs_debug(6, "Not %sting mode %s (no change)\n", set ? "set" : "unset", #mode); \
	}

#define MIN_MODE(member, mode, str) \
	if (!authorized_atleast(member, mode)) { \
		send_numeric2(user, 482, "%s: You're not a channel %s\r\n", channel->name, str); \
		continue; \
	}

static void handle_modes(struct irc_user *user, char *s)
{
	struct irc_member *member = NULL, *targetmember = NULL;
	struct irc_channel *channel = NULL;

	/* Stuff like this:
	 * MODE #channel
	 * MODE #channel +S
	 * MODE #channel +o jsmith
	 */
	char *modes, *channel_name = strsep(&s, " ");
	channel = get_channel(channel_name);
	modes = strsep(&s, " "); /* If there's anything left, it's the usernames to target for a channel mode */
	/* Unless there's a : */
	if (modes && *modes == ':') {
		modes++;
	}
	if (!modes) {
		/* Just get the modes for a channel */
		if (IS_CHANNEL_NAME(channel_name)) {
			print_channel_mode(user, channel); /* NULL OK */
		} else {
			/* XXX Apparently you can't view other users' modes? If it turns out this is fine, then we can just call get_user and call print_user_mode on that. */
			if (strcmp(channel_name, user->username)) {
				send_numeric(user, 502, "Can't change mode for other users\r\n");
				return;
			}
			print_user_mode(user); /* Print our mode */
		}
	} else {
		char *target;
		char mode;
		int set;
		int changed = 0;
		target = s; /* Anything left is the target, e.g. user to op */
		bbs_debug(5, "Modes: '%s'\n", modes);
		if (*modes == '+') {
			set = 1;
		} else if (*modes == '-') {
			set = 0;
		} else { /* Not + or - ? */
			send_numeric(user, 501, "Unknown MODE flag\r\n");
			return;
		}
		bbs_debug(3, "User %p requested %s modes for %s: %s\n", user, set ? "set" : "unset", target, channel_name);
		/*
		 * User modes: /mode jsmith +i => MODE jsmith +i
		 * Channel modes: /mode #test +S => MODE #test +S
		 * Channel modes for users: /mode #test +o jsmith => MODE #test +o jsmith
		 *
		 * List channel modes: /mode #test => MODE #test
		 */

		/* Find the member for this channel */
		if (IS_CHANNEL_NAME(channel_name)) {
			member = get_member_by_channel_name(user, channel_name);
			if (!member || !authorized_atleast(member, CHANNEL_USER_MODE_OP)) { /* Must be at least an op */
				send_numeric2(user, 482, "%s: You're not a channel operator\r\n", channel_name);
				return;
			}
			if (!channel) {
				send_numeric2(user, 403, "%s :No such channel\r\n", channel_name);
				return;
			}
		} else if (strcmp(user->nickname, channel_name)) {
			send_numeric(user, 502, "Can't change mode for other users\r\n");
			return;
		}
		if (target) {
			targetmember = get_member_by_username(target, channel_name);
		}
		for (modes++; *modes; modes++) { /* Skip the + or - to start */
			mode = *modes;
			bbs_debug(5, "Requesting %s mode %c for %s (%s)\n", set ? "set" : "unset", mode, target, S_IF(channel_name));
			if (IS_CHANNEL_NAME(channel_name)) { /* Channel, and it's a channel operator */
				switch (mode) {
					case 'S':
						SET_MODE(channel->modes, set, CHANNEL_MODE_TLS_ONLY);
						break;
					case 'q':
					case 'a':
						MIN_MODE(member, CHANNEL_USER_MODE_FOUNDER, "founder"); /* Only founders can change 'a' (whereas ops can deop other ops) */
						/* Fall through */
					case 'o':
					case 'h':
					case 'v':
						if (!target) {
							send_numeric(user, 461, "Not enough parameters\r\n");
							continue;
						}
						if (!targetmember) {
							send_numeric(user, 441, "They aren't on that channel\r\n");
							continue;
						}
						/* This is written out the long way instead of using the ternary operator so that #mode will print what we want in the macro */
						pthread_mutex_lock(&targetmember->lock);
						if (mode == 'q') {
							SET_MODE(targetmember->modes, set, CHANNEL_USER_MODE_FOUNDER);
						} else if (mode == 'a') {
							SET_MODE(targetmember->modes, set, CHANNEL_USER_MODE_ADMIN);
						} else if (mode == 'o') {
							SET_MODE(targetmember->modes, set, CHANNEL_USER_MODE_OP);
						} else if (mode == 'h') {
							SET_MODE(targetmember->modes, set, CHANNEL_USER_MODE_HALFOP);
						} else if (mode == 'v') {
							SET_MODE(targetmember->modes, set, CHANNEL_USER_MODE_VOICE);
						}
						pthread_mutex_unlock(&targetmember->lock);
						if (changed) {
							channel_broadcast(channel, NULL, ":%s MODE %s %c%c %s\r\n", user->nickname, channel->name, set ? '+' : '-', mode, targetmember->user->nickname);
						}
						break;
					case 'l':
						if (set && strlen_zero(target)) {
							send_numeric(user, 461, "Not enough parameters\r\n");
							continue;
						}
						SET_MODE(channel->modes, set, CHANNEL_MODE_LIMIT);
						if (set) {
							channel->limit = atoi(target); /* If this fails, the limit will be 0 (turned off), so not super dangerous... */
						} else {
							channel->limit = 0;
						}
						break;
					case 'm':
						SET_MODE(channel->modes, set, CHANNEL_MODE_MODERATED);
						break;
					case 'n':
						SET_MODE(channel->modes, set, CHANNEL_MODE_NO_EXTERNAL);
						break;
					case 'p':
						SET_MODE(channel->modes, set, CHANNEL_MODE_PRIVATE);
						break;
					case 'r':
						SET_MODE(channel->modes, set, CHANNEL_MODE_REGISTERED_ONLY);
						break;
					case 's':
						SET_MODE(channel->modes, set, CHANNEL_MODE_SECRET);
						break;
					case 't':
						SET_MODE(channel->modes, set, CHANNEL_MODE_TOPIC_PROTECTED);
						break;
					case 'z':
						SET_MODE(channel->modes, set, CHANNEL_MODE_REDUCED_MODERATION);
						break;
					case 'j': /* Throttled */
						/*! \todo Not implemented yet */
					default:
						bbs_warning("Unknown channel mode '%c'\n", isprint(mode) ? mode : ' ');
						send_numeric2(user, 472, "%c :is an unknown mode char to me\r\n", mode);
				}
				if (!target && changed) {
					channel_broadcast(channel, NULL, ":%s MODE %s %c%c\r\n", user->nickname, channel->name, set ? '+' : '-', mode);
				}
			} else { /* Same user */
				switch (mode) {
					case 'i':
						SET_MODE(user->modes, set, USER_MODE_INVISIBLE);
						break;
					case 'o': /* Channel operator */
						if (user->node->user->id == 1) { /* Allow the sysop to become a server operator */
							SET_MODE(user->modes, set, USER_MODE_OPERATOR);
						} else {
							send_numeric(user, 491, "No appropriate operator blocks were found for your host\r\n");
						}
						break;
					case 'Z': /* Valid mode but is read only */
					default:
						bbs_warning("Unknown user mode '%c'\n", isprint(mode) ? mode : ' ');
						send_numeric2(user, 472, "%c :is an unknown mode char to me\r\n", mode);
				}
				if (changed) {
					send_reply(user, ":%s MODE %s %c%c\r\n", user->nickname, user->nickname, set ? '+' : '-', mode);
				}
			}
		}
	}
}

static void channel_print_topic(struct irc_user *user, struct irc_channel *channel)
{
	if (channel->topic) {
		if (!user) { /* Broadcast (topic change) */
			send_numeric_broadcast(channel, NULL, 332, "%s :%s\r\n", channel->name, S_IF(channel->topic));
			send_numeric_broadcast(channel, user, 333, "%s %s %d\r\n", channel->name, channel->topicsetby, channel->topicsettime);
		} else {
			send_numeric2(user, 332, "%s :%s\r\n", channel->name, S_IF(channel->topic));
			send_numeric2(user, 333, "%s %s %d\r\n", channel->name, channel->topicsetby, channel->topicsettime);
		}
	} else {
		if (!user) {
			send_numeric_broadcast(channel, user, 331, "%s :No topic is set\r\n", channel->name);
		} else {
			send_numeric2(user, 331, "%s :No topic is set\r\n", channel->name);
		}
	}
}

/*! \todo this isn't locking safe */
static void handle_topic(struct irc_user *user, char *s)
{
	struct irc_channel *channel;
	char *channame = strsep(&s, " ");
	if (s && *s == ':') {
		s++;
	}

	channel = get_channel(channame);
	if (!channel) {
		send_numeric2(user, 403, "%s :No such channel\r\n", s);
	} else if (!s) { /* Print current channel topic */
		channel_print_topic(user, channel);
	} else {
		struct irc_member *m;
		if (strlen(s) > MAX_TOPIC_LENGTH) {
			send_numeric(user, 416, "Topic is too long\r\n"); /* XXX Not really the right numeric */
			return;
		}
		m = get_member_by_username(user->nickname, channel->name);
		if (!m || (channel->modes & CHANNEL_MODE_TOPIC_PROTECTED && !authorized_atleast(m, CHANNEL_USER_MODE_HALFOP))) { /* Need at least half op to set the topic, if protected. */
			send_numeric(user, 482, "You're not a channel operator\r\n");
		} else {
			char buf[128];
			free_if(channel->topic);
			free_if(channel->topicsetby);
			channel->topic = strdup(s);
			snprintf(buf, sizeof(buf),IDENT_PREFIX_FMT, IDENT_PREFIX_ARGS(user));
			channel->topicsetby = strdup(buf);
			channel->topicsettime = time(NULL);
			channel_print_topic(NULL, channel);
		}
	}
}

static void dump_who(struct irc_user *user, struct irc_user *whouser)
{
	const char *chan = "*"; /* https://modern.ircdocs.horse/#rplwhoreply-352 */
	int hopcount = 0;
	char userflags[3];

	pthread_mutex_lock(&whouser->lock);
	snprintf(userflags, sizeof(userflags), "%c%s", whouser->away ? 'G' : 'H', whouser->modes & USER_MODE_OPERATOR ? "*" : "");
	pthread_mutex_unlock(&whouser->lock);

	send_numeric(user, 352, "%s %s %s %s %s %s :%d %s\r\n", chan, whouser->username, whouser->hostname, bbs_hostname(), whouser->nickname, userflags, hopcount, whouser->realname);
}

/*! \brief Whether two users share any IRC channels in common */
static int channels_in_common(struct irc_user *u1, struct irc_user *u2)
{
	UNUSED(u1);
	UNUSED(u2);
	return 1; /*! \todo implement */
}

static void handle_who(struct irc_user *user, char *s)
{
	if (IS_CHANNEL_NAME(s)) {
		struct irc_member *member;
		struct irc_channel *channel = get_channel(s);
		if (!channel) {
			send_numeric2(user, 403, "%s :No such channel\r\n", s);
			return;
		}
		RWLIST_RDLOCK(&channel->members);
		RWLIST_TRAVERSE(&channel->members, member, entry) {
			if (member->user->modes & USER_MODE_INVISIBLE && !channels_in_common(member->user, user)) {
				continue;
			}
			dump_who(user, member->user);
		}
		RWLIST_UNLOCK(&channel->members);
	} else {
		struct irc_user *whouser = get_user(s);
		dump_who(user, whouser);
	}
	send_numeric(user, 315, "%s: End of WHO list\r\n", s);
}

/*!
 * \brief Assuming channel is a hidden channel, whether this channel should be considered off-limits to a user query
 * \retval 1 if user is not in the channel, 0 if the channel is not hidden or the user is in the channel
 */
static int suppress_channel(struct irc_user *user, struct irc_channel *channel)
{
	struct irc_member *m;
	if (!(channel->modes & CHANNEL_HIDDEN)) {
		return 0; /* It's not private or secret */
	}
	m = get_member(user, channel);
	if (!m) {
		return 1; /* Skip: requesting user isn't in this channel */
	}
	return 0;
}

static void handle_whois(struct irc_user *user, char *s)
{
	int now;
	char buf[256];
	int len = 0;
	char umodes[15];
	struct irc_channel *channel;
	struct irc_member *member;
	struct irc_user *u = get_user(s);
	if (!u) {
		send_numeric2(user, 401, "%s :No such nick/channel\r\n", s);
		return;
	}

	now = time(NULL);
	get_user_modes(umodes, sizeof(umodes), u);

	send_numeric2(user, 307, "%s :has identified for this nick\r\n", u->nickname); /* Everyone has, and nicks can't be changed, so... */
	send_numeric2(user, 311, "%s %s %s * :%s\r\n", u->nickname, u->username, u->hostname, u->realname);
	send_numeric2(user, 312, "%s %s :%s\r\n", u->nickname, bbs_hostname(), "Root IRC Server");
	if (user->modes & USER_MODE_OPERATOR) {
		send_numeric2(user, 313, "%s :is an IRC operator\r\n", u->nickname);
	}

	/* Channel memberships */
	RWLIST_RDLOCK(&channels);
	RWLIST_TRAVERSE(&channels, channel, entry) {
		if (channel->modes & CHANNEL_HIDDEN && suppress_channel(user, channel)) {
			continue;
		}
		RWLIST_RDLOCK(&channel->members);
		RWLIST_TRAVERSE(&channel->members, member, entry) {
			if (member->user == u) {
				if (member->user->modes & USER_MODE_INVISIBLE) {
					/* Include channels only if user is in them too (show only shared channels) */
					if (!get_member(user, channel)) {
						continue;
					}
				}
				len += snprintf(buf + len, sizeof(buf) - len, "%s%s%s", len ? " " : "", top_channel_membership_prefix(member), channel->name);
				if (len >= 200) {
					send_numeric2(user, 319, "%s :%s\r\n", u->nickname, buf);
					len = 0;
				}
			}
		}
		RWLIST_UNLOCK(&channel->members);
	}
	RWLIST_UNLOCK(&channels);
	if (len > 0) {
		send_numeric2(user, 319, "%s :%s\r\n", u->nickname, buf);
	}

	if (u->modes) {
		send_numeric2(user, 379, "%s :is using modes %s\r\n", u->nickname, umodes);
	}
	send_numeric2(user, 317, "%s %d %d :seconds idle, signon time\r\n", u->nickname, now - u->lastactive, u->joined);
	if (user->modes & USER_MODE_SECURE) {
		send_numeric2(user, 671, "%s :is using a secure connection\r\n", u->nickname);
	}
	send_numeric2(user, 318, "%s :End of /WHOIS list\r\n", s); /* case must be preserved, so use s instead of u->nickname */
}

static void handle_userhost(struct irc_user *user, char *s)
{
	char buf[256];
	struct irc_user *u = get_user(s);
	if (!u) {
		send_numeric2(user, 401, "%s :No such nick/channel\r\n", s);
		return;
	}
	pthread_mutex_lock(&u->lock);
	snprintf(buf, sizeof(buf), "%s %s = %c %s %s", u->nickname, u->modes & USER_MODE_OPERATOR ? "*" : "", u->away ? '-' : '+', S_IF(u->awaymsg), u->hostname);
	pthread_mutex_unlock(&u->lock);
	send_numeric(user, 302, "%s\r\n", buf);
}

static void handle_list(struct irc_user *user, char *s)
{
	struct irc_channel *channel;
	unsigned int minmembers = 0, maxmembers = 0;
	unsigned int mintopicage = 0, maxtopicage = 0;
	unsigned int now = time(NULL);
	char *elistcond, *conds;

	conds = s;
	while ((elistcond = strsep(&conds, ","))) {
		if (strlen_zero(elistcond) || strlen_zero(elistcond + 1)) {
			continue;
		}
		switch (*elistcond) {
			/* These are not inclusive */
			case '>':
				minmembers = atoi(elistcond + 1);
				break;
			case '<':
				maxmembers = atoi(elistcond + 1);
				break;
			case 'T':
				elistcond++;
				if (*elistcond == '<' && !strlen_zero(elistcond + 1)) {
					maxtopicage = atoi(elistcond + 1);
				} else if (*elistcond == '>' && !strlen_zero(elistcond + 1)) {
					mintopicage = atoi(elistcond + 1);
				}
				break;
			default:
				bbs_warning("Unhandled ELIST condition: %s\n", elistcond);
				break;
		}
	}

	send_numeric2(user, 321, "Channel :Users Name\r\n");
	RWLIST_RDLOCK(&channels);
	RWLIST_TRAVERSE(&channels, channel, entry) {
		/* Remember, the conditions are NOT inclusive. If they are equal, in other words, that is not a match, skip. */
		if (minmembers && channel->membercount <= minmembers) {
			continue;
		} else if (maxmembers && channel->membercount >= maxmembers) {
			continue;
		} else if (mintopicage && channel->topicsettime && channel->topicsettime >= now - maxtopicage) {
			continue; /* Topic too old */
		} else if (maxtopicage && channel->topicsettime && channel->topicsettime <= now - maxtopicage) {
			continue; /* Topic too new */
		}
		if (channel->modes & CHANNEL_MODE_SECRET && suppress_channel(user, channel)) {
			continue;
		}
		send_numeric2(user, 322, "%s %d :%s\r\n", channel->name, channel->membercount, S_IF(channel->topic));
	}
	RWLIST_UNLOCK(&channels);
	send_numeric(user, 323, "End of /LIST\r\n");
}

static void handle_help(struct irc_user *user, char *s)
{
	if (s && *s == ':') {
		s++;
	}
	if (strlen_zero(s)) {
		send_numeric(user, 704, "index * :** Help System **\r\n");
		/*! \todo add handlers and dynamically generate this? */
		send_numeric(user, 705, "index AWAY HELP INVITE JOIN KICK LIST MOTD NAMES NOTICE PART PING PONG PRIVMSG QUIT TOPIC USERHOST WHO WHOIS\r\n");
		send_numeric(user, 706, "index :End of /HELP\r\n");
		return;
	}
	/*! \todo add individual command help here */
	send_numeric(user, 524, "I don't know anything about that\r\n");
}

static int send_channel_members(struct irc_user *user, struct irc_channel *channel)
{
	struct irc_member *member;
	char buf[513];
	int len = 0;
	const char *symbol = PUBLIC_CHANNEL_PREFIX; /* Public channel */

	RWLIST_RDLOCK(&channel->members);
	RWLIST_TRAVERSE(&channel->members, member, entry) {
		len += snprintf(buf + len, sizeof(buf) - len, "%s%s%s", len ? " " : "", top_channel_membership_prefix(member), member->user->nickname);
		if (len >= 400) { /* Stop well short of the 512 character message limit and clear the buffer */
			len = 0;
			send_numeric2(user, 353, "%s %s :%s\r\n", symbol, channel->name, buf);
		}
	}
	RWLIST_UNLOCK(&channel->members);
	if (len > 0) { /* Last one */
		send_numeric2(user, 353, "%s %s :%s\r\n", symbol, channel->name, buf);
	}
	send_numeric2(user, 366, "%s :End of /NAMES list.\r\n", channel->name);
	return 0;
}

static int join_channel(struct irc_user *user, const char *name)
{
	struct irc_channel *channel;
	struct irc_member *member, *m;
	int newchan = 0;
	char modestr[16];
	int chanlen = strlen(name);

	/* Nip junk right in the bud before we even bother locking the list */
	if (!VALID_CHANNEL_NAME(name) || chanlen > MAX_CHANNEL_LENGTH || !valid_channame(name)) {
		send_numeric(user, 479, "Illegal channel name\r\n");
		return 0;
	}

	if (user->channelcount > MAX_CHANNELS) {
		send_numeric2(user, 405, "%s :You have joined too many channels\r\n", name);
		return 0;
	}

	/* We might potentially create a channel, so grab a WRLOCK from the get go */
	RWLIST_WRLOCK(&channels);
	RWLIST_TRAVERSE(&channels, channel, entry) {
		if (!strcmp(channel->name, name)) {
			break;
		}
	}
	if (!channel) {
		bbs_debug(3, "Creating channel '%s' for the first time\n", name);
		newchan = 1;
		channel = calloc(1, sizeof(*channel) + chanlen + 1);
		if (!channel) {
			RWLIST_UNLOCK(&channels);
			return -1;
		}
		strcpy(channel->data, name); /* Safe */
		channel->name = channel->data;
		channel->modes = CHANNEL_MODE_NONE;
		/* Set some default flags. */
		channel->modes |= CHANNEL_MODE_NO_EXTERNAL | CHANNEL_MODE_TOPIC_PROTECTED;
		if (bbs_user_is_registered(user->node->user)) {
			channel->modes |= CHANNEL_MODE_REGISTERED_ONLY;
		}
		channel->fp = NULL;
		if (log_channels) {
			char logfile[256];
			snprintf(logfile, sizeof(logfile), "%s/irc_channel_%s.txt", BBS_LOG_DIR, name);
			channel->fp = fopen(logfile, "a"); /* Append to existing file if it already exists */
			if (!channel->fp) {
				bbs_error("Failed to open log file %s: %s\n", logfile, strerror(errno));
				/* Just continue, what can ya do? */
			}
		}
		RWLIST_INSERT_HEAD(&channels, channel, entry);
	} else {
		if (channel->modes & CHANNEL_MODE_TLS_ONLY && !(user->modes & USER_MODE_SECURE)) {
			RWLIST_UNLOCK(&channels);
			/* Channel requires secure connections, but user isn't using one. Reject. */
			send_numeric(user, 477, "Cannot join channel (+S) - you need to use a secure connection\r\n"); /* XXX This is not the right numeric code, what is? */
			return -1;
		}
		if (channel->modes & CHANNEL_MODE_REGISTERED_ONLY && !bbs_user_is_registered(user->node->user)) {
			RWLIST_UNLOCK(&channels);
			send_numeric(user, 477, "Cannot join channel (+r) - you need to be logged into your account\r\n");
			return -1;
		}
		if (channel->modes & CHANNEL_MODE_LIMIT && channel->limit && channel->membercount >= channel->limit) {
			RWLIST_UNLOCK(&channels);
			send_numeric(user, 471, "Cannot join channel (+l) - channel is full, try again later\r\n");
			return -1;
		}
	}

	/* Check if we're already in the channel */
	RWLIST_WRLOCK(&channel->members);
	RWLIST_TRAVERSE(&channel->members, m, entry) {
		if (m->user == user) {
			break;
		}
	}
	if (m) {
		send_numeric(user, 714, "You're already on that channel\r\n");
		RWLIST_UNLOCK(&channel->members);
		RWLIST_UNLOCK(&channels);
		return -1;
	}

	/* Add ourself to the channel members */
	member = calloc(1, sizeof(*member));
	if (!member) {
		RWLIST_UNLOCK(&channel->members);
		if (newchan) {
			channel_free(channel); /* If we just created a new channel but couldn't join it, destroy it, since it has no members. Not yet in the list, so just free directly. */
		}
		RWLIST_UNLOCK(&channels);
		return -1; /* Well this is embarassing, we got this far... but we couldn't make it to the finish line */
	}
	member->user = user;
	member->modes = CHANNEL_USER_MODE_NONE;
	if (newchan) {
		member->modes |= CHANNEL_USER_MODE_OP; /* If you created it, you're the op. */
		if (user->node->user->id == 1) {
			/* OP still needs to be granted to founders (as we do above), higher prefixes don't implicitly grant lower ones.
			 * For example, Ambassador won't let you perform op operations unless you're an op. */
			member->modes |= CHANNEL_USER_MODE_FOUNDER; /* Automatically make the sysop a founder of any channel s/he creates */
		}
	}
	RWLIST_INSERT_HEAD(&channel->members, member, entry);
	channel->membercount += 1;
	user->channelcount += 1;
	RWLIST_UNLOCK(&channel->members);
	RWLIST_UNLOCK(&channels);

	user_setactive(user);

	/* These MUST be in this order: https://modern.ircdocs.horse/#join-message */
	channel_broadcast(channel, NULL, ":" IDENT_PREFIX_FMT " JOIN %s\r\n", IDENT_PREFIX_ARGS(user), channel->name); /* Send join message to everyone, including us */
	/* Don't send the mode now, because the client will just send a MODE command on its own anyways regardless */
	if (channel->topic) {
		channel_print_topic(user, channel);
	}
	send_channel_members(user, channel);
	if (!get_channel_user_modes(modestr, sizeof(modestr), member)) {
		channel_broadcast(channel, NULL, ":%s MODE %s %s %s\r\n", "ChanServ", channel->name, modestr, user->nickname);
	}
	return 0;
}

/*! \brief Must be called with WRLOCK on channels */
static int remove_channel(struct irc_channel *channel)
{
	struct irc_channel *c;
	RWLIST_TRAVERSE_SAFE_BEGIN(&channels, c, entry) {
		if (c == channel) {
			RWLIST_REMOVE_CURRENT(entry);
			bbs_debug(3, "Channel %s is now empty, removing\n", channel->name);
			channel_free(channel);
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	return c ? 0 : -1;
}

static int leave_channel(struct irc_user *user, const char *name)
{
	struct irc_channel *channel;
	struct irc_member *member;

	user_setactive(user);

	/* WRLOCK, since channel might become empty and need to be removed */
	RWLIST_WRLOCK(&channels);
	RWLIST_TRAVERSE(&channels, channel, entry) {
		if (!strcmp(channel->name, name)) {
			break;
		}
	}
	if (!channel) { /* Channel doesn't exist */
		RWLIST_UNLOCK(&channels);
		send_numeric2(user, 403, "%s :No such channel\r\n", name);
		return -1;
	}
	RWLIST_WRLOCK(&channel->members);
	RWLIST_TRAVERSE_SAFE_BEGIN(&channel->members, member, entry) {
		if (member->user == user) {
			channel_broadcast_nolock(channel, NULL, ":" IDENT_PREFIX_FMT " PART %s\r\n", IDENT_PREFIX_ARGS(user), channel->name); /* Make sure leaver gets his/her own PART message! */
			RWLIST_REMOVE_CURRENT(entry);
			channel->membercount -= 1;
			member->user->channelcount -= 1;
			free(member);
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&channel->members);
	if (RWLIST_EMPTY(&channel->members)) {
		remove_channel(channel);
	}
	RWLIST_UNLOCK(&channels);
	/* member is not a valid reference now, we just care that it was a reference */
	if (!member) { /* User doesn't exist in this channel */
		send_numeric(user, 442, "You're not on that channel\r\n");
		return -1;
	}
	return 0;
}

static void drop_member_if_present(struct irc_channel *channel, struct irc_user *user, const char *message)
{
	struct irc_member *member;

	/* If we're going to remove the user, we need a WRLOCK, so grab it from the get go. */
	RWLIST_WRLOCK(&channel->members);
	RWLIST_TRAVERSE_SAFE_BEGIN(&channel->members, member, entry) {
		if (member->user == user) {
			/* If we're leaving ALL channels, don't relay QUIT messages to ourselves. */
			bbs_debug(3, "Dropping user %s from channel %s\n", user->nickname, channel->name);
			RWLIST_REMOVE_CURRENT(entry);
			channel->membercount -= 1;
			member->user->channelcount -= 1;
			free(member);
			/* Already locked, so don't try to recursively lock: */
			channel_broadcast_nolock(channel, user, ":" IDENT_PREFIX_FMT " QUIT %s :%s\r\n", IDENT_PREFIX_ARGS(user), channel->name, S_IF(message));
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&channel->members);
	if (RWLIST_EMPTY(&channel->members)) {
		remove_channel(channel);
	}
}

static void kick_member(struct irc_channel *channel, struct irc_user *kicker, struct irc_user *kicked, const char *message)
{
	struct irc_member *member;

	/* If we're going to remove the user, we need a WRLOCK, so grab it from the get go. */
	RWLIST_WRLOCK(&channel->members);
	RWLIST_TRAVERSE_SAFE_BEGIN(&channel->members, member, entry) {
		if (member->user == kicked) {
			/* If we're leaving ALL channels, don't relay QUIT messages to ourselves. */
			bbs_debug(3, "Dropping user %s from channel %s\n", kicked->nickname, channel->name);
			RWLIST_REMOVE_CURRENT(entry);
			channel->membercount -= 1;
			free(member);
			/* Already locked, so don't try to recursively lock: */
			channel_broadcast_nolock(channel, NULL, ":" IDENT_PREFIX_FMT " KICK %s %s :%s\r\n", IDENT_PREFIX_ARGS(kicker), channel->name, kicked->nickname, S_IF(message));
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&channel->members);
	if (RWLIST_EMPTY(&channel->members)) {
		remove_channel(channel);
	}
}

static void leave_all_channels(struct irc_user *user, const char *message)
{
	struct irc_channel *channel;

	/* Remove from all channels the user is currently in, and broadcast a message to each of them.
	 * Because we might remove a user if this is the last user in the channel, we need a WRLOCK. */
	RWLIST_WRLOCK(&channels);
	/* We're going to have to traverse channels to find channels anyways,
	 * so simply traversing them all and seeing if the user is a member of each
	 * isn't as bad when you think about it that way. */
	RWLIST_TRAVERSE_SAFE_BEGIN(&channels, channel, entry) { /* We must use a safe traversal, since drop_member_if_present could cause the channel to be removed if it's now empty */
		drop_member_if_present(channel, user, message);
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&channels);
}

static int channel_count(void)
{
	int c;
	struct irc_channel *channel;
	RWLIST_RDLOCK(&channels);
	c = RWLIST_SIZE(&channels, channel, entry);
	RWLIST_UNLOCK(&channels);
	return c;
}

/*! \brief Message of the Day */
static void motd(struct irc_user *user)
{
	send_numeric(user, 375, "- %s Message of the Day -\r\n", bbs_hostname());
	/*! \todo Make this configurable or unique, more interesting in some way... */
	send_numeric(user, 372, "- This server powered by the Lightweight Bulletin Board System\r\n");
	send_numeric(user, 372, "- Visit us at %s\r\n", BBS_SOURCE_URL);
	send_numeric(user, 372, "- Welcome to %s chat\r\n", bbs_name());
	send_numeric(user, 376, "End of /MOTD command.\r\n");
}

static void hostmask(struct irc_user *user)
{
	char mask[32];
	/* Replace hostname with host mask, since nobody actually wants his or her location publicly shared */
	free_if(user->hostname);
	snprintf(mask, sizeof(mask), "node/%d", user->node->id);
	user->hostname = strdup(mask);
}

static int client_welcome(struct irc_user *user)
{
	char starttime[30];
	int count;
	int chancount;
	struct irc_user *u;
	char timebuf[30];

	bbs_time_friendly(bbs_starttime(), starttime, sizeof(starttime));

	RWLIST_WRLOCK(&users);
	RWLIST_TRAVERSE(&users, u, entry) {
		if (!strcasecmp(u->nickname, user->nickname)) {
			break;
		}
	}
	if (u) {
		send_numeric(user, 433, "Nickname is already in use\r\n");
		RWLIST_UNLOCK(&users);
		return -1;
	}

	hostmask(user); /* Cloak the user before adding to users list, so our IP doesn't leak on WHO/WHOIS */

	RWLIST_INSERT_HEAD(&users, user, entry);
	count = RWLIST_SIZE(&users, u, entry);
	RWLIST_UNLOCK(&users);

	send_numeric(user, 1, "Welcome to the %s Internet Relay Chat Network %s\r\n", bbs_name(), user->nickname);
	send_numeric(user, 2, "Your host is %s, running version %s\r\n", bbs_hostname(), IRC_SERVER_VERSION);
	send_numeric(user, 3, "This server was created %s\r\n", starttime);
	send_numeric2(user, 4, "%s %s %s %s %s\r\n", bbs_hostname(), IRC_SERVER_VERSION, usermodes, channelmodes, paramchannelmodes);
	/* We must explicitly advertise what prefixes we support or clients won't support them:
	 * https://modern.ircdocs.horse/#rplisupport-parameters
	 * https://defs.ircdocs.horse/defs/isupport.html
	 * https://defs.ircdocs.horse/defs/chanmembers.html
	 * http://www.irc.org/tech_docs/005.html
	 */
	/* MAX_CHANNELS applies to both # and &, but seems the spec doesn't allow providing a "combined" value for all channels? */
	send_numeric2(user, 5, "SAFELIST CHANTYPES=#& CHANMODES=%s CHANLIMIT=#:%d,&:%d :are supported by this server\r\n", chanmodes, MAX_CHANNELS, MAX_CHANNELS);
	send_numeric2(user, 5, "PREFIX=%s MAXLIST=%s MODES=26 CASEMAPPING=rfc1459 :are supported by this server\r\n", "(qaohv)~&@%+", DEF_MAXLIST); /* Ambassador ignores ascii for some reason but accepts rfc1459 */
	send_numeric2(user, 5, "NICKLEN=%d MAXNICKLEN=%d USERLEN=%d ELIST=TU AWAYLEN=%d CHANNELLEN=%d HOSTLEN=%d NETWORK=%s STATUSMSG=%s TOPICLEN=%d :are supported by this server\r\n",
		MAX_NICKLEN, MAX_NICKLEN, MAX_NICKLEN, MAX_AWAY_LEN, MAX_CHANNEL_LENGTH, MAX_HOSTLEN, bbs_name(), "&@%+", MAX_TOPIC_LENGTH);

	chancount = channel_count();

	send_numeric(user, 251, "There %s %d user%s on %d server%s\r\n", count == 1 ? "is" : "are", count, ESS(count), 1, ESS(1));
	send_numeric2(user, 254, "%d :channel%s formed\r\n", chancount, ESS(chancount));

	motd(user);

	if (bbs_user_is_registered(user->node->user) && user->node->user->lastlogin && strftime(timebuf, sizeof(timebuf), "%a %b %e %Y %I:%M %P %Z", user->node->user->lastlogin) > 0) { /* bbs_time_friendly does this internally */
		send_reply(user, "%s NOTICE %s :Last login was %s\r\n", bbs_hostname(), user->username, timebuf);
	}

	return 0;
}

static int do_sasl_auth(struct irc_user *user, char *s)
{
	int res, outlen;
	unsigned char *decoded;
	char *nickname, *username, *password;
	char *encoded;
	int runlen = 0;
	if (!STARTS_WITH(s, "AUTHENTICATE ")) {
		bbs_warning("Unhandled message: %s\n", s);
		return -1;
	}
	encoded = s + STRLEN("AUTHENTICATE ");
	/* AUTHENTICATE <BASE64(nick NUL username NUL password)> */
	decoded = base64_decode((unsigned char*) encoded, strlen(encoded), &outlen);
	/* If you were to dump decoded here using a printf-style function, you would just see the username, since the string is separated by NULs. We need the outlen. */
	if (!decoded) {
		return -1;
	}
	nickname = (char*) decoded;
	runlen += strlen(nickname) + 1;
	if (runlen >= outlen) {
		bbs_warning("No data after nickname?\n");
		free(decoded);
		return -1;
	}
	username = (char*) decoded + runlen;
	runlen += strlen(username) + 1;
	if (runlen >= outlen) {
		bbs_warning("No data after username?\n");
		free(decoded);
		return -1;
	}
	password = (char*) decoded + runlen;
	if (strcmp(nickname, user->nickname)) {
		bbs_warning("Nickname received '%s' does not match initial nick '%s'\n", nickname, user->nickname);
		free(decoded);
		return -1;
	}
	user->username = strdup(username);
	res = bbs_authenticate(user->node, username, password);
	memset(decoded, 0, outlen); /* Destroy the password from memory before we free it */
	free(decoded);
	if (res) {
		send_numeric(user, 904, "SASL authentication failed\r\n");
		return -1;
	}
	send_numeric(user, 903, "SASL authentication successful\r\n");
	/* The prefix is nick!ident@host */
	send_numeric(user, 900, IDENT_PREFIX_FMT " %s You are now logged in as %s\r\n", IDENT_PREFIX_ARGS(user), user->username, user->username);
	return 0;
}

static void handle_client(struct irc_user *user)
{
	int capnegotiate = 0;
	int res, started = 0;
	char buf[513];
	int mcount = 0;
	int sasl_auth = 0;
	int graceful_close = 0;

	for (;;) {
		char *s, *m = buf;
		res = bbs_fd_poll_read(user->rfd, PING_TIME, buf, sizeof(buf) - 1); /* Wait up to the ping interval time for something, anything, otherwise disconnect. */
		if (res <= 0) {
			/* Don't set graceful_close to 0 here, since after a QUIT, the client may close the connection first.
			 * The QUIT message should be whatever the client sent, since it was grateful, not connection closed by remote host. */
			break;
		}
		buf[res] = '\0'; /* Safe */
		/* Messages end in CR LF */
		if (res >= 2 && buf[res - 1] == '\n' && buf[res - 2] == '\r') {
			buf[res - 2] = '\0';
		} else if (buf[res - 1] == '\n') { /* No CR, but did get a LF at the end... okay, weird, but just go with it. */
			buf[res - 1] = '\0';
		} else {
			bbs_warning("Incomplete message from client: %s (ends in %d %d)\n", buf, res >= 2 ? buf[res - 2] : ' ', buf[res - 1]); /* XXX Now what? Continue reading? */
		}
		/* In practice, most IRC clients are nice and buffer any messages they sent to the server on connection,
		 * so dealing with a flood of messages at once that could be read all at once into the buffer isn't super likely.
		 * But it can happen, heck, lirc (the library used by door_irc) does this type of flooding for capability negotiation, first thing.
		 * Assume we could've gotten multiple complete messages, but the last one wasn't split between reads.
		 */
		while ((s = strsep(&m, "\r\n"))) {
			if (strlen_zero(s)) { /* For some reason, every other strsep we do returns an empty string? */
				continue;
			}
			mcount++;
			bbs_debug(8, "%p => %s\n", user, s); /* No trailing LF, so addding one here is fine */
			if (capnegotiate) {
				int sasl_failed = 0;
				/* XXX This is pretty rudimentary CAP support, it doesn't really support anything besides PLAIN SASL auth.
				 * It also doesn't fully account for all the possible scenarios allowed by the specs, only what's commonly done in practice. */
				if (capnegotiate == 1) {
					char *command = strsep(&s, " ");
					if (!s) {
						bbs_warning("No data after command %s\n", command);
						break; /* Just disconnect on the client */
					}
					/* Client will send a NICK, then USER: https://ircv3.net/specs/extensions/capability-negotiation.html */
					if (!strcasecmp(command, "NICK")) {
						user->nickname = strdup(s);
						bbs_debug(5, "Nickname is %s\n", user->nickname);
					} else if (!strcasecmp(command, "USER")) { /* Whole message is something like 'ambassador * * :New Now Know How' */
						char *realname;
						bbs_debug(5, "Username data is %s\n", s);
						realname = strsep(&s, " ");
						user->realname = strdup(realname);
						/* This is not actually user->username, ignore the message, but begin negotiating. */
						if (!user->nickname) {
							bbs_warning("Received USER without NICK?\n");
							break;
						}
						send_reply(user, "NOTICE AUTH :*** Processing connection to %s\r\n", bbs_hostname());
						send_reply(user, "NOTICE AUTH :*** Looking up your hostname...\r\n"); /* Not really, but everyone says this... */
						send_reply(user, "NOTICE AUTH :*** Checking Ident\r\n");
						send_reply(user, "NOTICE AUTH :*** No Ident response\r\n");
						send_reply(user, "NOTICE AUTH :*** Found your hostname: %s\r\n", user->node->ip);
						send_reply(user, "CAP * LS :multi-prefix sasl=PLAIN\r\n");
						/*! \todo We don't really support multi-prefix currently */
						capnegotiate++;
					} else {
						bbs_warning("Unhandled message: %s %s\n", command, s);
					}
				} else if (capnegotiate == 2) {
					if (!strcmp(s, "CAP REQ :multi-prefix")) {
						send_reply(user, "CAP * ACK :multi-prefix\r\n"); /* Colon technically optional, since there's only one capability */
						capnegotiate++;
					} else if (!strcmp(s, "CAP REQ :multi-prefix sasl")) {
						send_reply(user, "CAP * ACK :multi-prefix sasl\r\n");
						capnegotiate++;
					} else {
						bbs_warning("Unhandled message: %s\n", s);
					}
				} else if (capnegotiate == 3) {
					if (!strcmp(s, "AUTHENTICATE PLAIN")) {
						send_reply(user, "AUTHENTICATE +\r\n");
						capnegotiate++;
					} else {
						bbs_warning("Unhandled message: %s\n", s);
					}
				} else if (capnegotiate == 4) {
					capnegotiate++;
					sasl_failed = do_sasl_auth(user, s);
					if (!sasl_failed) {
						sasl_auth = 1;
					}
				} else if (capnegotiate == 5) {
					if (!strcmp(s, "CAP END")) {
						capnegotiate = 0; /* Done with CAP */
						bbs_debug(5, "Capability negotiation finished\n");
						if (!started) {
							if (!client_welcome(user)) {
								started = 1;
							}
						} else {
							bbs_error("Client %p already started?\n", user);
						}
					} else {
						bbs_warning("Unhandled message: %s\n", s);
					}
				} else {
					bbs_warning("Unhandled message: %s\n", s);
					send_numeric(user, 410, "Invalid CAP command\r\n");
					/* First message: Didn't start with CAP LS 302? Then client doesn't support SASL, just get going. */
				}
				if (capnegotiate == 5 && sasl_failed) {
					send_numeric(user, 906, "SASL authentication aborted\r\n");
				} else if (!started && !strcmp(s, "CAP END")) { /* CAP END can be sent at any time during capability negotiation */
					capnegotiate = 0; /* Done with CAP */
					bbs_debug(5, "Capability negotiation cancelled by client\n");
					if (!started) {
						if (!client_welcome(user)) {
							started = 1;
						}
					} else {
						bbs_error("Client %p already started?\n", user);
					}
				}
			} else if (!strcasecmp(s, "CAP LS 302")) {
				bbs_debug(5, "Client wants to negotiate\n"); /* Technically, a client could also just start with an unsolicited CAP REQ */
				capnegotiate = 1; /* Begin negotiation */
			} else { /* Post-CAP/SASL */
				char *current, *command = strsep(&s, " ");
				if (!strcasecmp(command, "PONG")) {
					pthread_mutex_lock(&user->lock);
					user->lastpong = time(NULL);
					pthread_mutex_unlock(&user->lock);
				} else if (!strcasecmp(command, "PING")) { /* Usually servers ping clients, but clients can ping servers too */
					send_reply(user, "PONG %s\r\n", S_IF(s)); /* Don't add another : because it's still in s, if present. */
				/* Any remaining commands require authentication.
				 * The nice thing about this IRC server is we authenticate using the BBS user,
				 * e.g. you don't create accounts using IRC, so we don't need to support guest access at all. */
				} else if (!sasl_auth && !bbs_user_is_registered(user->node->user) && require_sasl) {
					send_reply(user, "NOTICE AUTH :*** This server requires SASL for authentication. Please reconnect with SASL enabled.\r\n");
				/* We can't necessarily use %s (user->username) instead of %p (user), since if !require_sasl, we might not have a username still. */
				} else if (!started) {
					send_numeric(user, 451, "You have not registered\r\n");
				} else if (!strcasecmp(command, "PRIVMSG")) { /* List this as high up as possible, since this is the most common command */
					char *channel;
					/* Format for channel messages:
					 * PRIVMSG #channel :my message
					 */
					channel = strsep(&s, " ");
					if (channel) {
						if (*s == ':') {
							s++; /* Skip leading : */
						}
						privmsg(user, channel, 0, s);
					}
				} else if (!strcasecmp(command, "NOTICE")) { /* List this as high up as possible, since this is the most common command */
					char *channel;
					/* Format for channel messages:
					 * PRIVMSG #channel :my message
					 */
					channel = strsep(&s, " ");
					if (channel) {
						if (*s == ':') {
							s++; /* Skip leading : */
						}
						privmsg(user, channel, 1, s);
					}
				} else if (!strcasecmp(command, "MODE")) {
					handle_modes(user, s);
				} else if (!strcasecmp(command, "TOPIC")) { /* Get or set the topic */
					handle_topic(user, s);
				} else if (!strcasecmp(command, "JOIN")) {
					bbs_debug(3, "User %p wants to join channels: %s\n", user, s);
					rtrim(s); /* Not sure why this is necessary, but there's an extra space on the end it seems with Ambassador, at least. */
					while ((current = strsep(&s, ","))) {
						join_channel(user, current);
					}
				} else if (!strcasecmp(command, "PART")) {
					bbs_strterm(s, ':'); /* If there's a :, ignore anything after it */
					rtrim(s);
					bbs_debug(3, "User %p wants to leave channels: %s\n", user, s);
					while ((current = strsep(&s, ","))) {
						leave_channel(user, current);
					}
				} else if (!strcasecmp(command, "QUIT")) {
					bbs_debug(3, "User %p wants to quit: %s\n", user, S_IF(s));
					rtrim(s);
					leave_all_channels(user, s);
					graceful_close = 1; /* Defaults to 1 anyways, but this is definitely graceful */
					break; /* We're done. */
				} else if (!strcasecmp(command, "AWAY")) {
					if (!strlen_zero(s) && strlen(s) > MAX_AWAY_LEN) {
						send_numeric(user, 416, "Input too large\r\n"); /* XXX Not really the appropriate numeric */
						continue;
					}
					pthread_mutex_lock(&user->lock);
					free_if(user->awaymsg);
					if (!strlen_zero(s)) { /* Away */
						user->awaymsg = strdup(s);
						user->away = 1;
					} else { /* No longer away */
						user->away = 0;
					}
					pthread_mutex_unlock(&user->lock);
					send_numeric(user, user->away ? 306 : 305, "You %s marked as being away\r\n", user->away ? "have been" : "are no longer");
				} else if (!strcasecmp(command, "KICK")) {
					struct irc_member *member;
					char *reason, *kickusername, *channame = strsep(&s, " ");
					kickusername = strsep(&s, " ");
					reason = s;
					if (!kickusername) {
						send_numeric(user, 461, "Not enough parameters\r\n");
						continue;
					}
					/* KICK #channel jsmith :Reason for kicking user */
					member = get_member_by_channel_name(user, channame);
					if (!member || !authorized_atleast(member, CHANNEL_USER_MODE_HALFOP)) { /* Need at least half op to kick */
						send_numeric2(user, 482, "%s: You're not a channel operator\r\n", channame);
					} else {
						struct irc_member *kickuser;
						struct irc_channel *kickchan = get_channel(channame);
						if (!kickchan) {
							send_numeric2(user, 403, "%s :No such channel\r\n", channame);
							continue;
						}
						kickuser = get_member_by_username(kickusername, kickchan->name);
						if (!kickuser) {
							send_numeric2(user, 401, "%s :No such nick/channel\r\n", kickchan->name);
							continue;
						}
						kick_member(kickchan, user, kickuser->user, reason);
					}
				} else if (!strcasecmp(command, "INVITE")) {
					char *nick, *channame;
					struct irc_member *member;
					struct irc_channel *channel;
					struct irc_user *inviteduser;
					nick = strsep(&s, " ");
					channame = s;
					if (!nick || !channame) {
						send_numeric(user, 461, "Not enough parameters\r\n");
						continue;
					}
					channel = get_channel(channame);
					if (!channel) {
						send_numeric2(user, 403, "%s :No such channel\r\n", channame);
						continue;
					}
					member = get_member_by_channel_name(user, channame);
					if (!member) {
						send_numeric(user, 442, "You're not on that channel\r\n");
						continue;
					}
					member = get_member_by_username(nick, channame);
					if (member) {
						send_numeric2(user, 443, "%s %s :is already on channel\r\n", nick, channame);
						continue;
					}
					/*! \todo if invite only, then only channel ops can invite */
					inviteduser = get_user(nick);
					if (!inviteduser) {
						send_numeric2(user, 401, "%s :No such nick/channel\r\n", nick);
						continue;
					}
					send_reply(inviteduser, ":" IDENT_PREFIX_FMT " INVITE %s %s\r\n", IDENT_PREFIX_ARGS(user), inviteduser->nickname, channame);
					send_numeric2(user, 341, "%s %s\r\n", nick, channame); /* Confirm to inviter */
				} else if (!strcasecmp(command, "NAMES")) {
					struct irc_channel *channel = get_channel(s);
					/* Many servers don't allow NAMES unless you're in the channel: we do... */
					if (!channel) {
						send_numeric2(user, 403, "%s :No such channel\r\n", s);
						continue;
					}
					/* ...unless it's private/secret */
					if (channel->modes & CHANNEL_HIDDEN && suppress_channel(user, channel)) {
						send_numeric(user, 442, "You're not on that channel\r\n");
						continue;
					}
					send_channel_members(user, channel);
				} else if (!strcasecmp(command, "WHO")) {
					/* WHO username or WHO #channel, mask patterns not supported */
					handle_who(user, s);
				} else if (!strcasecmp(command, "WHOIS")) {
					handle_whois(user, s);
				} else if (!strcasecmp(command, "USERHOST")) {
					handle_userhost(user, s);
				} else if (!strcasecmp(command, "LIST")) {
					handle_list(user, s);
				} else if (!strcasecmp(command, "MOTD")) {
					motd(user);
				} else if (!strcasecmp(command, "HELP")) {
					handle_help(user, s);
				} else {
					/*! \todo add support for remaining missing commands, e.g. KNOCK */
					send_numeric2(user, 421, "%s :Unknown command\r\n", command);
					bbs_warning("%p: Unhandled message: %s %s\n", user, command, s);
				}
			}
		}
	}
	if (!graceful_close) {
		leave_all_channels(user, "Remote user closed the connection"); /* poll or read failed */
	}
	if (started) {
		unlink_user(user);
	}
}

/* The threading model here is pretty basic.
 * We have one thread per client.
 * Each of these threads will wait for activity from the client.
 * Messages are relayed to all participants in the channel when a client sends a message,
 * which is fine since we can read/write to sockets independently (with the appropriate locking, of course).
 * There are no separate threads for channels. The only other thread that exists is the periodic ping thread.
 *
 * In theory, we don't need one thread per client.
 * We could poll all the clients and handle activity as we get them, in a single thread.
 * However, the current node threading model expects a unique thread per node,
 * and admittedly this is simpler so that's how it is for now. But this is certainly a potential future improvement.
 *
 * TL;DR This IRC server uses N+1 threads, where N is the number of clients connected.
 */

/*! \brief Thread to periodically ping all clients and dump any that don't respond with a pong back in time */
static void *ping_thread(void *unused)
{
	struct irc_user *user;

	UNUSED(unused);

	for (;;) {
		int now, clients = 0;
		usleep(PING_TIME * 1000); /* convert ms to us */
		now = time(NULL);
		RWLIST_RDLOCK(&users);
		RWLIST_TRAVERSE(&users, user, entry) {
			/* Prevent concurrent writes to a user */
			pthread_mutex_lock(&user->lock);
			if (user->lastping && user->lastpong < now - PING_TIME) {
				char buf[32];
				/* Client never responded to the last ping. Disconnect it. */
				bbs_debug(3, "Ping expired for %p: last ping=%d, last pong=%d (now %d)\n", user, user->lastping, user->lastpong, now);
				snprintf(buf, sizeof(buf), "Ping timeout: %d seconds", now - user->lastpong); /* No CR LF */
				leave_all_channels(user, buf);
				send_reply(user, "ERROR :Connection timeout\r\n");
				shutdown(user->node->fd, SHUT_RDWR); /* Make the client handler thread break */
			} else {
				dprintf(user->wfd, "PING :%d\r\n", now);
				user->lastping = now;
				clients++;
			}
			pthread_mutex_unlock(&user->lock);
		}
		RWLIST_UNLOCK(&users);
		if (clients) {
			bbs_debug(5, "Performed periodic ping of %d client%s\n", clients, ESS(clients));
		}
	}
	return NULL;
}

/*! \brief Thread to handle a single IRC/IRCS client */
static void irc_handler(struct bbs_node *node, int secure)
{
#ifdef HAVE_OPENSSL
	SSL *ssl;
#endif
	int rfd, wfd;
	struct irc_user *user;

	user = calloc(1, sizeof(*user));
	if (!user) {
		return;
	}
	pthread_mutex_init(&user->lock, NULL);

	/* Start TLS if we need to */
	if (secure) {
		ssl = ssl_new_accept(node->fd, &rfd, &wfd);
		if (!ssl) {
			bbs_error("Failed to create SSL\n");
			return;
		}
	} else {
		rfd = wfd = node->fd;
	}

	user->rfd = rfd;
	user->wfd = wfd;
	user->node = node;
	user->modes = USER_MODE_NONE;
	user->joined = time(NULL);
	user->hostname = strdup(node->ip);
	if (secure) {
		user->modes |= USER_MODE_SECURE;
	}

	handle_client(user);

#ifdef HAVE_OPENSSL
	if (secure) { /* implies ssl */
		ssl_close(ssl);
		ssl = NULL;
	}
#endif
	user_free(user);
}

static void *__irc_handler(void *varg)
{
	struct bbs_node *node = varg;

	node->thread = pthread_self();
	bbs_node_begin(node);

	irc_handler(node, !strcmp(node->protname, "IRCS") ? 1 : 0); /* Actually handle the IRC/IRCS client */

	bbs_debug(3, "Node %d has ended its %s session\n", node->id, node->protname);
	bbs_node_exit(node); /* node is no longer a valid reference */
	return NULL;
}

/*! \brief Single listener thread for IRC and/or IRCS */
static void *irc_listener(void *unused)
{
	UNUSED(unused);
	bbs_tcp_listener2(irc_socket, ircs_socket, "IRC", "IRCS", __irc_handler, BBS_MODULE_SELF);
	return NULL;
}

static int load_config(void)
{
	struct bbs_config *cfg;

	cfg = bbs_config_load("net_irc.conf", 0);
	if (!cfg) {
		return 0;
	}

	bbs_config_val_set_true(cfg, "general", "logchannels", &log_channels);
	bbs_config_val_set_true(cfg, "general", "requiresasl", &require_sasl);

	/* IRC */
	bbs_config_val_set_true(cfg, "irc", "enabled", &irc_enabled);
	bbs_config_val_set_port(cfg, "irc", "port", &irc_port);

	/* IRCS */
	bbs_config_val_set_true(cfg, "ircs", "enabled", &ircs_enabled);
	bbs_config_val_set_port(cfg, "ircs", "port", &ircs_port);

	if (ircs_enabled && !ssl_available()) {
		bbs_error("TLS is not available, IRCS may not be used\n");
		return -1;
	}

	return 0;
}

static int load_module(void)
{
	if (load_config()) {
		return -1;
	}
	/* Since load_config returns 0 if no config, do this check here instead of in load_config: */
	if (!irc_enabled && !ircs_enabled) {
		bbs_debug(3, "Neither IRC nor IRCS is enabled, declining to load\n");
		return -1; /* Nothing is enabled */
	}

	/* If we can't start the TCP listeners, decline to load */
	if (irc_enabled && bbs_make_tcp_socket(&irc_socket, irc_port)) {
		return -1;
	}
	if (ircs_enabled && bbs_make_tcp_socket(&ircs_socket, ircs_port)) {
		close_if(irc_socket);
		return -1;
	}

	if (bbs_pthread_create(&irc_ping_thread, NULL, ping_thread, NULL)) {
		bbs_error("Unable to create IRC ping thread.\n");
		close_if(irc_socket);
		close_if(ircs_socket);
		return -1;
	}
	if (bbs_pthread_create(&irc_listener_thread, NULL, irc_listener, NULL)) {
		bbs_error("Unable to create IRC listener thread.\n");
		pthread_cancel(irc_ping_thread);
		close_if(irc_socket);
		close_if(ircs_socket);
		return -1;
	}

	if (irc_enabled) {
		bbs_register_network_protocol("IRC", irc_port);
	}
	if (ircs_enabled) {
		bbs_register_network_protocol("IRCS", ircs_port);
	}
	return 0;
}

static int unload_module(void)
{
	pthread_cancel(irc_ping_thread);
	pthread_cancel(irc_listener_thread);
	pthread_kill(irc_listener_thread, SIGURG);
	bbs_pthread_join(irc_ping_thread, NULL);
	bbs_pthread_join(irc_listener_thread, NULL);
	if (irc_enabled) {
		bbs_unregister_network_protocol(irc_port);
	}
	if (ircs_enabled) {
		bbs_unregister_network_protocol(ircs_port);
	}
	destroy_channels();
	return 0;
}

BBS_MODULE_INFO_STANDARD("RFC1459 Internet Relay Chat Server");