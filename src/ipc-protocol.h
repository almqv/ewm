/* See LICENSE file for copyright and license details.
 *
 * Wire protocol shared by the ewm IPC server (ipc.c) and the ewm-msg client
 * (ewm-msg.c). This header must stay dependency-free (libc only).
 *
 * Every message is a dwm_ipc_header_t immediately followed by `size` bytes
 * of payload. Payloads sent by the server are JSON and include a trailing
 * NUL byte (counted in `size`).
 */
#ifndef IPC_PROTOCOL_H_
#define IPC_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// clang-format off
#define IPC_MAGIC     "DWM-IPC"
#define IPC_MAGIC_ARR { 'D', 'W', 'M', '-', 'I', 'P', 'C' }
#define IPC_MAGIC_LEN 7 // Not including null char
// clang-format on

/* Maximum payload size in bytes accepted by either side */
#define IPC_MAX_MESSAGE_SIZE 1000000

#define IPC_EVENT_NAME_TAG_CHANGE           "tag_change_event"
#define IPC_EVENT_NAME_CLIENT_FOCUS_CHANGE  "client_focus_change_event"
#define IPC_EVENT_NAME_LAYOUT_CHANGE        "layout_change_event"
#define IPC_EVENT_NAME_MONITOR_FOCUS_CHANGE "monitor_focus_change_event"
#define IPC_EVENT_NAME_FOCUSED_TITLE_CHANGE "focused_title_change_event"
#define IPC_EVENT_NAME_FOCUSED_STATE_CHANGE "focused_state_change_event"

typedef enum IPCMessageType {
	IPC_TYPE_RUN_COMMAND    = 0,
	IPC_TYPE_GET_MONITORS   = 1,
	IPC_TYPE_GET_TAGS       = 2,
	IPC_TYPE_GET_LAYOUTS    = 3,
	IPC_TYPE_GET_DWM_CLIENT = 4,
	IPC_TYPE_SUBSCRIBE      = 5,
	IPC_TYPE_EVENT          = 6
} IPCMessageType;

typedef enum IPCEvent {
	IPC_EVENT_TAG_CHANGE           = 1 << 0,
	IPC_EVENT_CLIENT_FOCUS_CHANGE  = 1 << 1,
	IPC_EVENT_LAYOUT_CHANGE        = 1 << 2,
	IPC_EVENT_MONITOR_FOCUS_CHANGE = 1 << 3,
	IPC_EVENT_FOCUSED_TITLE_CHANGE = 1 << 4,
	IPC_EVENT_FOCUSED_STATE_CHANGE = 1 << 5
} IPCEvent;

typedef enum IPCSubscriptionAction {
	IPC_ACTION_UNSUBSCRIBE = 0,
	IPC_ACTION_SUBSCRIBE   = 1
} IPCSubscriptionAction;

/**
 * Every IPC packet starts with this structure
 */
typedef struct dwm_ipc_header {
	uint8_t magic[IPC_MAGIC_LEN];
	uint32_t size;
	uint8_t type;
} __attribute__((packed)) dwm_ipc_header_t;

/**
 * Resolve the IPC socket path into buf:
 *   $EWM_SOCKET if set and non-empty, else
 *   $XDG_RUNTIME_DIR/ewm-<display>.sock if XDG_RUNTIME_DIR is set, else
 *   /tmp/ewm-<uid>-<display>.sock
 * where <display> is $DISPLAY ("0" if unset) with '/' replaced by '_'.
 *
 * Returns 0 on success, -1 if the path does not fit in len bytes.
 */
static inline int ipc_socket_path(char *buf, size_t len) {
	const char *env = getenv("EWM_SOCKET");
	const char *display, *runtime;
	size_t start, dlen, i;
	int n;

	if (env && *env) {
		n = snprintf(buf, len, "%s", env);
		return (n < 0 || (size_t) n >= len) ? -1 : 0;
	}

	display = getenv("DISPLAY");
	if (!display || !*display)
		display = "0";
	runtime = getenv("XDG_RUNTIME_DIR");

	if (runtime && *runtime)
		n = snprintf(buf, len, "%s/ewm-", runtime);
	else
		n = snprintf(buf, len, "/tmp/ewm-%lu-", (unsigned long) getuid());
	if (n < 0 || (size_t) n >= len)
		return -1;

	start = (size_t) n;
	dlen  = strlen(display);
	n     = snprintf(buf + start, len - start, "%s.sock", display);
	if (n < 0 || (size_t) n >= len - start)
		return -1;

	for (i = start; i < start + dlen; i++)
		if (buf[i] == '/')
			buf[i] = '_';

	return 0;
}

#endif /* IPC_PROTOCOL_H_ */
