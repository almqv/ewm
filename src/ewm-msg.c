#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <yajl/yajl_gen.h>
#include <yajl/yajl_tree.h>

#include "ipc-protocol.h"

#define YSTR(str)    yajl_gen_string(gen, (unsigned char *) str, strlen(str))
#define YINT(num)    yajl_gen_integer(gen, num)
#define YDOUBLE(num) yajl_gen_double(gen, num)
#define YBOOL(v)     yajl_gen_bool(gen, v)
#define YNULL()      yajl_gen_null(gen)
#define YARR(body)                                                           \
	{                                                                        \
		yajl_gen_array_open(gen);                                            \
		body;                                                                \
		yajl_gen_array_close(gen);                                           \
	}
#define YMAP(body)                                                           \
	{                                                                        \
		yajl_gen_map_open(gen);                                              \
		body;                                                                \
		yajl_gen_map_close(gen);                                             \
	}

typedef unsigned long Window;

static int sock_fd               = -1;
static unsigned int ignore_reply = 0;

static void fatal(int status, const char *format, ...) {
	va_list args;

	fputs("ewm-msg: ", stderr);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);

	exit(status);
}

/**
 * Read exactly count bytes from the socket.
 *
 * Returns 0 on success, -1 on read error (errno is set), -2 on EOF
 */
static int read_all(void *buf, size_t count) {
	size_t read_bytes = 0;

	while (read_bytes < count) {
		const ssize_t n =
		    read(sock_fd, (uint8_t *) buf + read_bytes, count - read_bytes);

		if (n == 0)
			return -2;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		read_bytes += n;
	}

	return 0;
}

/**
 * Receive one message. The reply is NUL-terminated and must be freed.
 * Exits if the message can't be received.
 */
static void read_socket(IPCMessageType *msg_type, uint32_t *msg_size,
                        char **msg) {
	dwm_ipc_header_t header;
	int ret;

	if ((ret = read_all(&header, sizeof(header))) < 0)
		goto fail;

	if (memcmp(header.magic, IPC_MAGIC, IPC_MAGIC_LEN) != 0)
		fatal(2, "invalid magic string in reply");

	if (header.size > IPC_MAX_MESSAGE_SIZE)
		fatal(2, "reply too long: %" PRIu32 " bytes, maximum is %d",
		      (uint32_t) header.size, IPC_MAX_MESSAGE_SIZE);

	if (!(*msg = malloc(header.size + 1)))
		fatal(2, "cannot allocate %" PRIu32 " bytes",
		      (uint32_t) header.size + 1);

	if ((ret = read_all(*msg, header.size)) < 0)
		goto fail;

	(*msg)[header.size] = '\0';
	*msg_size           = header.size;
	*msg_type           = header.type;

	return;

fail:
	if (ret == -2)
		fatal(2, "connection closed by ewm");
	fatal(2, "error receiving response from socket: %s", strerror(errno));
}

static void write_socket(const void *buf, size_t count) {
	size_t written = 0;

	while (written < count) {
		const ssize_t n = send(sock_fd, (const uint8_t *) buf + written,
		                       count - written, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fatal(2, "error sending message: %s", strerror(errno));
		}
		written += n;
	}
}

static void connect_to_socket(const char *path) {
	struct sockaddr_un addr;

	// Initialize struct to 0
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;

	if (strlen(path) >= sizeof(addr.sun_path))
		fatal(1, "socket path too long: %s", path);
	strcpy(addr.sun_path, path);

	if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		fatal(1, "cannot create socket: %s", strerror(errno));

	if (connect(sock_fd, (const struct sockaddr *) &addr,
	            sizeof(struct sockaddr_un))
	    < 0)
		fatal(1, "cannot connect to %s: %s", path, strerror(errno));
}

static void send_message(IPCMessageType msg_type, size_t msg_size,
                         const uint8_t *msg) {
	if (msg_size > IPC_MAX_MESSAGE_SIZE)
		fatal(1, "message too long: %zu bytes, maximum is %d", msg_size,
		      IPC_MAX_MESSAGE_SIZE);

	dwm_ipc_header_t header = {.magic = IPC_MAGIC_ARR,
	                           .size  = msg_size,
	                           .type  = msg_type};

	write_socket(&header, sizeof(header));
	write_socket(msg, msg_size);
}

/**
 * Parse an optionally negative decimal integer.
 *
 * Returns 1 if s is one and fits in a long long, 0 otherwise
 */
static int parse_signed_int(const char *s, long long *num) {
	const char *p = *s == '-' ? s + 1 : s;

	if (!*p)
		return 0;
	for (; *p; p++)
		if (!isdigit((unsigned char) *p))
			return 0;

	errno = 0;
	*num  = strtoll(s, NULL, 10);

	return errno != ERANGE;
}

/**
 * Parse a decimal unsigned integer.
 *
 * Returns 1 if s is one and fits in an unsigned long, 0 otherwise
 */
static int parse_unsigned_int(const char *s, unsigned long *num) {
	if (!*s)
		return 0;
	for (const char *p = s; *p; p++)
		if (!isdigit((unsigned char) *p))
			return 0;

	errno = 0;
	*num  = strtoul(s, NULL, 10);

	return errno != ERANGE;
}

/**
 * Parse a decimal number with an optional leading '-' and at most one '.'
 * that is neither the first nor the last character.
 *
 * Returns 1 if s is one and fits in a double, 0 otherwise
 */
static int parse_float(const char *s, double *num) {
	const size_t len = strlen(s);
	int is_dot_used  = 0;
	size_t digits    = 0;

	for (size_t i = 0; i < len; i++) {
		if (isdigit((unsigned char) s[i]))
			digits++;
		else if (!is_dot_used && s[i] == '.' && i != 0 && i != len - 1)
			is_dot_used = 1;
		else if (s[i] == '-' && i == 0)
			continue;
		else
			return 0;
	}

	if (!digits)
		return 0;

	errno = 0;
	*num  = strtod(s, NULL);

	return errno != ERANGE;
}

/**
 * Check whether a reply is an error reply: {"result": "error", ...}
 */
static int is_error_reply(const char *reply) {
	const char *result_path[] = {"result", 0};
	yajl_val tree             = yajl_tree_parse(reply, NULL, 0);
	yajl_val result = yajl_tree_get(tree, result_path, yajl_t_string);
	const char *res = YAJL_GET_STRING(result);
	const int error = res && strcmp(res, "error") == 0;

	yajl_tree_free(tree);

	return error;
}

/**
 * Receive a reply and print it unless print is 0.
 *
 * Returns 1 if the reply is an error reply, 0 otherwise
 */
static int handle_socket_reply(int print) {
	IPCMessageType reply_type;
	uint32_t reply_size;
	char *reply;
	int error;

	read_socket(&reply_type, &reply_size, &reply);

	if (print) {
		printf("%.*s\n", (int) reply_size, reply);
		fflush(stdout);
	}
	error = is_error_reply(reply);
	free(reply);

	return error;
}

static yajl_gen new_gen(void) {
	yajl_gen gen = yajl_gen_alloc(NULL);

	if (!gen)
		fatal(1, "cannot allocate JSON generator");

	return gen;
}

static int run_command(const char *name, char *args[], int argc) {
	const unsigned char *msg;
	size_t msg_size;
	long long inum;
	double fnum;

	yajl_gen gen = new_gen();

	// Message format:
	// {
	//   "command": "<name>",
	//   "args": [ ... ]
	// }
	// clang-format off
  YMAP(
    YSTR("command"); YSTR(name);
    YSTR("args"); YARR(
      for (int i = 0; i < argc; i++) {
        if (parse_signed_int(args[i], &inum)) {
          YINT(inum);
        } else if (parse_float(args[i], &fnum)) {
          YDOUBLE(fnum);
        } else {
          YSTR(args[i]);
        }
      }
    )
  )
	// clang-format on

	yajl_gen_get_buf(gen, &msg, &msg_size);

	send_message(IPC_TYPE_RUN_COMMAND, msg_size, msg);

	handle_socket_reply(!ignore_reply);

	yajl_gen_free(gen);

	return 0;
}

static int get_monitors(void) {
	send_message(IPC_TYPE_GET_MONITORS, 1, (const uint8_t *) "");
	handle_socket_reply(1);
	return 0;
}

static int get_tags(void) {
	send_message(IPC_TYPE_GET_TAGS, 1, (const uint8_t *) "");
	handle_socket_reply(1);

	return 0;
}

static int get_layouts(void) {
	send_message(IPC_TYPE_GET_LAYOUTS, 1, (const uint8_t *) "");
	handle_socket_reply(1);

	return 0;
}

static int get_dwm_client(Window win) {
	const unsigned char *msg;
	size_t msg_size;

	yajl_gen gen = new_gen();

	// Message format:
	// {
	//   "client_window_id": "<win>"
	// }
	// clang-format off
  YMAP(
    YSTR("client_window_id"); YINT(win);
  )
	// clang-format on

	yajl_gen_get_buf(gen, &msg, &msg_size);

	send_message(IPC_TYPE_GET_DWM_CLIENT, msg_size, msg);

	handle_socket_reply(1);

	yajl_gen_free(gen);

	return 0;
}

/**
 * Subscribe to an event
 *
 * Returns 0 on success, -1 if ewm replied with an error
 */
static int subscribe(const char *event) {
	const unsigned char *msg;
	size_t msg_size;
	int error;

	yajl_gen gen = new_gen();

	// Message format:
	// {
	//   "event": "<event>",
	//   "action": "subscribe"
	// }
	// clang-format off
  YMAP(
    YSTR("event"); YSTR(event);
    YSTR("action"); YSTR("subscribe");
  )
	// clang-format on

	yajl_gen_get_buf(gen, &msg, &msg_size);

	send_message(IPC_TYPE_SUBSCRIBE, msg_size, msg);

	error = handle_socket_reply(!ignore_reply);

	yajl_gen_free(gen);

	if (error) {
		fprintf(stderr, "ewm-msg: cannot subscribe to %s\n", event);
		return -1;
	}

	return 0;
}

static void usage_error(const char *prog_name, const char *format, ...) {
	va_list args;
	va_start(args, format);

	fprintf(stderr, "Error: ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\nusage: %s [options] <command> [...]\n", prog_name);
	fprintf(stderr, "Try '%s help'\n", prog_name);

	va_end(args);
	exit(1);
}

static void print_usage(const char *name) {
	printf("usage: %s [options] <command> [...]\n", name);
	puts("");
	puts("Commands:");
	puts("  run_command <name> [args...]    Run an IPC command");
	puts("");
	puts("  get_monitors                    Get monitor properties");
	puts("");
	puts("  get_tags                        Get list of tags");
	puts("");
	puts("  get_layouts                     Get list of layouts");
	puts("");
	puts("  get_dwm_client <window_id>      Get ewm client properties");
	puts("");
	puts("  subscribe [events...]           Subscribe to specified events");
	puts("                                  "
	     "Options: " IPC_EVENT_NAME_TAG_CHANGE ",");
	puts("                                  " IPC_EVENT_NAME_LAYOUT_CHANGE
	     ",");
	puts("                                 "
	     " " IPC_EVENT_NAME_CLIENT_FOCUS_CHANGE ",");
	puts("                                 "
	     " " IPC_EVENT_NAME_MONITOR_FOCUS_CHANGE ",");
	puts("                                 "
	     " " IPC_EVENT_NAME_FOCUSED_TITLE_CHANGE ",");
	puts("                                 "
	     " " IPC_EVENT_NAME_FOCUSED_STATE_CHANGE);
	puts("");
	puts("  help                            Display this message");
	puts("");
	puts("Options:");
	puts("  --ignore-reply                  Don't print reply messages from");
	puts("                                  run_command and subscribe.");
	puts("");
	puts("  --socket PATH                   Connect to the ewm socket at "
	     "PATH.");
	puts("                                  Default: $EWM_SOCKET, else");
	puts("                                  "
	     "$XDG_RUNTIME_DIR/ewm-<display>.sock,");
	puts("                                  else "
	     "/tmp/ewm-<uid>-<display>.sock");
	puts("");
}

int main(int argc, char *argv[]) {
	const char *prog_name   = argv[0];
	const char *socket_path = NULL;
	char default_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
	unsigned long win = 0;
	int i             = 1;

	for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
		if (strcmp(argv[i], "--ignore-reply") == 0)
			ignore_reply = 1;
		else if (strcmp(argv[i], "--socket") == 0) {
			if (++i >= argc)
				usage_error(prog_name, "Expected a path after --socket");
			socket_path = argv[i];
		} else
			usage_error(prog_name, "Invalid option '%s'", argv[i]);
	}

	if (i >= argc)
		usage_error(prog_name, "Expected an argument, got none");

	const char *command = argv[i++];

	// Validate arguments before connecting
	if (strcmp(command, "help") == 0) {
		print_usage(prog_name);
		return 0;
	} else if (strcmp(command, "run_command") == 0) {
		if (i >= argc)
			usage_error(prog_name, "No command specified");
	} else if (strcmp(command, "get_dwm_client") == 0) {
		if (i >= argc)
			usage_error(prog_name, "Expected the window id");
		if (!parse_unsigned_int(argv[i], &win))
			usage_error(prog_name, "Expected unsigned integer argument");
	} else if (strcmp(command, "subscribe") == 0) {
		if (i >= argc)
			usage_error(prog_name, "Expected event name");
	} else if (strcmp(command, "get_monitors") != 0
	           && strcmp(command, "get_tags") != 0
	           && strcmp(command, "get_layouts") != 0) {
		usage_error(prog_name, "Invalid argument '%s'", command);
	}

	if (!socket_path) {
		if (ipc_socket_path(default_path, sizeof(default_path)) < 0)
			fatal(1, "default socket path is too long");
		socket_path = default_path;
	}
	connect_to_socket(socket_path);

	if (strcmp(command, "run_command") == 0) {
		// Command arguments are everything after command name
		run_command(argv[i], argv + i + 1, argc - i - 1);
	} else if (strcmp(command, "get_monitors") == 0) {
		get_monitors();
	} else if (strcmp(command, "get_tags") == 0) {
		get_tags();
	} else if (strcmp(command, "get_layouts") == 0) {
		get_layouts();
	} else if (strcmp(command, "get_dwm_client") == 0) {
		get_dwm_client(win);
	} else if (strcmp(command, "subscribe") == 0) {
		for (int j = i; j < argc; j++)
			if (subscribe(argv[j]) < 0)
				return 1;
		// Keep listening for events forever
		while (1)
			handle_socket_reply(1);
	}

	return 0;
}
