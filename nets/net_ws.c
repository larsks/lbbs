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
 * \brief RFC 6455 WebSocket Server
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#include "include/bbs.h"

#include <string.h>
#include <ctype.h> /* use isdigit */
#include <pthread.h>
#include <poll.h>

#include "include/module.h"
#include "include/config.h"
#include "include/node.h"
#include "include/user.h"
#include "include/utils.h"
#include "include/tls.h"
#include "include/test.h"

/* Needed for mod_http.h */
#include "include/linkedlists.h"
#include "include/variables.h"

#include "include/mod_http.h"
#include "include/net_ws.h"

#include <wss.h> /* libwss */

static int http_port = 0, https_port = 0;
static int ws_port = 0, wss_port = 0;
static char *allowed_origins = NULL;
static char phpsessdir[PATH_MAX] = "";
static char phpsessname[84] = "";
static char phpsessprefix[84] = "";

static void ws_log(int level, int len, const char *file, const char *function, int line, const char *buf)
{
	/*! \todo should be a version of __bbs_log that doesn't call asprintf, for efficiency (accepts # bytes) */
	switch (level) {
		case WS_LOG_ERROR:
			__bbs_log(LOG_ERROR, 0, file, line, function, "%.*s", len, buf);
			break;
		case WS_LOG_WARNING:
			__bbs_log(LOG_WARNING, 0, file, line, function, "%.*s", len, buf);
			break;
		case WS_LOG_DEBUG:
		default:
			__bbs_log(LOG_DEBUG, level - WS_LOG_DEBUG + 1, file, line, function, "%.*s", len, buf);
	}
}

enum php_var_type {
	PHP_VAR_NUMBER = 0,
	PHP_VAR_BOOL,
	PHP_VAR_STRING,
	PHP_VAR_ARRAY,
	/* I'm probably leaving some stuff out here... but this should capture the major things, certainly everything we care about */
};

struct php_var {
	const char *name;
	enum php_var_type type;
	union {
		int number;
		char *string;
		struct php_varlist *array;
		unsigned int bool:1;
	} value;
	RWLIST_ENTRY(php_var) entry;
	char data[]; /* name, and for string vars, also the string */
};

RWLIST_HEAD(php_varlist, php_var);

struct ws_session {
	struct wss_client *client;	/*!< libwss WebSocket client */
	struct bbs_node *node;		/*!< BBS node */
	struct http_session *http;	/*!< HTTP session */
	pthread_mutex_t lock;		/*!< Session lock for serializing writing */
	void *data;					/*!< Module specific user data */
	int pollfd;					/*!< Additional fd to poll */
	int pollms;					/*!< Poll timeout */
	struct php_varlist varlist;
	unsigned int proxied:1;		/*!< Reverse proxied or direct? */
	unsigned int sessionchecked:1;
};

struct ws_route {
	const char *uri;
	struct ws_callbacks *callbacks;
	RWLIST_ENTRY(ws_route) entry;
	void *mod;
	char data[0];
};

static RWLIST_HEAD_STATIC(routes, ws_route);

int __websocket_route_register(const char *uri, struct ws_callbacks *callbacks, void *mod)
{
	struct ws_route *route;

	RWLIST_WRLOCK(&routes);
	RWLIST_TRAVERSE(&routes, route, entry) {
		if (!strcmp(uri, route->uri)) {
			bbs_error("WebSocket route '%s' already registered\n", route->uri);
			RWLIST_UNLOCK(&routes);
			return -1;
		}
	}
	route = calloc(1, sizeof(*route) + strlen(uri) + 1);
	if (ALLOC_FAILURE(route)) {
		RWLIST_UNLOCK(&routes);
		return -1;
	}
	route->mod = mod;
	route->callbacks = callbacks;
	strcpy(route->data, uri); /* Safe */
	route->uri = route->data;

	RWLIST_INSERT_HEAD(&routes, route, entry);
	RWLIST_UNLOCK(&routes);
	return 0;
}

int websocket_route_unregister(const char *uri)
{
	struct ws_route *route;

	RWLIST_WRLOCK(&routes);
	RWLIST_TRAVERSE_SAFE_BEGIN(&routes, route, entry) {
		if (!strcmp(route->uri, uri)) {
			RWLIST_REMOVE_CURRENT(entry);
			free(route);
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&routes);
	if (!route) {
		bbs_error("WebSocket route '%s' was not registered?\n", uri);
		return -1;
	}
	return 0;
}

static struct ws_route *find_route(const char *uri)
{
	struct ws_route *route;

	RWLIST_RDLOCK(&routes);
	RWLIST_TRAVERSE(&routes, route, entry) {
		if (!strcmp(uri, route->uri)) {
			break;
		}
	}
	if (route) {
		bbs_module_ref(route->mod);
	}
	RWLIST_UNLOCK(&routes);
	return route;
}

void websocket_attach_user_data(struct ws_session *ws, void *data)
{
	ws->data = data;
}

void websocket_set_custom_poll_fd(struct ws_session *ws, int fd, int pollms)
{
	ws->pollfd = fd;
	ws->pollms = pollms;
}

void websocket_sendtext(struct ws_session *ws, const char *buf, size_t len)
{
	pthread_mutex_lock(&ws->lock);
	wss_write(ws->client, WS_OPCODE_TEXT, buf, len);
	pthread_mutex_unlock(&ws->lock);
}

static int php_var_append(struct php_varlist *vars, const char *name, enum php_var_type type, void *value, size_t len)
{
	struct php_var *var;
	size_t namelen;

	if (strlen_zero(name)) { /* This is not right, but don't crash, or assert */
		bbs_error("Variable name is NULL?\n");
		return -1;
	}

	namelen = strlen(name);
	var = calloc(1, sizeof(*var) + namelen + 1 + (len ? len + 1 : 0)); /* len only used for strings */
	if (ALLOC_FAILURE(var)) {
		return -1;
	}

	var->type = type;
	strcpy(var->data, name);
	var->name = var->data;

	switch(type) {
		case PHP_VAR_NUMBER:
			var->value.number = *((int*) value);
			bbs_debug(5, "Added NUMBER variable '%s' = %d\n", name, var->value.number);
			break;
		case PHP_VAR_BOOL:
			SET_BITFIELD(var->value.bool, *((int*) value));
			bbs_debug(5, "Added BOOL variable '%s' = %d\n", name, var->value.bool);
			break;
		case PHP_VAR_STRING:
			memcpy(var->data + namelen + 1, value, len); /* NOT null terminated! */
			var->data[namelen + 1 + len] = '\0';
			var->value.string = var->data + namelen + 1;
			bbs_debug(5, "Added STRING variable '%s' = %s\n", name, var->value.string);
			break;
		case PHP_VAR_ARRAY:
			var->value.array = (struct php_varlist*) value;
			bbs_debug(5, "Added ARRAY variable %s\n", name);
			break;
	}
	RWLIST_INSERT_TAIL(vars, var, entry);
	return 0;
}

static void php_vars_destroy(struct php_varlist *vars)
{
	struct php_var *var;
	while ((var = RWLIST_REMOVE_HEAD(vars, entry))) {
		switch (var->type) {
			case PHP_VAR_ARRAY:
				php_vars_destroy(var->value.array);
				free(var->value.array);
				/* Fall through */
			case PHP_VAR_NUMBER:
			case PHP_VAR_BOOL:
			case PHP_VAR_STRING:
				free(var);
				break;
		}
	}
}

#define PARSE_EXPECT_CHAR(ch, c) \
	if (ch != c) { \
		bbs_warning("Expected %c but found (%d) %c at position %ld (remainder: %s)\n", c, sep, isprint(sep) ? sep : ' ', s - start, s); \
		return -1; \
	}

static int php_unserialize_array(struct php_varlist *vars, char **sptr, const char *start, size_t len, int arraylen)
{
	char tmpbuf[20];
	char *name = NULL;
	int res = 0;
	int is_value = 0;
	int c = 0;

	char *s = *sptr;

	while (c < arraylen) {
		char vartype, sep;
		struct php_varlist *sublist;
		int tmp, remaining;

		is_value = name ? 1 : 0;

		vartype = *s++;
		sep = *s++;
		PARSE_EXPECT_CHAR(sep, ':');
		switch (vartype) {
			case 'b': /* Boolean */
				if (!name) {
					bbs_warning("Boolean cannot be used for array key\n");
					return -1;
				}
				tmp = atoi(s); /* atoi will stop where it should */
				res |= php_var_append(vars, name, PHP_VAR_BOOL, &tmp, 0);
				s++;
				break;
			case 'i': /* Number */
				tmp = atoi(s); /* atoi will stop where it should */
				if (name) {
					res |= php_var_append(vars, name, PHP_VAR_NUMBER, &tmp, 0);
				} else {
					snprintf(tmpbuf, sizeof(tmpbuf), "%d", tmp);
					name = tmpbuf;
				}
				while (*s && *s != ';') {
					s++;
				}
				break;
			case 's': /* String */
				tmp = atoi(s); /* This is the length of the string */
				if (tmp < 0 || tmp > 65535) {
					bbs_warning("String length invalid or disallowed: %d\n", tmp);
					return -1;
				}
				while (isdigit(*s)) { /* Skip length */
					s++;
				}
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, ':');
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '"');
				/* Check bounds BEFORE we just use the next N bytes... otherwise we have a Heartbleed-style bug... */
				remaining = ((int) len - (int) (s - start));
				if (tmp >= remaining) {
					bbs_warning("String length would take us out of bounds (%d >= %d)\n", tmp, remaining);
					return -1;
				}
				if (!s) {
					bbs_warning("String value is NULL?\n");
					return -1;
				}
				if (name) {
					res |= php_var_append(vars, name, PHP_VAR_STRING, s, (size_t) tmp);
				} else {
					snprintf(tmpbuf, sizeof(tmpbuf), "%.*s", tmp, s);
					name = tmpbuf;
				}
				s += tmp;
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '"');
				break;
			case 'a': /* Array */
				if (!name) {
					bbs_warning("Array cannot be used for array key\n");
					return -1;
				}
				tmp = atoi(s);
				while (isdigit(*s)) { /* Skip size */
					s++;
				}
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '{');
				sublist = calloc(1, sizeof(*sublist));
				if (ALLOC_FAILURE(sublist)) {
					return -1;
				}

				*sptr = s;
				res |= php_var_append(vars, name, PHP_VAR_ARRAY, sublist, 0);
				if (!res) {
					res |= php_unserialize_array(sublist, sptr, start, len, tmp);
				}
				s = *sptr;

				if (res) {
					break;
				}
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '}');
				break;
			default:
				bbs_warning("Unexpected var type '%c'\n", vartype);
				return -1;
		}
		/* Abort immediately if any failure occurs */
		if (res) {
			return res;
		}
		if (is_value) {
			name = NULL; /* Reset for next round */
			c++;
		}
		sep = *s++;
		PARSE_EXPECT_CHAR(sep, ';');
	}

	*sptr = s;
	return 0;
}

static int php_unserialize(struct php_varlist *vars, char **sptr, const char *start, size_t len)
{
	int res = 0;

	/* Parse it:
	 * Example:
	 *
	 * $_SESSION['myarray']['server'] = 'localhost';
	 * $_SESSION['myarray']['port'] = 143;
	 * $_SESSION['myarray']['secure'] = false;
	 * $_SESSION['test'] = 3;
	 * $_SESSION['testing'] = "44";
	 * $_SESSION['true'] = true;
	 *
	 * myarray|a:3:{s:6:"server";s:9:"localhost";s:4:"port";1:143;s:6:"secure";b:0;}test|i:3;testing|s:2:"44";true|b:1;
	 *
	 */

	/* Observations:
	 * The | symbol separates the key from the value (at the top level)
	 * values begin with their type and value, prefixed by length in octets if needed (for strings), e.g. a:3:, s:2:
	 * For strings, this includes \, ", or any other characters that might seem like they escape or need to be escaped.
	 * Just use the length.
	 */

	char *s = *sptr; /* Nice side effect? Can't use restrict because of strsep, but since this is local it should have the same effect anyways */

	bbs_debug(6, "Parsing: %s\n", start);

	for (;;) {
		char vartype, sep;
		struct php_varlist *sublist;
		int tmp, remaining;
		char *name = strsep(&s, "|");
		if (strlen_zero(name)) {
			/* We're done */
			break;
		}
		if (strlen_zero(s)) {
			bbs_warning("Key has no value?\n");
			return -1;
		}
		vartype = *s++;
		if (strlen_zero(s)) {
			bbs_warning("Unexpected end of key value\n");
			return -1;
		}
		sep = *s++;
		PARSE_EXPECT_CHAR(sep, ':');
		if (strlen_zero(s)) {
			bbs_warning("Key has no value?\n");
			return -1;
		}
		switch (vartype) {
			case 'b': /* Boolean */
				tmp = atoi(s); /* atoi will stop where it should */
				res |= php_var_append(vars, name, PHP_VAR_BOOL, &tmp, 0);
				s++;
				break;
			case 'i': /* Number */
				tmp = atoi(s); /* atoi will stop where it should */
				res |= php_var_append(vars, name, PHP_VAR_NUMBER, &tmp, 0);
				while (*s && *s != ';') {
					s++;
				}
				break;
			case 's': /* String */
				tmp = atoi(s); /* This is the length of the string */
				if (tmp < 0 || tmp > 65535) {
					bbs_warning("String length invalid or disallowed: %d\n", tmp);
					return -1;
				}
				while (isdigit(*s)) { /* Skip length */
					s++;
				}
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, ':');
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '"');
				/* Check bounds BEFORE we just use the next N bytes... otherwise we have a Heartbleed-style bug... */
				remaining = ((int) len - (int) (s - start));
				if (tmp >= remaining) {
					bbs_warning("String length would take us out of bounds (%d >= %d)\n", tmp, remaining);
					return -1;
				}
				if (!s) {
					bbs_warning("String value is NULL?\n");
					return -1;
				}
				res |= php_var_append(vars, name, PHP_VAR_STRING, s, (size_t) tmp);
				s += tmp;
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '"');
				break;
			case 'a': /* Array */
				/* Arrays are kind of unique in that, technically, the keys can be numbers, too, not just strings. */
				/* We're unfaithful here in that we force all keys to be strings, to simplify things a bit.
				 * If the numbers are just the implicit ordering (e.g. arr|a:3:{i:0;i:4;i:1;i:3;i:2;s:1:"2"; ),
				 * it won't matter because order is still preserved anyways. But otherwise, this is a limitation to keep in mind. */
				tmp = atoi(s);
				while (isdigit(*s)) { /* Skip size */
					s++;
				}
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, ':');
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '{');
				sublist = calloc(1, sizeof(*sublist));
				if (ALLOC_FAILURE(sublist)) {
					return -1;
				}

				*sptr = s;
				res |= php_var_append(vars, name, PHP_VAR_ARRAY, sublist, 0);
				if (!res) {
					res |= php_unserialize_array(sublist, sptr, start, len, tmp);
				}
				s = *sptr;

				if (res) {
					break;
				}
				sep = *s++;
				PARSE_EXPECT_CHAR(sep, '}');
				continue; /* After arrays, no ;, it ends with } */
			default:
				bbs_warning("Unexpected var type '%c'\n", vartype);
				return -1;
		}
		/* Abort immediately if any failure occurs */
		if (res) {
			return res;
		}
		sep = *s++;
		if (!sep) {
			break; /* That's fine too. There won't be one after the last one. */
		}
		PARSE_EXPECT_CHAR(sep, ';');
	}

	*sptr = s;
	return 0;
}

static struct php_var *php_var_find(struct php_varlist *vars, const char *name)
{
	struct php_var *v;

	/* No need to lock. No other thread knows about this. */
	RWLIST_TRAVERSE(vars, v, entry) {
		if (!strcmp(v->name, name)) {
			return v;
		}
	}
	return NULL;
}

static int test_php_unserialize(void)
{
	char buf[512];
	int length;
	int res = -1;
	struct php_varlist varlist, *sublist;
	struct php_var *v;
	char *bufptr;

	memset(&varlist, 0, sizeof(varlist));

	length = snprintf(buf, sizeof(buf), "foo|b:1;string|s:3:\"123\";arr|a:3:{i:0;i:4;i:1;i:3;i:2;s:1:\"2\";}");
	bufptr = buf;
	php_unserialize(&varlist, &bufptr, buf, (size_t) length);

	v = php_var_find(&varlist, "foo");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_BOOL);
	bbs_test_assert_equals(v->value.bool, 1);

	v = php_var_find(&varlist, "string");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_STRING);
	bbs_test_assert_str_exists_equals(v->value.string, "123");

	v = php_var_find(&varlist, "arr");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_ARRAY);

	v = php_var_find(&varlist, "arr");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_ARRAY);
	sublist = v->value.array;

	v = php_var_find(sublist, "0");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_NUMBER);
	bbs_test_assert_equals(v->value.number, 4);

	php_vars_destroy(&varlist);

	length = snprintf(buf, sizeof(buf), "webmail|a:11:{s:6:\"server\";s:9:\"localhost\";s:4:\"port\";i:143;s:6:\"secure\";b:0;s:10:\"smtpserver\";s:9:\"localhost\";s:8:\"smtpport\";i:587;s:10:\"smtpsecure\";s:4:\"none\";s:8:\"username\";s:4:\"test\";s:8:\"password\";s:4:\"test\";s:10:\"loginlimit\";i:0;s:6:\"append\";b:1;s:6:\"active\";i:1686046936;}test|b:1;testing|s:9:\"4|4test\"s\";arr|a:3:{i:0;i:4;i:1;i:3;i:2;s:1:\"2\";}");
	bufptr = buf;
	php_unserialize(&varlist, &bufptr, buf, (size_t) length);

	v = php_var_find(&varlist, "testing");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_STRING);
	bbs_test_assert_str_exists_equals(v->value.string, "4|4test\"s");

	v = php_var_find(&varlist, "webmail");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_ARRAY);
	sublist = v->value.array;

	v = php_var_find(sublist, "port");
	bbs_test_assert_exists(v);
	bbs_test_assert_equals(v->type, PHP_VAR_NUMBER);
	bbs_test_assert_equals(v->value.number, 143);

	res = 0;

cleanup:
	php_vars_destroy(&varlist);
	return res;
}



static struct unit_tests {
	const char *name;
	int (*callback)(void);
} tests[] =
{
	{ "PHP Sessions", test_php_unserialize },
};

static int php_load_session(struct ws_session *ws)
{
	/* Check if a PHP session exists with this name */
	char sessfile[PATH_MAX + 6];
	char *contents, *dup;
	int length; /* XXX Should really be size_t */
	const char *sessionid = http_get_cookie(ws->http, phpsessname);

	if (!sessionid) {
		bbs_debug(4, "No PHP session cookie set\n");
		return -1;
	}

	snprintf(sessfile, sizeof(sessfile), "%s/sess_%s", phpsessdir, sessionid);
	if (!bbs_file_exists(sessfile)) { /* Not strictly needed, but don't emit warning in bbs_file_to_string if it doesn't */
		bbs_debug(4, "Session file %s does not exist\n", sessfile);
		return -1;
	}
	contents = bbs_file_to_string(sessfile, 8192, &length);
	if (!contents) {
		bbs_debug(4, "Session file %s too large / empty?\n", sessfile);
		return -1;
	}

	dup = contents;
	php_unserialize(&ws->varlist, &dup, contents, (size_t) length);
	free(contents);
	return 0;
}

static struct php_var *php_get_session_value(struct ws_session *ws, const char *key)
{
	struct php_var *var;
	struct php_varlist *varlist = &ws->varlist;

	/* Don't check this more than once */
	if (!ws->sessionchecked) {
		ws->sessionchecked = 1;
		php_load_session(ws);
	}

	if (!s_strlen_zero(phpsessprefix)) {
		var = php_var_find(varlist, phpsessprefix);
		if (!var) {
			bbs_verb(4, "PHP session variable '%s' not found\n", phpsessprefix);
			return NULL;
		}
		if (var->type != PHP_VAR_ARRAY) {
			bbs_verb(4, "PHP session variable '%s' not array\n", phpsessprefix);
			return NULL;
		}
		varlist = var->value.array;
	}

	var = php_var_find(varlist, key);
	if (!var) {
		bbs_debug(2, "PHP session key '%s' not found\n", key);
		return NULL;
	}
	return var;
}

static const char *php_get_session_string(struct ws_session *ws, const char *key)
{
	struct php_var *var = php_get_session_value(ws, key);

	if (var->type != PHP_VAR_STRING) {
		bbs_debug(1, "Variable %s exists, but it's not a string\n", key);
		return NULL;
	}
	return var->value.string;
}

static int php_get_session_number(struct ws_session *ws, const char *key)
{
	struct php_var *var = php_get_session_value(ws, key);

	switch (var->type) {
		case PHP_VAR_BOOL:
			return var->value.bool;
		case PHP_VAR_NUMBER:
			return var->value.number;
		case PHP_VAR_STRING:
			return atoi(var->value.string);
		case PHP_VAR_ARRAY:
			bbs_debug(2, "Variable %s is an array, not number\n", key);
			return 0;
	}
	return 0;
}

const char *websocket_session_data_string(struct ws_session *ws, const char *key)
{
	if (!ws->proxied) {
		/*! \todo Ask mod_http for the answer */
		bbs_warning("Not proxied, dunno how to answer\n");
		return NULL;
	} else {
		if (!s_strlen_zero(phpsessname) && !s_strlen_zero(phpsessdir)) {
			return php_get_session_string(ws, key);
		} else {
			bbs_debug(1, "No way to look up session information externally\n");
		}
	}
	return NULL;
}

int websocket_session_data_number(struct ws_session *ws, const char *key)
{
	if (ws->proxied) {
		if (!s_strlen_zero(phpsessname) && !s_strlen_zero(phpsessdir)) {
			return php_get_session_number(ws, key);
		} else {
			bbs_debug(1, "No way to look up session information externally\n");
		}
	} else {
		bbs_warning("Not proxied, dunno how to answer\n");
		/*! \todo Ask mod_http for the answer */
		return 0;
	}
	return 0;
}

/*! \note The details of the underlying WebSocket library are intentionally abstracted away here.
 * WebSocket applications just deal with the net_ws module, the net_ws module deals with
 * the WebSocket library that actually speaks the WebSocket protocol on the wire. */
static void ws_handler(struct bbs_node *node, struct http_session *http, int rfd, int wfd, int proxied)
{
	struct ws_session ws;
	struct ws_route *route;
	struct wss_client *client;
	int res;
	int want_ping = 0;
	char ping_data[15];
	struct pollfd pfds[2];

	/* no memset needed, directly initialize all values */
	ws.node = node;
	ws.http = http;
	ws.data = NULL;
	ws.pollfd = -1;
	ws.pollms = -1;
	memset(&ws.varlist, 0, sizeof(ws.varlist));
	SET_BITFIELD(ws.proxied, proxied);

	bbs_verb(5, "Handling %s WebSocket client on node %d to %s\n", proxied ? "proxied" : "direct", node->id, http->req->uri);

	route = find_route(http->req->uri);
	if (!route) {
		bbs_warning("Rejecting WebSocket connection for '%s' (no such WebSocket route)\n", http->req->uri);
		return; /* Get lost, dude */
	}

	if (allowed_origins) {
		/* Check that the client's origin is allowed. */
		char match_str[256];
		const char *origin = http_request_header(http, "Origin");
		if (strlen_zero(origin)) {
			bbs_warning("No Origin header supplied\n");
			return; /* Goodbye */
		} else if (strchr(origin, ',')) {
			bbs_warning("Origin header seems invalid: %s\n", origin);
			return;
		}
		snprintf(match_str, sizeof(match_str), ",%s,", origin);
		if (!strstr(allowed_origins, match_str)) {
			bbs_warning("Client origin '%s' is not explicitly allowed, rejecting\n", origin);
			return;
		}
		bbs_debug(4, "Origin '%s' is explicitly allowed\n", origin);
	}

	client = wss_client_new(&ws, rfd, wfd);
	if (!client) {
		bbs_error("Failed to create WebSocket client\n");
		return;
	}
	ws.client = client; /* Needed as part of structure so it can be accessed in websocket_sendtext */

	pthread_mutex_init(&ws.lock, NULL);

	if (route->callbacks->on_open && route->callbacks->on_open(&ws)) {
		return;
	}

	memset(&pfds, 0, sizeof(pfds));
	pfds[0].fd = rfd;
	pfds[0].events = POLLIN;

	/* In case there's something else to poll. */
	pfds[1].fd = -1;
	pfds[1].events = POLLIN;

	for (;;) {
		nfds_t numfds = ws.pollfd == -1 ? 1 : 2;
		pfds[1].fd = ws.pollfd;
		pfds[0].revents = pfds[1].revents = 0;
		res = poll(pfds, numfds, ws.pollms);
		if (res < 0) {
			if (errno = EINTR) {
				continue;
			}
			bbs_warning("poll failed: %s\n", strerror(errno));
			break;
		}
		/*! \todo IMAP: poll imap client fd, as well as websocket client fd - could use readline on the IMAP side. */
		if (pfds[0].revents) {
			res = wss_read(client, SEC_MS(55), 1); /* Pass in 1 since we already know poll returned activity for this fd */
			if (res < 0) {
				bbs_debug(3, "Failed to read WebSocket frame\n");
				if (wss_error_code(client)) {
					wss_close(client, wss_error_code(client));
				} else {
					wss_close(client, WS_CLOSE_PROTOCOL_ERROR);
				}
				break;
			} else if (!res) {
				int framelen;
				/* Send a ping if we haven't heard anything in a while */
				if (++want_ping > 1) {
					/* Already had a ping outstanding that wasn't ponged. Disconnect. */
					bbs_debug(3, "Still haven't received ping reply, disconnecting client\n");
					break;
				}
				/* Use current timestamp as our ping data */
				framelen = snprintf(ping_data, sizeof(ping_data), "%ld", time(NULL));
				wss_write(client, WS_OPCODE_PING, ping_data, (size_t) framelen);
			} else {
				struct wss_frame *frame = wss_client_frame(client);
				bbs_debug(1, "WebSocket '%s' frame received\n", wss_frame_name(frame));
				switch (wss_frame_opcode(frame)) {
					case WS_OPCODE_TEXT:
						if (route->callbacks->on_text_message && route->callbacks->on_text_message(&ws, ws.data, wss_frame_payload(frame), wss_frame_payload_length(frame))) {
							wss_frame_destroy(frame);
							goto done; /* Can't break out of loop from within switch */
						}
						break;
					case WS_OPCODE_BINARY:
						/* Do something... */
						bbs_warning("Ignoring received binary frame\n");
						break;
					case WS_OPCODE_CLOSE:
						/* Close the connection and break */
						bbs_debug(2, "Client closed WebSocket connection (code %d)\n", wss_close_code(frame));
						wss_close(client, WS_CLOSE_NORMAL);
						wss_frame_destroy(frame);
						goto done; /* Can't break out of loop from within switch */
					case WS_OPCODE_PING:
						/* Reply in kind with a pong containing the same payload */
						wss_write(client, WS_OPCODE_PONG, wss_frame_payload(frame), wss_frame_payload_length(frame));
						break;
					case WS_OPCODE_PONG:
						/* Confirm receipt of a previous ping, or ignore if unexpected */
						if (wss_frame_payload_length(frame) && !strcmp(ping_data, wss_frame_payload(frame))) {
							want_ping = 0;
						} else {
							bbs_debug(5, "Ignoring unexpected PONG\n");
						}
						break;
					default:
						bbs_warning("Unexpected WS opcode %d?\n", wss_frame_opcode(frame));
				}
				wss_frame_destroy(frame);
			}
		} else if (pfds[1].revents) {
			/* Activity on the application's file descriptor of interest. Let it know. */
			if (route->callbacks->on_poll_activity) {
				if (route->callbacks->on_poll_activity(&ws, ws.data)) {
					break;
				}
			}
		} else {
			/* Nothing happened. Let the application know. */
			if (route->callbacks->on_poll_timeout) {
				if (route->callbacks->on_poll_timeout(&ws, ws.data)) {
					break;
				}
			}
		}
	}

done:
	if (client) {
		if (route->callbacks->on_close) {
			route->callbacks->on_close(&ws, ws.data);
		}
		wss_client_destroy(client);
		pthread_mutex_destroy(&ws.lock);
	}
	bbs_module_unref(route->mod);
	php_vars_destroy(&ws.varlist);
}

static void ws_direct_handler(struct bbs_node *node, int secure)
{
	SSL *ssl = NULL;
	int res;

	/* needed for HTTP structure */
	char buf[1024];
	struct readline_data rldata;
	struct http_session http;

	memset(&http, 0, sizeof(http));
	http.node = node;
	http.rldata = &rldata;
	http.req = &http.reqstack;
	http.res = &http.resstack;
	SET_BITFIELD(http.secure, secure);

	/* Start TLS if we need to */
	if (secure) {
		ssl = ssl_new_accept(node->fd, &http.rfd, &http.wfd);
		if (!ssl) {
			return; /* Disconnect. */
		}
	} else {
		http.rfd = http.wfd = node->fd;
	}

	/* If this is a direct connection, either the client connected directly to us,
	 * or we went through a reverse proxy, which is going to relay the headers forward to us.
	 * Either way, we still need to perform the websocket handshake (most likely upgrading from HTTP 1.1).
	 * Read the headers and pay attention only to the ones that we care about. */

	/* XXX How do we know this is actually an HTTP request coming in, in the first place, not a direct WS connection from the get go? */

	bbs_readline_init(&rldata, buf, sizeof(buf));
	res = http_parse_request(&http, buf); /* This will, among other things, load any cookies in the request, so we can identify the client. */
	if (res) {
		goto cleanup; /* Just disconnect, don't even bother sending a response */
	}

	bbs_debug(4, "Ready to begin WebSocket handshake\n");
	if (!http_websocket_upgrade_requested(&http)) {
		bbs_debug(3, "Not a WebSocket client?\n"); /* Probably just rando TCP traffic hitting this port. Drop it. */
		goto cleanup;
	} else if (http_websocket_handshake(&http)) {
		goto cleanup; /* WebSocket handshake failed */
	}

	/* Handshake succeeded! Okay, we're done with the HTTP stuff now. It's just websockets from here on out. */
	ws_handler(node, &http, http.rfd, http.wfd, 1); /* Seems backwards, but this is a reverse proxied connection, most likely */
	http_request_cleanup(http.req);

cleanup:
	if (ssl) {
		ssl_close(ssl);
		ssl = NULL;
	}
}

static enum http_response_code ws_proxy_handler(struct http_session *http)
{
	/* If this is a reverse proxied connection through the web server, upgrade it first,
	 * then hand it off to the websocket server. */
	if (!http_websocket_upgrade_requested(http) || http_websocket_handshake(http)) {
		return HTTP_BAD_REQUEST;
	}
	/* This is now a websocket connection, speaking the websocket protocol. */
	ws_handler(http->node, http, http->rfd, http->wfd, 0);
	/* Return back to the web server, which will simply terminate the connection */
	return http->res->code;
}

static void *__ws_handler(void *varg)
{
	struct bbs_node *node = varg;

	node->thread = pthread_self();
	bbs_node_begin(node);

	ws_direct_handler(node, !strcmp(node->protname, "WSS"));

	bbs_debug(3, "Node %d has ended its %s session\n", node->id, node->protname);
	bbs_node_exit(node); /* node is no longer a valid reference */
	return NULL;
}

static int load_config(void)
{
	struct bbs_config *cfg;

	cfg = bbs_config_load("net_http.conf", 0);
	if (cfg) {
		int http_enabled = 0, https_enabled = 0;
		bbs_config_val_set_true(cfg, "http", "enabled", &http_enabled);
		if (http_enabled) {
			bbs_config_val_set_port(cfg, "http", "port", &http_port);
		}
		bbs_config_val_set_true(cfg, "https", "enabled", &https_enabled);
		if (https_enabled) {
			bbs_config_val_set_port(cfg, "https", "port", &https_port);
		}
	}
	cfg = bbs_config_load("net_ws.conf", 0);
	if (cfg) {
		struct bbs_config_section *section = NULL;
		bbs_config_val_set_path(cfg, "sessions", "phpsessdir", phpsessdir, sizeof(phpsessdir));
		bbs_config_val_set_str(cfg, "sessions", "phpsessname", phpsessname, sizeof(phpsessname));
		bbs_config_val_set_str(cfg, "sessions", "phpsessprefix", phpsessprefix, sizeof(phpsessprefix));
		bbs_config_val_set_port(cfg, "ws", "port", &ws_port);
		bbs_config_val_set_port(cfg, "wss", "port", &wss_port);
		while ((section = bbs_config_walk(cfg, section))) {
			/* Already processed */
			if (!strcmp(bbs_config_section_name(section), "sessions")) {
				continue;
			} else if (!strcmp(bbs_config_section_name(section), "ws") || !strcmp(bbs_config_section_name(section), "wss")) {
				continue;
			}

			if (!strcmp(bbs_config_section_name(section), "origins")) {
				struct bbs_keyval *keyval = NULL;
				struct dyn_str origins;
				int numorigins = 0;
				memset(&origins, 0, sizeof(origins));
				while ((keyval = bbs_config_section_walk(section, keyval))) {
					const char *host = bbs_keyval_key(keyval);
					dyn_str_append(&origins, ",", STRLEN(","));
					dyn_str_append(&origins, host, strlen(host));
					numorigins++;
				}
				if (numorigins) {
					dyn_str_append(&origins, ",", STRLEN(","));
					/* Now every value is surrounded by commas, including the first and last (makes it easy to use strstr) */
					allowed_origins = origins.buf;
				}
			} else {
				bbs_warning("Unknown section name, ignoring: %s\n", bbs_config_section_name(section));
			}
		}
	}
	if (!allowed_origins) {
		bbs_warning("All origins are implicitly allowed: application may be vulnerable to client side attacks\n");
	}
	return 0;
}

static int unload_module(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(tests); i++) {
		bbs_unregister_test(tests[i].callback);
	}

	http_unregister_route(ws_proxy_handler);
	if (ws_port) {
		bbs_stop_tcp_listener(ws_port);
	}
	if (wss_port) {
		bbs_stop_tcp_listener(wss_port);
	}
	free_if(allowed_origins);
	return 0;
}

static int load_module(void)
{
	unsigned int i;
	int res = 0;
	if (load_config()) {
		return -1;
	}
	wss_set_logger(ws_log);
	wss_set_log_level(WS_LOG_DEBUG + 5);
	/* Register reverse proxy routes if needed */

	for (i = 0; i < ARRAY_LEN(tests); i++) {
		res |= bbs_register_test(tests[i].name, tests[i].callback);
	}

	/* XXX Need to register all routes? */
	if (http_port) {
		res |= http_register_insecure_route("/ws", (unsigned short int) http_port, NULL, HTTP_METHOD_GET, ws_proxy_handler);
	}
	if (https_port) {
		res |= http_register_secure_route("/ws", (unsigned short int) http_port, NULL, HTTP_METHOD_GET, ws_proxy_handler);
	}
	if (res) {
		return unload_module();
	}
	/* Register listener(s) to accept WebSocket connections directly, e.g. from another reverse proxy (e.g. Apache HTTP server) */
	res = bbs_start_tcp_listener3(ws_port ? ws_port : 0, wss_port ? wss_port : 0, 0, "WS", "WSS", NULL, __ws_handler);
	return res ? unload_module() : res;
}

BBS_MODULE_INFO_FLAGS_DEPENDENT("WebSocket Server", MODFLAG_GLOBAL_SYMBOLS, "mod_http.so");