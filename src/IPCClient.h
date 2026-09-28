#ifndef IPC_CLIENT_H_
#define IPC_CLIENT_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>

#include "ipc-protocol.h"

typedef struct IPCClient IPCClient;
/**
 * This structure contains the details of an IPC Client and pointers for a
 * linked list
 */
struct IPCClient {
	int fd;
	int subscriptions;

	/* Pending outgoing data */
	char *buffer;
	uint32_t buffer_size;

	/* Incoming message being assembled across EPOLLIN events */
	uint8_t in_header[sizeof(dwm_ipc_header_t)];
	uint32_t in_header_len;  /* header bytes received so far */
	char *in_payload;        /* in_size + 1 bytes once header is complete */
	uint32_t in_payload_len; /* payload bytes received so far */
	uint32_t in_size;        /* payload size announced by the header */
	uint8_t in_type;         /* message type announced by the header */

	int eof;     /* peer closed its write side; drop once output flushed */
	int closing; /* scheduled for removal; no further I/O */

	struct epoll_event event;
	IPCClient *next;
	IPCClient *prev;
};

typedef IPCClient *IPCClientList;

/**
 * Allocate memory for new IPCClient with the specified file descriptor and
 * initialize struct.
 *
 * @param fd File descriptor of IPC client
 *
 * @return Address to allocated IPCClient struct, NULL on allocation failure
 */
IPCClient *ipc_client_new(int fd);

/**
 * Add an IPC Client to the specified list
 *
 * @param list Address of the list to add the client to
 * @param nc Address of the IPCClient
 */
void ipc_list_add_client(IPCClientList *list, IPCClient *nc);

/**
 * Remove an IPCClient from the specified list
 *
 * @param list Address of the list to remove the client from
 * @param c Address of the IPCClient
 */
void ipc_list_remove_client(IPCClientList *list, IPCClient *c);

/**
 * Get an IPCClient from the specified IPCClient list
 *
 * @param list List to search
 * @param fd File descriptor of the IPCClient
 *
 * @return Address of the IPCClient, NULL if not found
 */
IPCClient *ipc_list_get_client(IPCClientList list, int fd);

#endif // IPC_CLIENT_H_
