#ifndef IPC_H_
#define IPC_H_

#include <stdint.h>
#include <sys/epoll.h>
#include <yajl/yajl_gen.h>

#include "IPCClient.h"
#include "ipc-protocol.h"

// clang-format off
#define IPCCOMMAND(FUNC, ARGC, TYPES)                                          \
  { #FUNC, {FUNC }, ARGC, (ArgType[ARGC])TYPES }
// clang-format on

typedef enum ArgType {
	ARG_TYPE_NONE  = 0,
	ARG_TYPE_UINT  = 1,
	ARG_TYPE_SINT  = 2,
	ARG_TYPE_FLOAT = 3,
	ARG_TYPE_PTR   = 4,
	ARG_TYPE_STR   = 5
} ArgType;

/**
 * An IPCCommand function can have either of these function signatures
 */
typedef union ArgFunction {
	void (*single_param)(const Arg *);
	void (*array_param)(const Arg *, int);
} ArgFunction;

typedef struct IPCCommand {
	char *name;
	ArgFunction func;
	unsigned int argc;
	ArgType *arg_types;
} IPCCommand;

typedef struct IPCParsedCommand {
	char *name;
	Arg *args;
	ArgType *arg_types;
	unsigned int argc;
} IPCParsedCommand;

/**
 * Initialize the IPC socket and the IPC module. Fails if another instance is
 * already listening on socket_path; a stale socket file is replaced.
 *
 * @param socket_path Path to create the socket at
 * @param p_epoll_fd File descriptor for epoll
 * @param commands Address of IPCCommands array defined in config.h
 * @param commands_len Length of commands[] array
 *
 * @return int The file descriptor of the socket if it was successfully
 * created, -1 otherwise
 */
int ipc_init(const char *socket_path, const int p_epoll_fd,
             IPCCommand commands[], const int commands_len);

/**
 * Uninitialize the socket and module: drop all clients, close and unlink the
 * socket, free allocated memory and restore static variables to their state
 * before ipc_init
 */
void ipc_cleanup(void);

/**
 * Get the file descriptor of the IPC socket
 *
 * @return int File descriptor of IPC socket, -1 if socket not created.
 */
int ipc_get_sock_fd(void);

/**
 * Get address to IPCClient with specified file descriptor
 *
 * @param fd File descriptor of IPC Client
 *
 * @return Address to IPCClient with specified file descriptor, NULL
 * otherwise
 */
IPCClient *ipc_get_client(int fd);

/**
 * Check if an IPC client exists with the specified file descriptor
 *
 * @param fd File descriptor
 *
 * @return int 1 if client exists, 0 otherwise
 */
int ipc_is_client_registered(int fd);

/**
 * Immediately disconnect an IPCClient: stop polling its file descriptor,
 * close it, remove the client from the list of known clients and free it.
 * The client is always freed.
 *
 * @param c Address of IPCClient
 *
 * @return The result of close() on the client's file descriptor
 */
int ipc_drop_client(IPCClient *c);

/**
 * Accept an IPC Client requesting to connect to the socket and add it to the
 *   list of clients. The client socket is made non-blocking and
 *   close-on-exec.
 *
 * @return File descriptor of new client, -1 on error
 */
int ipc_accept_client(void);

/**
 * Read available data of the client's next incoming message without
 * blocking. Partial messages are kept in the client and completed by later
 * calls.
 *
 * @param c Address of IPCClient
 * @param msg_type Assigned the message type of a complete message
 * @param msg_size Assigned the payload size of a complete message
 * @param msg Assigned the payload of a complete message, NUL-terminated
 *   (msg_size + 1 bytes). This must be freed using free().
 *
 * @return 1 if a complete message was received, 0 if the message is not
 * complete yet, -1 if the client closed its end of the connection, -2 on a
 * read error or invalid message (the client should be dropped).
 */
int ipc_read_client(IPCClient *c, uint8_t *msg_type, uint32_t *msg_size,
                    char **msg);

/**
 * Write as much of the pending buffer of the client to the client's socket
 * as possible without blocking.
 *
 * @param c Client whose buffer to write
 *
 * @return Number of bytes written >= 0, -1 on a write error (the client
 * should be dropped). errno will still be set from the send operation.
 */
ssize_t ipc_write_client(IPCClient *c);

/**
 * Prepare a message in the specified client's buffer. If the client's pending
 * output would exceed its limit the client is scheduled to be dropped
 * instead.
 *
 * @param c Client to prepare message for
 * @param msg_type Type of message to prepare
 * @param msg_size Size of the message in bytes. Must not exceed
 *   IPC_MAX_MESSAGE_SIZE
 * @param msg Message to prepare (not including header). This pointer can be
 *   freed after the function invocation.
 */
void ipc_prepare_send_message(IPCClient *c, const IPCMessageType msg_type,
                              const uint32_t msg_size, const char *msg);

/**
 * Prepare an error message in the specified client's buffer
 *
 * @param c Client to prepare message for
 * @param msg_type Type of message
 * @param format Format string following vsprintf
 * @param ... Arguments for format string
 */
void ipc_prepare_reply_failure(IPCClient *c, IPCMessageType msg_type,
                               const char *format, ...);

/**
 * Prepare a success message in the specified client's buffer
 *
 * @param c Client to prepare message for
 * @param msg_type Type of message
 */
void ipc_prepare_reply_success(IPCClient *c, IPCMessageType msg_type);

/**
 * Send a tag_change_event to all subscribers. Should be called only when
 * there has been a tag state change.
 *
 * @param mon_num The index of the monitor (Monitor.num property)
 * @param old_state The old tag state
 * @param new_state The new (now current) tag state
 */
void ipc_tag_change_event(const int mon_num, TagState old_state,
                          TagState new_state);

/**
 * Send a client_focus_change_event to all subscribers. Should be called only
 * when the client focus changes.
 *
 * @param mon_num The index of the monitor (Monitor.num property)
 * @param old_client The old DWM client selection (Monitor.lastsel), may be
 *   NULL
 * @param new_client The new (now current) DWM client selection, may be NULL
 */
void ipc_client_focus_change_event(const int mon_num, Client *old_client,
                                   Client *new_client);

/**
 * Send a layout_change_event to all subscribers. Should be called only
 * when there has been a layout change.
 *
 * @param mon_num The index of the monitor (Monitor.num property)
 * @param old_symbol The old layout symbol
 * @param old_layout Address to the old Layout
 * @param new_symbol The new (now current) layout symbol
 * @param new_layout Address to the new Layout
 */
void ipc_layout_change_event(const int mon_num, const char *old_symbol,
                             const Layout *old_layout, const char *new_symbol,
                             const Layout *new_layout);

/**
 * Send a monitor_focus_change_event to all subscribers. Should be called only
 * when the monitor focus changes.
 *
 * @param last_mon_num The index of the previously selected monitor
 * @param new_mon_num The index of the newly selected monitor
 */
void ipc_monitor_focus_change_event(const int last_mon_num,
                                    const int new_mon_num);

/**
 * Send a focused_title_change_event to all subscribers. Should only be called
 * if a selected client has a title change.
 *
 * @param mon_num Index of the client's monitor
 * @param client_id Window XID of client
 * @param old_name Old name of the client window
 * @param new_name New name of the client window
 */
void ipc_focused_title_change_event(const int mon_num, const Window client_id,
                                    const char *old_name,
                                    const char *new_name);

/**
 * Send a focused_state_change_event to all subscribers. Should only be called
 * if a selected client has a state change.
 *
 * @param mon_num Index of the client's monitor
 * @param client_id Window XID of client
 * @param old_state Old state of the client
 * @param new_state New state of the client
 */
void ipc_focused_state_change_event(const int mon_num, const Window client_id,
                                    const ClientState *old_state,
                                    const ClientState *new_state);

/**
 * Compare the current state of all monitors (mons, selmon, lastselmon
 * globals) with the last state seen and send events for any changes to
 * their subscribers. Updates Monitor.tagstate, Monitor.lastsel,
 * Monitor.lastltsymbol, Monitor.lastlt, Client.prevstate and lastselmon.
 * Monitor.lastsel and lastselmon may be NULL.
 */
void ipc_send_events(void);

/**
 * Handle an epoll event caused by a registered IPC client. Read, process, and
 * reply to any received messages, then write the pending buffer if the
 * client is ready to receive. Input is processed before EPOLLHUP/EPOLLERR,
 * which drop the client, as do read/write errors and protocol violations.
 *
 * @param ev Associated epoll event returned by epoll_wait
 *
 * @return 0 if the event was successfully handled, -1 on any error receiving
 * or handling incoming messages or on an unregistered client.
 */
int ipc_handle_client_epoll_event(struct epoll_event *ev);

/**
 * Handle an epoll event caused by the IPC socket. This function only handles
 * an EPOLLIN event indicating a new client requesting to connect to the
 * socket.
 *
 * @param ev Associated epoll event returned by epoll_wait
 *
 * @return The file descriptor of the accepted client, -1 if not an EPOLLIN
 * event or if a new IPC client connection request could not be accepted.
 */
int ipc_handle_socket_epoll_event(struct epoll_event *ev);

#endif /* IPC_H_ */
