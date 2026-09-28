#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <yajl/yajl_gen.h>
#include <yajl/yajl_tree.h>

#include "util.h"
#include "yajl_dumps.h"

// This file is #included into dwm.c, so every file-scope name is prefixed
// to avoid aliasing dwm.c's globals (e.g. its own epoll_fd).
static struct sockaddr_un ipc_sockaddr;
static struct epoll_event ipc_sock_epoll_event;
static IPCClientList ipc_clients = NULL;
static int ipc_epoll_fd          = -1;
static int ipc_sock_fd           = -1;
static IPCCommand *ipc_commands;
static unsigned int ipc_commands_len;
// Nesting depth of client event handling. Clients scheduled for removal are
// only freed at depth 0, so no handler is left with a dangling IPCClient.
static int ipc_handling = 0;

static const uint32_t IPC_MAX_SIZE = IPC_MAX_MESSAGE_SIZE;
// Pending output above this limit means the client isn't reading; drop it
static const uint32_t IPC_MAX_PENDING_OUTPUT = 1 << 20;
static const int IPC_SOCKET_BACKLOG          = 5;
// Bound on messages handled per EPOLLIN so one client can't starve the WM.
// Epoll is level-triggered, so remaining input wakes us up again.
static const int IPC_MAX_MESSAGES_PER_EVENT = 32;

/**
 * Update the epoll events the client is woken up for. On failure the client
 * is scheduled for removal.
 */
static void ipc_set_client_events(IPCClient *c, uint32_t events) {
	if (c->event.events == events)
		return;

	c->event.events = events;
	if (epoll_ctl(ipc_epoll_fd, EPOLL_CTL_MOD, c->fd, &c->event) < 0) {
		fprintf(stderr, "Failed to modify epoll events of IPC fd %d: %s\n",
		        c->fd, strerror(errno));
		c->closing = 1;
	}
}

/**
 * Free all clients scheduled for removal, unless called from within client
 * event handling.
 */
static void ipc_reap_clients(void) {
	IPCClient *c, *next;

	if (ipc_handling)
		return;

	for (c = ipc_clients; c; c = next) {
		next = c->next;
		if (c->closing)
			ipc_drop_client(c);
	}
}

/**
 * Check whether the socket at ipc_sockaddr has a live listener
 *
 * Returns 1 if another process accepts connections on it, 0 otherwise
 */
static int ipc_socket_is_live(void) {
	int fd = socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int live;

	if (fd < 0)
		return 0;

	// A full backlog (EAGAIN) still means somebody is listening
	live = connect(fd, (const struct sockaddr *) &ipc_sockaddr,
	               sizeof(ipc_sockaddr))
	        == 0
	    || errno == EAGAIN || errno == EINPROGRESS;
	close(fd);

	return live;
}

/**
 * Create IPC socket at specified path and return file descriptor to socket.
 * This initializes the static variables ipc_sockaddr and ipc_sock_fd.
 */
static int ipc_create_socket(const char *filename) {
	char *normal_filename;
	char *parent;
	struct stat st;
	const size_t addr_size = sizeof(struct sockaddr_un);
	const int sock_type    = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
	int fd, res;
	mode_t old_umask;

	normalizepath(filename, &normal_filename);

	if (strlen(normal_filename) >= sizeof(ipc_sockaddr.sun_path)) {
		fprintf(stderr, "IPC socket path is too long (max %zu bytes): %s\n",
		        sizeof(ipc_sockaddr.sun_path) - 1, normal_filename);
		free(normal_filename);
		return -1;
	}

	// For portability clear the addr structure, since some implementations
	// have nonstandard fields in the structure
	memset(&ipc_sockaddr, 0, addr_size);
	ipc_sockaddr.sun_family = AF_LOCAL;
	strcpy(ipc_sockaddr.sun_path, normal_filename);

	// Create parent directories. A relative path without '/' has none; if
	// this fails, bind() reports the error.
	if (parentdir(normal_filename, &parent) == 0) {
		mkdirp(parent);
		free(parent);
	}
	free(normal_filename);

	// Replace a stale socket file, but never one owned by a live instance
	if (lstat(ipc_sockaddr.sun_path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			fprintf(stderr, "%s exists and is not a socket\n",
			        ipc_sockaddr.sun_path);
			return -1;
		}
		if (ipc_socket_is_live()) {
			fprintf(stderr, "Another instance is already listening on %s\n",
			        ipc_sockaddr.sun_path);
			return -1;
		}
		DEBUG("Removing stale socket %s\n", ipc_sockaddr.sun_path);
		unlink(ipc_sockaddr.sun_path);
	}

	fd = socket(AF_LOCAL, sock_type, 0);
	if (fd == -1) {
		fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
		return -1;
	}

	DEBUG("Created socket at %s\n", ipc_sockaddr.sun_path);

	/* only the owner may connect: the socket runs WM commands */
	old_umask = umask(077);
	res       = bind(fd, (const struct sockaddr *) &ipc_sockaddr, addr_size);
	umask(old_umask);
	if (res == -1) {
		fprintf(stderr, "Failed to bind socket to %s: %s\n",
		        ipc_sockaddr.sun_path, strerror(errno));
		close(fd);
		return -1;
	}

	DEBUG("Socket bound\n");

	if (listen(fd, IPC_SOCKET_BACKLOG) < 0) {
		fprintf(stderr, "Failed to listen for connections on socket: %s\n",
		        strerror(errno));
		close(fd);
		unlink(ipc_sockaddr.sun_path);
		return -1;
	}

	DEBUG("Now listening for connections on socket\n");

	ipc_sock_fd = fd;

	return fd;
}

/**
 * Initialization for generic JSON message. This is used to allocate the yajl
 * handle and set yajl options for replies and events.
 */
static void ipc_init_message(yajl_gen *gen) {
	if (!(*gen = yajl_gen_alloc(NULL)))
		die("yajl_gen_alloc failed");
	yajl_gen_config(*gen, yajl_gen_beautify, 1);
}

/**
 * Get the buffer of a finished yajl handle. The length includes the trailing
 * null char.
 *
 * Returns 0 on success
 * Returns -1 if the generated document is malformed
 * Returns -2 if the document exceeds IPC_MAX_SIZE
 */
static int ipc_message_buffer(yajl_gen gen, const unsigned char **buf,
                              size_t *len) {
	// A complete document refuses further values. Anything else means an
	// earlier yajl_gen_* call failed and the document is truncated.
	if (yajl_gen_null(gen) != yajl_gen_generation_complete)
		return -1;
	if (yajl_gen_get_buf(gen, buf, len) != yajl_gen_status_ok)
		return -1;
	(*len)++; // For null char
	if (*len > IPC_MAX_SIZE)
		return -2;

	return 0;
}

/**
 * Prepares buffers of IPC subscribers of specified event using buffer from
 * yajl handle. Frees the yajl handle.
 */
static void ipc_event_prepare_send_message(yajl_gen gen, IPCEvent event) {
	const unsigned char *buffer;
	size_t len = 0;

	if (ipc_message_buffer(gen, &buffer, &len) < 0) {
		fprintf(stderr, "Failed to generate IPC event %d\n", event);
	} else {
		for (IPCClient *c = ipc_clients; c; c = c->next) {
			if (c->subscriptions & event) {
				DEBUG("Sending event %d to fd %d\n", event, c->fd);
				ipc_prepare_send_message(c, IPC_TYPE_EVENT, len,
				                         (const char *) buffer);
			}
		}
	}

	// Not documented, but this frees buffer
	yajl_gen_free(gen);
}

/**
 * Prepares the IPC client's buffer with a message using the buffer of the
 * yajl handle. Frees the yajl handle.
 */
static void ipc_reply_prepare_send_message(yajl_gen gen, IPCClient *c,
                                           IPCMessageType msg_type) {
	const unsigned char *buffer;
	size_t len = 0;
	int res    = ipc_message_buffer(gen, &buffer, &len);

	if (res == 0)
		ipc_prepare_send_message(c, msg_type, len, (const char *) buffer);

	// Not documented, but this frees buffer
	yajl_gen_free(gen);

	if (res == -1)
		ipc_prepare_reply_failure(c, msg_type, "Failed to generate reply");
	else if (res == -2)
		ipc_prepare_reply_failure(c, msg_type, "Reply too large");
}

/**
 * Check whether any live client is subscribed to the event
 */
static int ipc_is_subscribed(IPCEvent event) {
	for (IPCClient *c = ipc_clients; c; c = c->next)
		if (!c->closing && (c->subscriptions & event))
			return 1;

	return 0;
}

/**
 * Find the IPCCommand with the specified name
 *
 * Returns 0 if a command with the specified name was found
 * Returns -1 if a command with the specified name could not be found
 */
static int ipc_get_ipc_command(const char *name, IPCCommand *ipc_command) {
	for (unsigned int i = 0; i < ipc_commands_len; i++) {
		if (strcmp(ipc_commands[i].name, name) == 0) {
			*ipc_command = ipc_commands[i];
			return 0;
		}
	}

	return -1;
}

/**
 * Free the members of a IPCParsedCommand struct
 */
static void ipc_free_parsed_command_members(IPCParsedCommand *command) {
	for (unsigned int i = 0; i < command->argc; i++) {
		if (command->arg_types[i] == ARG_TYPE_STR)
			free((void *) command->args[i].v);
	}
	free(command->args);
	free(command->arg_types);
	free(command->name);
	memset(command, 0, sizeof(IPCParsedCommand));
}

/**
 * Parse a IPC_TYPE_RUN_COMMAND message from a client. This function extracts
 * the arguments, argument count, argument types, and command name and returns
 * the parsed information as an IPCParsedCommand. If this function returns
 * successfully, the parsed_command must be freed using
 * ipc_free_parsed_command_members. On failure nothing needs to be freed and
 * *reason describes the error.
 *
 * Returns 0 if the message was successfully parsed
 * Returns -1 otherwise
 */
static int ipc_parse_run_command(const char *msg,
                                 IPCParsedCommand *parsed_command,
                                 const char **reason) {
	char error_buffer[1000];
	yajl_val parent =
	    yajl_tree_parse(msg, error_buffer, sizeof(error_buffer));

	if (parent == NULL) {
		fprintf(stderr, "Failed to parse command from client: %s\n",
		        error_buffer);
		*reason = "Invalid JSON";
		return -1;
	}

	// Format:
	// {
	//   "command": "<command name>"
	//   "args": [ "arg1", "arg2", ... ]
	// }
	const char *command_path[] = {"command", 0};
	yajl_val command_val = yajl_tree_get(parent, command_path, yajl_t_string);

	if (command_val == NULL) {
		*reason = "No 'command' key found in message";
		yajl_tree_free(parent);
		return -1;
	}

	const char *args_path[] = {"args", 0};
	yajl_val args_val       = yajl_tree_get(parent, args_path, yajl_t_array);

	if (args_val == NULL) {
		*reason = "No 'args' array found in message";
		yajl_tree_free(parent);
		return -1;
	}

	const char *command_name = YAJL_GET_STRING(command_val);
	const size_t name_size   = strlen(command_name) + 1;
	const size_t len         = args_val->u.array.len;

	// If no arguments are specified, make a dummy argument to pass to the
	// function. This is just the way dwm's void(Arg*) functions are setup.
	// calloc leaves it as a zeroed ARG_TYPE_NONE argument.
	parsed_command->argc = len ? len : 1;
	parsed_command->name = ecalloc(name_size, sizeof(char));
	parsed_command->args = ecalloc(parsed_command->argc, sizeof(Arg));
	parsed_command->arg_types =
	    ecalloc(parsed_command->argc, sizeof(ArgType));
	memcpy(parsed_command->name, command_name, name_size);

	DEBUG("Received command: %s\n", parsed_command->name);

	Arg *args          = parsed_command->args;
	ArgType *arg_types = parsed_command->arg_types;

	for (size_t i = 0; i < len; i++) {
		yajl_val arg_val = args_val->u.array.values[i];

		if (YAJL_IS_INTEGER(arg_val)) {
			// Any values below 0 must be a signed int, others are unsigned
			if (YAJL_GET_INTEGER(arg_val) < 0) {
				args[i].i    = YAJL_GET_INTEGER(arg_val);
				arg_types[i] = ARG_TYPE_SINT;
				DEBUG("i=%ld\n", args[i].i);
			} else {
				args[i].ui   = YAJL_GET_INTEGER(arg_val);
				arg_types[i] = ARG_TYPE_UINT;
				DEBUG("ui=%lu\n", args[i].ui);
			}
		} else if (YAJL_IS_DOUBLE(arg_val)) {
			args[i].f    = (float) YAJL_GET_DOUBLE(arg_val);
			arg_types[i] = ARG_TYPE_FLOAT;
			DEBUG("f=%f\n", args[i].f);
		} else if (YAJL_IS_STRING(arg_val)) {
			const char *arg_s = YAJL_GET_STRING(arg_val);
			size_t arg_s_size = strlen(arg_s) + 1;
			char *copy        = ecalloc(arg_s_size, sizeof(char));

			memcpy(copy, arg_s, arg_s_size);
			args[i].v    = copy;
			arg_types[i] = ARG_TYPE_STR;
		} else {
			*reason = "Unsupported argument type (expected number or "
			          "string)";
			ipc_free_parsed_command_members(parsed_command);
			yajl_tree_free(parent);
			return -1;
		}
	}

	yajl_tree_free(parent);

	return 0;
}

/**
 * Check if the given arguments are the correct length and type. Also do any
 * casting to correct the types.
 *
 * Returns 0 if the arguments were the correct length and types
 * Returns -1 if the argument count doesn't match
 * Returns -2 if the argument types don't match
 */
static int ipc_validate_run_command(IPCParsedCommand *parsed,
                                    const IPCCommand actual) {
	if (actual.argc != parsed->argc)
		return -1;

	for (unsigned int i = 0; i < parsed->argc; i++) {
		ArgType ptype = parsed->arg_types[i];
		ArgType atype = actual.arg_types[i];

		if (ptype != atype) {
			if (ptype == ARG_TYPE_UINT && atype == ARG_TYPE_PTR)
				// If this argument is supposed to be a void pointer, cast it
				parsed->args[i].v = (void *) parsed->args[i].ui;
			else if (ptype == ARG_TYPE_UINT && atype == ARG_TYPE_SINT)
				// If this argument is supposed to be a signed int, cast it
				parsed->args[i].i = parsed->args[i].ui;
			else
				return -2;
		}
	}

	return 0;
}

/**
 * Convert event name to their IPCEvent equivalent enum value
 *
 * Returns 0 if a valid event name was given
 * Returns -1 otherwise
 */
static int ipc_event_stoi(const char *subscription, IPCEvent *event) {
	if (strcmp(subscription, IPC_EVENT_NAME_TAG_CHANGE) == 0)
		*event = IPC_EVENT_TAG_CHANGE;
	else if (strcmp(subscription, IPC_EVENT_NAME_CLIENT_FOCUS_CHANGE) == 0)
		*event = IPC_EVENT_CLIENT_FOCUS_CHANGE;
	else if (strcmp(subscription, IPC_EVENT_NAME_LAYOUT_CHANGE) == 0)
		*event = IPC_EVENT_LAYOUT_CHANGE;
	else if (strcmp(subscription, IPC_EVENT_NAME_MONITOR_FOCUS_CHANGE) == 0)
		*event = IPC_EVENT_MONITOR_FOCUS_CHANGE;
	else if (strcmp(subscription, IPC_EVENT_NAME_FOCUSED_TITLE_CHANGE) == 0)
		*event = IPC_EVENT_FOCUSED_TITLE_CHANGE;
	else if (strcmp(subscription, IPC_EVENT_NAME_FOCUSED_STATE_CHANGE) == 0)
		*event = IPC_EVENT_FOCUSED_STATE_CHANGE;
	else
		return -1;
	return 0;
}

/**
 * Parse a IPC_TYPE_SUBSCRIBE message from a client. This function extracts
 * the event name and the subscription action from the message.
 *
 * Returns 0 if message was successfully parsed
 * Returns -1 otherwise, *reason describes the error
 */
static int ipc_parse_subscribe(const char *msg,
                               IPCSubscriptionAction *subscribe,
                               IPCEvent *event, const char **reason) {
	char error_buffer[100];
	yajl_val parent =
	    yajl_tree_parse(msg, error_buffer, sizeof(error_buffer));
	int ret = -1;

	if (parent == NULL) {
		fprintf(stderr, "Failed to parse subscription from client: %s\n",
		        error_buffer);
		*reason = "Invalid JSON";
		return -1;
	}

	// Format:
	// {
	//   "event": "<event name>"
	//   "action": "<subscribe|unsubscribe>"
	// }
	const char *event_path[]  = {"event", 0};
	const char *action_path[] = {"action", 0};
	yajl_val event_val    = yajl_tree_get(parent, event_path, yajl_t_string);
	yajl_val action_val   = yajl_tree_get(parent, action_path, yajl_t_string);
	const char *event_str = YAJL_GET_STRING(event_val);
	const char *action    = YAJL_GET_STRING(action_val);

	if (event_str == NULL) {
		*reason = "No 'event' key found in message";
	} else if (ipc_event_stoi(event_str, event) < 0) {
		*reason = "Event does not exist";
	} else if (action == NULL) {
		*reason = "No 'action' key found in message";
	} else if (strcmp(action, "subscribe") == 0) {
		*subscribe = IPC_ACTION_SUBSCRIBE;
		ret        = 0;
	} else if (strcmp(action, "unsubscribe") == 0) {
		*subscribe = IPC_ACTION_UNSUBSCRIBE;
		ret        = 0;
	} else {
		*reason = "Invalid action specified for subscription";
	}

	yajl_tree_free(parent);

	return ret;
}

/**
 * Parse an IPC_TYPE_GET_DWM_CLIENT message from a client. This function
 * extracts the window id from the message.
 *
 * Returns 0 if message was successfully parsed
 * Returns -1 otherwise, *reason describes the error
 */
static int ipc_parse_get_dwm_client(const char *msg, Window *win,
                                    const char **reason) {
	char error_buffer[100];
	yajl_val parent =
	    yajl_tree_parse(msg, error_buffer, sizeof(error_buffer));
	int ret = -1;

	if (parent == NULL) {
		fprintf(stderr, "Failed to parse message from client: %s\n",
		        error_buffer);
		*reason = "Invalid JSON";
		return -1;
	}

	// Format:
	// {
	//   "client_window_id": <client window id>
	// }
	const char *win_path[] = {"client_window_id", 0};
	yajl_val win_val       = yajl_tree_get(parent, win_path, yajl_t_number);

	if (!YAJL_IS_INTEGER(win_val) || YAJL_GET_INTEGER(win_val) < 0) {
		*reason = "No valid 'client_window_id' found in message";
	} else {
		*win = YAJL_GET_INTEGER(win_val);
		ret  = 0;
	}

	yajl_tree_free(parent);

	return ret;
}

/**
 * Called when an IPC_TYPE_RUN_COMMAND message is received from a client. This
 * function parses, executes the given command, and prepares a reply message
 * to the client indicating success/failure.
 *
 * NOTE: There is currently no check for argument validity beyond the number
 * of arguments given and types of arguments. There is also no way to check if
 * the function succeeded based on dwm's void(const Arg*) function types.
 * Pointer arguments can cause crashes if they are not validated in the
 * function itself.
 *
 * Returns 0 if message was successfully parsed
 * Returns -1 on failure parsing message
 */
static int ipc_run_command(IPCClient *ipc_client, const char *msg) {
	IPCParsedCommand parsed_command;
	IPCCommand ipc_command;
	const char *reason = NULL;

	// Initialize struct
	memset(&parsed_command, 0, sizeof(IPCParsedCommand));

	if (ipc_parse_run_command(msg, &parsed_command, &reason) < 0) {
		ipc_prepare_reply_failure(ipc_client, IPC_TYPE_RUN_COMMAND,
		                          "Failed to parse run command: %s", reason);
		return -1;
	}

	if (ipc_get_ipc_command(parsed_command.name, &ipc_command) < 0) {
		ipc_prepare_reply_failure(ipc_client, IPC_TYPE_RUN_COMMAND,
		                          "Command %s not found",
		                          parsed_command.name);
		ipc_free_parsed_command_members(&parsed_command);
		return -1;
	}

	int res = ipc_validate_run_command(&parsed_command, ipc_command);
	if (res < 0) {
		if (res == -1)
			ipc_prepare_reply_failure(ipc_client, IPC_TYPE_RUN_COMMAND,
			                          "%u arguments provided, %u expected",
			                          parsed_command.argc, ipc_command.argc);
		else
			ipc_prepare_reply_failure(ipc_client, IPC_TYPE_RUN_COMMAND,
			                          "Type mismatch");
		ipc_free_parsed_command_members(&parsed_command);
		return -1;
	}

	if (parsed_command.argc == 1)
		ipc_command.func.single_param(parsed_command.args);
	else if (parsed_command.argc > 1)
		ipc_command.func.array_param(parsed_command.args,
		                             parsed_command.argc);

	DEBUG("Called function for command %s\n", parsed_command.name);

	ipc_free_parsed_command_members(&parsed_command);

	ipc_prepare_reply_success(ipc_client, IPC_TYPE_RUN_COMMAND);
	return 0;
}

/**
 * Called when an IPC_TYPE_GET_MONITORS message is received from a client. It
 * prepares a reply with the properties of all of the monitors in JSON.
 */
static void ipc_get_monitors(IPCClient *c) {
	yajl_gen gen;
	ipc_init_message(&gen);
	dump_monitors(gen, mons, selmon);

	ipc_reply_prepare_send_message(gen, c, IPC_TYPE_GET_MONITORS);
}

/**
 * Called when an IPC_TYPE_GET_TAGS message is received from a client. It
 * prepares a reply with info about all the tags in JSON.
 */
static void ipc_get_tags(IPCClient *c) {
	yajl_gen gen;
	ipc_init_message(&gen);
	dump_tags(gen, tags, LENGTH(tags));

	ipc_reply_prepare_send_message(gen, c, IPC_TYPE_GET_TAGS);
}

/**
 * Called when an IPC_TYPE_GET_LAYOUTS message is received from a client. It
 * prepares a reply with a JSON array of available layouts
 */
static void ipc_get_layouts(IPCClient *c) {
	yajl_gen gen;
	ipc_init_message(&gen);
	dump_layouts(gen, layouts, LENGTH(layouts));

	ipc_reply_prepare_send_message(gen, c, IPC_TYPE_GET_LAYOUTS);
}

/**
 * Called when an IPC_TYPE_GET_DWM_CLIENT message is received from a client.
 * It prepares a JSON reply with the properties of the client with the
 * specified window XID, or a failure reply.
 *
 * Returns 0 if the message was successfully parsed and if the client with the
 *   specified window XID was found
 * Returns -1 otherwise
 */
static int ipc_get_dwm_client(IPCClient *ipc_client, const char *msg) {
	const char *reason = NULL;
	Window win;

	if (ipc_parse_get_dwm_client(msg, &win, &reason) < 0) {
		ipc_prepare_reply_failure(ipc_client, IPC_TYPE_GET_DWM_CLIENT,
		                          "Failed to parse get_dwm_client: %s",
		                          reason);
		return -1;
	}

	// Find client with specified window XID
	for (const Monitor *m = mons; m; m = m->next)
		for (Client *c = m->clients; c; c = c->next)
			if (c->win == win) {
				yajl_gen gen;
				ipc_init_message(&gen);

				dump_client(gen, c);

				ipc_reply_prepare_send_message(gen, ipc_client,
				                               IPC_TYPE_GET_DWM_CLIENT);

				return 0;
			}

	ipc_prepare_reply_failure(ipc_client, IPC_TYPE_GET_DWM_CLIENT,
	                          "Client with window id %lu not found", win);
	return -1;
}

/**
 * Called when an IPC_TYPE_SUBSCRIBE message is received from a client. It
 * subscribes/unsubscribes the client from the specified event and replies
 * with the result.
 *
 * Returns 0 if the message was successfully parsed.
 * Returns -1 if the message could not be parsed
 */
static int ipc_subscribe(IPCClient *c, const char *msg) {
	IPCSubscriptionAction action = IPC_ACTION_SUBSCRIBE;
	IPCEvent event               = 0;
	const char *reason           = NULL;

	if (ipc_parse_subscribe(msg, &action, &event, &reason) < 0) {
		ipc_prepare_reply_failure(c, IPC_TYPE_SUBSCRIBE, "%s", reason);
		return -1;
	}

	if (action == IPC_ACTION_SUBSCRIBE) {
		DEBUG("Subscribing client on fd %d to %d\n", c->fd, event);
		c->subscriptions |= event;
	} else {
		DEBUG("Unsubscribing client on fd %d from %d\n", c->fd, event);
		c->subscriptions &= ~event;
	}

	ipc_prepare_reply_success(c, IPC_TYPE_SUBSCRIBE);
	return 0;
}

/**
 * Compare monitor state with the last seen state and send events for
 * changes. Unlike ipc_send_events this never frees clients, so it is safe to
 * call while handling a client's messages.
 */
static void ipc_send_events_internal(void) {
	for (Monitor *m = mons; m; m = m->next) {
		unsigned int urg = 0, occ = 0, tagset = 0;

		for (Client *c = m->clients; c; c = c->next) {
			occ |= c->tags;

			if (c->isurgent)
				urg |= c->tags;
		}
		tagset = m->tagset[m->seltags];

		TagState new_state = {.selected = tagset,
		                      .occupied = occ,
		                      .urgent   = urg};

		if (memcmp(&m->tagstate, &new_state, sizeof(TagState)) != 0) {
			ipc_tag_change_event(m->num, m->tagstate, new_state);
			m->tagstate = new_state;
		}

		if (m->lastsel != m->sel) {
			ipc_client_focus_change_event(m->num, m->lastsel, m->sel);
			m->lastsel = m->sel;
		}

		if (strcmp(m->ltsymbol, m->lastltsymbol) != 0
		    || m->lastlt != m->lt[m->sellt]) {
			ipc_layout_change_event(m->num, m->lastltsymbol, m->lastlt,
			                        m->ltsymbol, m->lt[m->sellt]);
			strcpy(m->lastltsymbol, m->ltsymbol);
			m->lastlt = m->lt[m->sellt];
		}

		Client *sel = m->sel;
		if (!sel)
			continue;
		ClientState *o = &sel->prevstate;
		ClientState n  = {.oldstate     = sel->oldstate,
		                  .isfixed      = sel->isfixed,
		                  .isfloating   = sel->isfloating,
		                  .isfullscreen = sel->isfullscreen,
		                  .isurgent     = sel->isurgent,
		                  .neverfocus   = sel->neverfocus};
		if (memcmp(o, &n, sizeof(ClientState)) != 0) {
			ipc_focused_state_change_event(m->num, sel->win, o, &n);
			*o = n;
		}
	}

	// lastselmon is NULL at startup and after its monitor was removed;
	// there is no previous monitor to report then.
	if (lastselmon != selmon) {
		if (lastselmon && selmon)
			ipc_monitor_focus_change_event(lastselmon->num, selmon->num);
		lastselmon = selmon;
	}
}

/**
 * Handle a complete message received from a client and prepare the reply.
 * msg is NUL-terminated.
 *
 * Returns 0 if the request succeeded, -1 if a failure reply was prepared
 */
static int ipc_handle_message(IPCClient *c, uint8_t msg_type,
                              uint32_t msg_size, const char *msg) {
	DEBUG("[fd %d] Received message: '%.*s' Message type: %" PRIu8
	      " Message size: %" PRIu32 "\n",
	      c->fd, (int) msg_size, msg, msg_type, msg_size);

	switch (msg_type) {
	case IPC_TYPE_GET_MONITORS:
		ipc_get_monitors(c);
		return 0;
	case IPC_TYPE_GET_TAGS:
		ipc_get_tags(c);
		return 0;
	case IPC_TYPE_GET_LAYOUTS:
		ipc_get_layouts(c);
		return 0;
	case IPC_TYPE_RUN_COMMAND:
	case IPC_TYPE_GET_DWM_CLIENT:
	case IPC_TYPE_SUBSCRIBE:
		break;
	default:
		ipc_prepare_reply_failure(c, msg_type,
		                          "Invalid message type: %" PRIu8, msg_type);
		return -1;
	}

	if (msg_size == 0 || msg[0] == '\0') {
		ipc_prepare_reply_failure(c, msg_type, "Empty message");
		return -1;
	}

	if (msg_type == IPC_TYPE_RUN_COMMAND) {
		if (ipc_run_command(c, msg) < 0)
			return -1;
		ipc_send_events_internal();
		return 0;
	} else if (msg_type == IPC_TYPE_GET_DWM_CLIENT) {
		return ipc_get_dwm_client(c, msg);
	} else {
		return ipc_subscribe(c, msg);
	}
}

/**
 * The client closed its write side: stop reading and drop it once its
 * pending replies are written.
 */
static void ipc_client_eof(IPCClient *c) {
	DEBUG("EOF from client at fd %d\n", c->fd);
	c->eof = 1;
	ipc_set_client_events(c, c->event.events & ~EPOLLIN);
	if (c->buffer_size == 0)
		c->closing = 1;
}

/**
 * Read and handle the client's pending messages. If drain is set, all
 * available input is handled (the connection is going away), otherwise at
 * most IPC_MAX_MESSAGES_PER_EVENT messages.
 *
 * Returns 0 on success, -1 if any request failed or the client misbehaved
 */
static int ipc_handle_client_input(IPCClient *c, int drain) {
	int err = 0;

	for (int handled = 0;
	     !c->closing && (drain || handled < IPC_MAX_MESSAGES_PER_EVENT);
	     handled++) {
		uint8_t msg_type;
		uint32_t msg_size;
		char *msg;
		int ret = ipc_read_client(c, &msg_type, &msg_size, &msg);

		if (ret == 0)
			break;
		if (ret == -1) {
			ipc_client_eof(c);
			break;
		}
		if (ret < 0) {
			fprintf(stderr,
			        "Error reading message: dropping client at fd %d\n",
			        c->fd);
			c->closing = 1;
			err        = -1;
			break;
		}

		if (ipc_handle_message(c, msg_type, msg_size, msg) < 0)
			err = -1;
		free(msg);
	}

	return err;
}

int ipc_init(const char *socket_path, const int p_epoll_fd,
             IPCCommand commands[], const int commands_len) {
	// Initialize struct to 0
	memset(&ipc_sock_epoll_event, 0, sizeof(ipc_sock_epoll_event));

	int socket_fd = ipc_create_socket(socket_path);
	if (socket_fd < 0)
		return -1;

	ipc_commands     = commands;
	ipc_commands_len = commands_len;

	ipc_epoll_fd = p_epoll_fd;

	// Wake up to incoming connection requests
	ipc_sock_epoll_event.data.fd = socket_fd;
	ipc_sock_epoll_event.events  = EPOLLIN;
	if (epoll_ctl(ipc_epoll_fd, EPOLL_CTL_ADD, socket_fd,
	              &ipc_sock_epoll_event)) {
		fprintf(stderr, "Failed to add sock file descriptor to epoll: %s\n",
		        strerror(errno));
		ipc_cleanup();
		return -1;
	}

	return socket_fd;
}

void ipc_cleanup(void) {
	// Free clients and their buffers
	while (ipc_clients)
		ipc_drop_client(ipc_clients);

	if (ipc_sock_fd >= 0) {
		// Stop waking up for socket events
		if (ipc_epoll_fd >= 0)
			epoll_ctl(ipc_epoll_fd, EPOLL_CTL_DEL, ipc_sock_fd,
			          &ipc_sock_epoll_event);
		close(ipc_sock_fd);
		// Delete socket
		unlink(ipc_sockaddr.sun_path);
	}

	// Uninitialize all static variables
	ipc_epoll_fd     = -1;
	ipc_sock_fd      = -1;
	ipc_commands     = NULL;
	ipc_commands_len = 0;
	memset(&ipc_sock_epoll_event, 0, sizeof(struct epoll_event));
	memset(&ipc_sockaddr, 0, sizeof(struct sockaddr_un));
}

int ipc_get_sock_fd(void) {
	return ipc_sock_fd;
}

IPCClient *ipc_get_client(int fd) {
	return ipc_list_get_client(ipc_clients, fd);
}

int ipc_is_client_registered(int fd) {
	return (ipc_get_client(fd) != NULL);
}

int ipc_accept_client(void) {
	IPCClient *nc;
	int fd, flags;

	fd = accept(ipc_sock_fd, NULL, NULL);
	if (fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR
		    && errno != ECONNABORTED)
			fprintf(stderr, "Failed to accept IPC connection: %s\n",
			        strerror(errno));
		return -1;
	}

	// accept() doesn't inherit O_NONBLOCK from the listening socket
	if ((flags = fcntl(fd, F_GETFL)) < 0
	    || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
	    || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		fprintf(stderr, "Failed to set flags on new client fd %d: %s\n", fd,
		        strerror(errno));
		close(fd);
		return -1;
	}

	if (!(nc = ipc_client_new(fd))) {
		fputs("Failed to allocate IPC client\n", stderr);
		close(fd);
		return -1;
	}

	// Wake up to messages from this client. EPOLLHUP and EPOLLERR are
	// always reported.
	nc->event.events = EPOLLIN;
	if (epoll_ctl(ipc_epoll_fd, EPOLL_CTL_ADD, fd, &nc->event) < 0) {
		fprintf(stderr, "Failed to add client fd %d to epoll: %s\n", fd,
		        strerror(errno));
		close(fd);
		free(nc);
		return -1;
	}

	ipc_list_add_client(&ipc_clients, nc);

	DEBUG("New client at fd: %d\n", fd);

	return fd;
}

int ipc_drop_client(IPCClient *c) {
	const int fd = c->fd;

	// Stop waking up to messages from this client before closing, so the
	// fd is not left in the epoll set if it was duplicated
	if (ipc_epoll_fd >= 0)
		epoll_ctl(ipc_epoll_fd, EPOLL_CTL_DEL, fd, &c->event);

	// Even on EINTR the fd is released on Linux; never retry close()
	int res = close(fd);
	if (res < 0 && errno != EINTR)
		fprintf(stderr, "Failed to close fd %d: %s\n", fd, strerror(errno));

	ipc_list_remove_client(&ipc_clients, c);
	free(c->buffer);
	free(c->in_payload);
	free(c);

	DEBUG("Removed client on fd %d\n", fd);

	return res;
}

int ipc_read_client(IPCClient *c, uint8_t *msg_type, uint32_t *msg_size,
                    char **msg) {
	const uint32_t header_size = sizeof(dwm_ipc_header_t);
	ssize_t n;

	// Accumulate header
	while (c->in_header_len < header_size) {
		n = recv(c->fd, c->in_header + c->in_header_len,
		         header_size - c->in_header_len, 0);

		if (n == 0) {
			if (c->in_header_len > 0)
				fprintf(stderr,
				        "Unexpectedly reached EOF while reading header "
				        "from fd %d. Read %" PRIu32
				        " bytes, expected %" PRIu32 " bytes.\n",
				        c->fd, c->in_header_len, header_size);
			return -1;
		} else if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			fprintf(stderr, "Failed to read from fd %d: %s\n", c->fd,
			        strerror(errno));
			return -2;
		}

		c->in_header_len += n;
		if (c->in_header_len < header_size)
			continue;

		// Header complete: validate it and allocate the payload
		dwm_ipc_header_t header;
		memcpy(&header, c->in_header, header_size);

		if (memcmp(header.magic, IPC_MAGIC, IPC_MAGIC_LEN) != 0) {
			fprintf(stderr,
			        "Invalid magic string from fd %d. Got '%.*s', expected "
			        "'%s'\n",
			        c->fd, IPC_MAGIC_LEN, (const char *) header.magic,
			        IPC_MAGIC);
			return -2;
		}

		if (header.size > IPC_MAX_SIZE) {
			fprintf(stderr,
			        "Message too long from fd %d: %" PRIu32
			        " bytes. Maximum message size is: %" PRIu32 "\n",
			        c->fd, (uint32_t) header.size, IPC_MAX_SIZE);
			return -2;
		}

		c->in_size        = header.size;
		c->in_type        = header.type;
		c->in_payload_len = 0;
		if (!(c->in_payload = malloc(c->in_size + 1))) {
			fprintf(stderr,
			        "Failed to allocate %" PRIu32 " bytes for fd %d\n",
			        c->in_size + 1, c->fd);
			return -2;
		}
	}

	// Accumulate payload
	while (c->in_payload_len < c->in_size) {
		n = recv(c->fd, c->in_payload + c->in_payload_len,
		         c->in_size - c->in_payload_len, 0);

		if (n == 0) {
			fprintf(stderr,
			        "Unexpectedly reached EOF while reading payload from fd "
			        "%d. Read %" PRIu32 " bytes, expected %" PRIu32
			        " bytes.\n",
			        c->fd, c->in_payload_len, c->in_size);
			return -1;
		} else if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			fprintf(stderr, "Failed to read from fd %d: %s\n", c->fd,
			        strerror(errno));
			return -2;
		}

		c->in_payload_len += n;
	}

	// Make sure the message is null terminated to avoid parsing issues
	c->in_payload[c->in_size] = '\0';

	*msg_type = c->in_type;
	*msg_size = c->in_size;
	*msg      = c->in_payload;

	c->in_payload     = NULL;
	c->in_header_len  = 0;
	c->in_payload_len = 0;

	return 1;
}

ssize_t ipc_write_client(IPCClient *c) {
	size_t written = 0;

	while (written < c->buffer_size) {
		const ssize_t n = send(c->fd, c->buffer + written,
		                       c->buffer_size - written, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno != EPIPE && errno != ECONNRESET)
				fprintf(stderr, "Failed to write to fd %d: %s\n", c->fd,
				        strerror(errno));
			return -1;
		}

		written += n;
		DEBUG("Wrote %zu/%" PRIu32 " to client at fd %d\n", written,
		      c->buffer_size, c->fd);
	}

	if (written == c->buffer_size) {
		c->buffer_size = 0;
		free(c->buffer);
		// No dangling pointers!
		c->buffer = NULL;
		// Stop waking up when client is ready to receive messages
		ipc_set_client_events(c, c->event.events & ~EPOLLOUT);
	} else if (written > 0) {
		// Shift unwritten buffer to beginning of buffer
		c->buffer_size -= written;
		memmove(c->buffer, c->buffer + written, c->buffer_size);
	}

	return written;
}

void ipc_prepare_send_message(IPCClient *c, const IPCMessageType msg_type,
                              const uint32_t msg_size, const char *msg) {
	dwm_ipc_header_t header = {.magic = IPC_MAGIC_ARR,
	                           .type  = msg_type,
	                           .size  = msg_size};

	const uint32_t header_size = sizeof(dwm_ipc_header_t);
	const uint32_t packet_size = header_size + msg_size;
	char *buffer;

	if (c->closing)
		return;

	if (msg_size > IPC_MAX_SIZE) {
		fprintf(stderr,
		        "Refusing to send %" PRIu32 " byte message to fd %d\n",
		        msg_size, c->fd);
		return;
	}

	if (c->buffer_size + packet_size > IPC_MAX_PENDING_OUTPUT) {
		fprintf(stderr,
		        "Client at fd %d is not reading its messages (%" PRIu32
		        " bytes pending): dropping it\n",
		        c->fd, c->buffer_size);
		c->closing = 1;
		return;
	}

	if (!(buffer = realloc(c->buffer, c->buffer_size + packet_size))) {
		fprintf(stderr, "Failed to allocate output buffer for fd %d\n",
		        c->fd);
		c->closing = 1;
		return;
	}
	c->buffer = buffer;

	// Copy header to end of client buffer
	memcpy(c->buffer + c->buffer_size, &header, header_size);
	c->buffer_size += header_size;

	// Copy message to end of client buffer
	memcpy(c->buffer + c->buffer_size, msg, msg_size);
	c->buffer_size += msg_size;

	// Wake up when client is ready to receive messages
	ipc_set_client_events(c, c->event.events | EPOLLOUT);
}

void ipc_prepare_reply_failure(IPCClient *c, IPCMessageType msg_type,
                               const char *format, ...) {
	const unsigned char *msg;
	size_t msg_len;
	yajl_gen gen;
	va_list args;

	// Get output size
	va_start(args, format);
	int len = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if (len < 0)
		len = 0;

	char *buffer = ecalloc(len + 1, sizeof(char));

	va_start(args, format);
	vsnprintf(buffer, len + 1, format, args);
	va_end(args);

	fprintf(stderr, "[fd %d] Error: %s\n", c->fd, buffer);

	ipc_init_message(&gen);
	dump_error_message(gen, buffer);

	switch (ipc_message_buffer(gen, &msg, &msg_len)) {
	case 0:
		ipc_prepare_send_message(c, msg_type, msg_len, (const char *) msg);
		break;
	case -2:
		// The reason embeds client data; send it without the details
		yajl_gen_free(gen);
		ipc_init_message(&gen);
		dump_error_message(gen, "Error message too large");
		if (ipc_message_buffer(gen, &msg, &msg_len) == 0)
			ipc_prepare_send_message(c, msg_type, msg_len,
			                         (const char *) msg);
		break;
	default:
		fprintf(stderr, "[fd %d] Failed to generate error reply\n", c->fd);
		c->closing = 1;
		break;
	}

	yajl_gen_free(gen);
	free(buffer);
}

void ipc_prepare_reply_success(IPCClient *c, IPCMessageType msg_type) {
	const char *success_msg = "{\"result\":\"success\"}";
	const size_t msg_len    = strlen(success_msg) + 1; // +1 for null char

	ipc_prepare_send_message(c, msg_type, msg_len, success_msg);
}

void ipc_tag_change_event(int mon_num, TagState old_state,
                          TagState new_state) {
	yajl_gen gen;

	if (!ipc_is_subscribed(IPC_EVENT_TAG_CHANGE))
		return;
	ipc_init_message(&gen);
	dump_tag_event(gen, mon_num, old_state, new_state);
	ipc_event_prepare_send_message(gen, IPC_EVENT_TAG_CHANGE);
}

void ipc_client_focus_change_event(int mon_num, Client *old_client,
                                   Client *new_client) {
	yajl_gen gen;

	if (!ipc_is_subscribed(IPC_EVENT_CLIENT_FOCUS_CHANGE))
		return;
	ipc_init_message(&gen);
	dump_client_focus_change_event(gen, old_client, new_client, mon_num);
	ipc_event_prepare_send_message(gen, IPC_EVENT_CLIENT_FOCUS_CHANGE);
}

void ipc_layout_change_event(const int mon_num, const char *old_symbol,
                             const Layout *old_layout, const char *new_symbol,
                             const Layout *new_layout) {
	yajl_gen gen;

	if (!ipc_is_subscribed(IPC_EVENT_LAYOUT_CHANGE))
		return;
	ipc_init_message(&gen);
	dump_layout_change_event(gen, mon_num, old_symbol, old_layout, new_symbol,
	                         new_layout);
	ipc_event_prepare_send_message(gen, IPC_EVENT_LAYOUT_CHANGE);
}

void ipc_monitor_focus_change_event(const int last_mon_num,
                                    const int new_mon_num) {
	yajl_gen gen;

	if (!ipc_is_subscribed(IPC_EVENT_MONITOR_FOCUS_CHANGE))
		return;
	ipc_init_message(&gen);
	dump_monitor_focus_change_event(gen, last_mon_num, new_mon_num);
	ipc_event_prepare_send_message(gen, IPC_EVENT_MONITOR_FOCUS_CHANGE);
}

void ipc_focused_title_change_event(const int mon_num, const Window client_id,
                                    const char *old_name,
                                    const char *new_name) {
	yajl_gen gen;

	if (!ipc_is_subscribed(IPC_EVENT_FOCUSED_TITLE_CHANGE))
		return;
	ipc_init_message(&gen);
	dump_focused_title_change_event(gen, mon_num, client_id, old_name,
	                                new_name);
	ipc_event_prepare_send_message(gen, IPC_EVENT_FOCUSED_TITLE_CHANGE);
}

void ipc_focused_state_change_event(const int mon_num, const Window client_id,
                                    const ClientState *old_state,
                                    const ClientState *new_state) {
	yajl_gen gen;

	if (!ipc_is_subscribed(IPC_EVENT_FOCUSED_STATE_CHANGE))
		return;
	ipc_init_message(&gen);
	dump_focused_state_change_event(gen, mon_num, client_id, old_state,
	                                new_state);
	ipc_event_prepare_send_message(gen, IPC_EVENT_FOCUSED_STATE_CHANGE);
}

void ipc_send_events(void) {
	ipc_send_events_internal();
	// Drop subscribers that overflowed their output buffer
	ipc_reap_clients();
}

int ipc_handle_client_epoll_event(struct epoll_event *ev) {
	const int fd = ev->data.fd;
	IPCClient *c = ipc_get_client(fd);
	int ret      = 0;

	if (c == NULL)
		return -1;

	ipc_handling++;

	// Serve pending input first, so a client that writes a request and
	// closes the connection still gets its request executed
	if ((ev->events & EPOLLIN) && !c->eof && !c->closing)
		ret = ipc_handle_client_input(c, ev->events & (EPOLLHUP | EPOLLERR));

	if (ev->events & (EPOLLHUP | EPOLLERR)) {
		DEBUG("EPOLLHUP/EPOLLERR received from client at fd %d\n", fd);
		c->closing = 1;
	} else if ((ev->events & EPOLLOUT) && !c->closing && c->buffer_size) {
		DEBUG("Sending message to client at fd %d...\n", fd);
		if (ipc_write_client(c) < 0)
			c->closing = 1;
	}

	// Half-closed client whose replies have all been written
	if (c->eof && c->buffer_size == 0)
		c->closing = 1;

	ipc_handling--;
	ipc_reap_clients();

	return ret;
}

int ipc_handle_socket_epoll_event(struct epoll_event *ev) {
	if (!(ev->events & EPOLLIN))
		return -1;

	// EPOLLIN means incoming client connection request
	DEBUG("Received EPOLLIN event on socket\n");

	return ipc_accept_client();
}
