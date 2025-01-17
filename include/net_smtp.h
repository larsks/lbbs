/*
 * LBBS -- The Lightweight Bulletin Board System
 *
 * Copyright (C) 2023, Naveen Albert
 *
 * Naveen Albert <bbs@phreaknet.org>
 *
 */

/*! \file
 *
 * \brief RFC5321 Simple Mail Transfer Protocol (SMTP)
 *
 */

/* SMTP relay port (mail transfer agents) */
#define DEFAULT_SMTP_PORT 25

/* Mainly for encrypted SMTP message submission agents, though not explicitly in the RFC */
#define DEFAULT_SMTPS_PORT 465

/* Mainly for message submission agents, not encrypted by default, but may use STARTTLS */
#define DEFAULT_SMTP_MSA_PORT 587

struct smtp_session;

/*! \brief Get the SMTP hostname of the local SMTP server, suitable for use in HELO/EHLO */
const char *smtp_hostname(void);

/*!
 * \brief Whether an external host is allowed to relay mail for a particular domain
 * \param srcip Client IP address
 * \param hostname MAIL FROM domain
 * \retval 1 if explicitly authorized
 * \retval 0 if not authorized
 */
int smtp_relay_authorized(const char *srcip, const char *hostname);

/*!
 * \brief Whether a message is exempt from certain checks due to it being accepted for relay from another MTA
 * \param smtp
 * \retval 1 if exempt
 * \retval 0 if not exempt
 */
int smtp_is_exempt_relay(struct smtp_session *smtp);

/*!
 * \brief Get a timestamp string appropriate for the Received header
 * \param received Received time
 * \param[out] buf
 * \param len Length of buf
 */
void smtp_timestamp(time_t received, char *buf, size_t len);

/* == SMTP filters: these modify the message itself, but (generally) do not determine what will happen to it == */

enum smtp_filter_type {
	SMTP_FILTER_PREPEND = 0, 	/* Prepend headers to the received message (efficient, but only suitable for a narrow range of functions) */
	/* Allow for future expansion, e.g. milters (though this would require hooks at multiple stages, not just after body received) */
};

enum smtp_filter_scope {
	SMTP_SCOPE_INDIVIDUAL = 0, 	/* Run individually for each recipient of a message */
	SMTP_SCOPE_COMBINED, 		/* Run once for all recipients of a message */
};

enum smtp_direction {
	SMTP_DIRECTION_SUBMIT = (1 << 0),	/*!< Message submission */
	SMTP_DIRECTION_IN = (1 << 1),		/*!< Incoming mail from another MTA */
	SMTP_DIRECTION_OUT = (1 << 2),		/*!< Outgoing mail to another MTA */
};

struct smtp_filter_data {
	struct smtp_session *smtp;		/*!< SMTP session */
	int inputfd;					/*!< File descriptor from which message can be read */
	const char *recipient;			/*!< Recipient (RCPT TO). Only available for SMTP_DIRECTION_IN and SMTP_DIRECTION_SUBMIT, and if the scope is SMTP_SCOPE_INDIVIDUAL */
	size_t size;					/*!< Message length */
	enum smtp_direction dir;		/*!< Direction */
	int received;					/*!< Time that message was received */
	/* Duplicated fields: these are simply duplicated from smtp: */
	struct bbs_node *node;			/*!< Node */
	const char *from;				/*!< Envelope from */
	const char *helohost;			/*!< HELO/EHLO hostname */
	/* Set by filter callbacks */
	char *spf;						/*!< Allocated SPF header value */
	char *dkim;						/*!< Allocated DKIM results */
	char *dmarc;					/*!< Allocated DMARC results */
	char *arc;						/*!< Allocated ARC results */
	char *authresults;				/*!< Allocated Authentication-Results header */
	/* INTERNAL: Do not access these fields directly. Use the publicly exposed functions. */
	int outputfd;					/*!< File descriptor to write to, to prepend to message */
	char outputfile[64];			/*!< Temporary output file name */
	char *body;						/*!< RFC822 message as string */
};

struct smtp_filter_provider {
	int (*on_body)(struct smtp_filter_data *data);	/*!< Callback for SMTP_FILTER_PREPEND. Return 0 on success, -1 on failure (continue), 1 to abort further processing. */
};

/*!
 * \brief Register an SMTP filter
 * \param cb Callback function
 * \param type The type of filter
 * \param scope Scope for which this filter applies
 * \param dir Bitmask of directions for which this filter applies
 * \param priority Priority of filter. Lower-number priorities are executed first.
 * \param mod
 * \retval 0 on success, -1 on failure
 */
int __smtp_filter_register(struct smtp_filter_provider *provider, enum smtp_filter_type type, enum smtp_filter_scope scope, enum smtp_direction dir, int priority, void *mod);

#define smtp_filter_register(cb, type, scope, dir, priority) __smtp_filter_register(cb, type, scope, dir, priority, BBS_MODULE_SELF)

/*!
 * \brief Unregister an SMTP filter
 * \param cb
 * \retval 0 on success, -1 on failure
 */
int smtp_filter_unregister(struct smtp_filter_provider *provider);

/*! \brief Get the BBS node of an SMTP session */
struct bbs_node *smtp_node(struct smtp_session *smtp);

/*! \brief Get SMTP protocol used */
const char *smtp_protname(struct smtp_session *smtp);

/*! \brief Get the SMTP MAIL FROM address */
const char *smtp_from(struct smtp_session *smtp);

/*! \brief Get the SMTP MAIL FROM domain */
const char *smtp_from_domain(struct smtp_session *smtp);

/*! \brief Whether SPF validation should be performed */
int smtp_should_validate_spf(struct smtp_session *smtp);

/*! \brief Whether DKIM validation should be performed */
int smtp_should_validate_dkim(struct smtp_session *smtp);

/*! \brief Whether this is a message submission */
int smtp_is_message_submission(struct smtp_session *smtp);

/*! \brief Whether the sender's privacy should be protected */
int smtp_should_preserve_privacy(struct smtp_session *smtp);

/*!
 * \brief Get the estimated message size provided in MAIL FROM (SIZE=)
 * \return Size provided during MAIL FROM (may or may not be accurate!)
 * \return 0 if no estimated size provided
 */
size_t smtp_message_estimated_size(struct smtp_session *smtp);

/*!
 * \brief Get the Content-Type of a message, if available
 * \param smtp
 * \return Content-Type value
 * \return NULL if unavailable
 */
const char *smtp_message_content_type(struct smtp_session *smtp);

/*! \brief Time that message was received */
time_t smtp_received_time(struct smtp_session *smtp);

/*! \brief Get RFC822 message as string */
const char *smtp_message_body(struct smtp_filter_data *f);

/*! \brief Prepend arbitrary data to a message */
int __attribute__ ((format (gnu_printf, 2, 3))) smtp_filter_write(struct smtp_filter_data *f, const char *fmt, ...);

/*! \brief Prepend a header to a message */
int smtp_filter_add_header(struct smtp_filter_data *f, const char *name, const char *value);

/*! \brief Run a group of SMTP filters */
void smtp_run_filters(struct smtp_filter_data *fdata, enum smtp_direction dir);

/* == SMTP processor callbacks - these determine what will happen to a message, based on the message, but do not modify it == */

#define SMTP_MSG_DIRECTION_IN 0
#define SMTP_MSG_DIRECTION_OUT 1

struct smtp_msg_process {
	/* Inputs */
	int fd;						/*!< File descriptor of SMTP session */
	struct mailbox *mbox;		/*!< Mailbox (incoming only) */
	struct bbs_user *user;		/*!< BBS user (outgoing only) */
	struct bbs_node *node;		/*!< BBS node */
	const char *datafile;		/*!< Name of email data file */
	FILE *fp;					/*!< Email data file (used internally only) */
	const char *from;			/*!< Envelope from */
	const char *recipient;		/*!< Envelope to - only for INCOMING messages */
	int size;					/*!< Size of email */
	int userid;					/*!< User ID (outgoing only) */
	unsigned int direction:1;	/*!< 0 = incoming, 1 = outgoing */
	/* Outputs */
	unsigned int bounce:1;		/*!< Whether to send a bounce */
	unsigned int drop:1;		/*!< Whether message should be dropped */
	int res;					/*!< General return code */
	char *newdir;				/*!< New message location (incoming only) */
	char *bouncemsg;			/*!< Bounce message */
	struct stringlist *forward;	/*!< Forwarding addresses */
	char *relayroute;			/*!< Relay route */
};

/*! \brief Initialize an smtp_msg_process structure for use */
void smtp_mproc_init(struct smtp_session *smtp, struct smtp_msg_process *mproc);

/*!
 * \brief Register an SMTP processor callback to run on each message received or sent
 * \param cb Callback that should return nonzero to stop processing further callbacks
 */
#define smtp_register_processor(cb) __smtp_register_processor(cb, BBS_MODULE_SELF)

int __smtp_register_processor(int (*cb)(struct smtp_msg_process *mproc), void *mod);

/*! \brief Unregister an SMTP processor previously registered with smtp_register_processor */
int smtp_unregister_processor(int (*cb)(struct smtp_msg_process *mproc));

/*!
 * \brief Run SMTP callbacks for a message (only called by net_smtp)
 */
int smtp_run_callbacks(struct smtp_msg_process *mproc);

struct smtp_response {
	/* Response */
	int code;
	const char *subcode;
	const char *reply;
};

#define smtp_abort(r, c, sub, msg) \
	r->code = c; \
	r->subcode = #sub; \
	r->reply = msg;

struct smtp_delivery_agent {
	/*! \brief RCPT TO handler: can we deliver to this address? */
	/*! \retval 0 if this recipient cannot be handled by this delivery agent, 1 if yes, -1 if no and no other handler may handle it */
	int (*exists)(struct smtp_session *smtp, struct smtp_response *resp, const char *address, const char *user, const char *domain, int fromlocal, int tolocal);
	/*! \brief Deliver message (final delivery) */
	int (*deliver)(struct smtp_session *smtp, struct smtp_response *resp, const char *from, const char *recipient, const char *user, const char *domain, int fromlocal, int tolocal, int srcfd, size_t datalen, void **freedata);

	/* Supplementary (and very optional) callbacks, only need to be provided by one module */
	/*! \brief Save a copy of a sent message */
	int (*save_copy)(struct smtp_session *smtp, struct smtp_msg_process *mproc, int srcfd, size_t datalen, char *newfile, size_t newfilelen);
	/*! \brief Relay a message through another message submission agent */
	int (*relay)(struct smtp_session *smtp, struct smtp_msg_process *mproc, int srcfd, size_t datalen, struct stringlist *recipients);
};

#define smtp_register_delivery_handler(agent, priority) __smtp_register_delivery_handler(agent, priority, BBS_MODULE_SELF)

/*!
 * \brief Register an SMTP delivery agent
 * \param agent
 * \param priority Used for preference ordering. Like MX priorities, lower is more important.
 * \param void
 * \retval 0 on success, -1 on failure
 */
int __smtp_register_delivery_handler(struct smtp_delivery_agent *agent, int priority, void *mod);

/*!
 * \brief Unregister an SMTP delivery agent
 * \param agent
 * \retval 0 on success, -1 on failure
 */
int smtp_unregister_delivery_agent(struct smtp_delivery_agent *agent);

/*! \brief RFC 3464 2.3.3 Action field values */
enum smtp_delivery_action {
	DELIVERY_FAILED,
	DELIVERY_DELAYED,
	DELIVERY_DELIVERED,
	DELIVERY_RELAYED,
	DELIVERY_EXPANDED,
};

struct smtp_delivery_outcome;

/*!
 * \brief Create a delivery status notification for a recipient
 * \param recipient Email address of the recipient for which delivery failed
 * \param hostname Hostname of the remote MTA. NULL for local mail server.
 * \param ipaddr IP address of the remote MTA. NULL for local mail server.
 * \param status Status code
 * \param error The error as reported by the remote (or local) MTA.
 * \param stage The stage of delivery, e.g. "end of DATA", "RCPT TO", etc.
 * \param prot Protocol name for this stage, e.g. smtp, x-unix, etc.
 * \param action Delivery action
 * \param retryuntil Time until which message delivery will be retried (only if action is DELIVERY_DELAYED)
 * \return NULL on failure
 * \return Delivery failure, which must be freed using smtp_delivery_outcome_free
 */
struct smtp_delivery_outcome *smtp_delivery_outcome_new(const char *recipient, const char *hostname, const char *ipaddr, const char *status, const char *error, const char *prot, const char *stage, enum smtp_delivery_action action, struct tm *retryuntil);

/*!
 * \brief Free smtp_delivery_outcome (or multiple) allocated by smtp_delivery_outcome_new
 * \param f
 * \param n Number of elements (must be contiguous)
 */
void smtp_delivery_outcome_free(struct smtp_delivery_outcome **f, int n);

/*!
 * \brief Deliver an SMTP non-delivery report (bounce), originating from the postmaster
 * \param sendinghost HELO/EHLO hostname of MTA that sent us this message. NULL if not available.
 * \param arrival Time that message was originally delivered by sender for delivery
 * \param sender Email address that will receive the non-delivery report
 * \param srcfd File descriptor from which original message may be read.
 * \param offset Offset, in bytes, into srcfd from which to begin copying
 * \param msglen Number of bytes to read from srcfd.
 * \param f Delivery failure
 * \param n Number of delivery failures (must be contiguous)
 * \retval 0 if bounce was delivered or queued, -1 on failure
 * \note You should ignore the return code, because there is nothing that can be done if the bounce fails to be delivered.
 */
int smtp_dsn(const char *sendinghost, struct tm *arrival, const char *sender, int srcfd, int offset, size_t msglen, struct smtp_delivery_outcome **f, int n);

/*!
 * \brief Inject a message to deliver via SMTP, from outside of the SMTP protocol
 * \param mailfrom MAIL FROM. Do not include <>.
 * \param recipients List of recipients for RCPT TO. Must include <>. This list will be consumed and be empty be valid upon returning.
 * \param filename Entire RFC822 message
 * \return Same as expand_and_deliver's return value.
 */
int smtp_inject(const char *mailfrom, struct stringlist *recipients, const char *filename, size_t length);
