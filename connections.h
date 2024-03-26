#ifndef CONNECTIONS_H
#define CONNECTIONS_H

#include <sys/select.h>

#include "main.h"

typedef struct Connection {
	// The socket file descriptor.
	int socket;
	// The node ID. Equal to `-1` if it isn't yet known.
	NodeID node_id;
	// See read-lines.c
	char buffer[MAX_NODE_MESSAGE_SIZE];
	int buffer_index;
	// The IP address of the remote host.
	char ip_addr[IPV4_ADDR_STR_SIZE];
	// The destination TCP port. Only valid for outbound connections.
	char tcp_port[TCP_PORT_STR_SIZE];
} Connection;

#define MAX_INBOUND_CHORDS (MAX_NODES - 2)
#define MAX_CONNECTIONS (MAX_INBOUND_CHORDS + 4)
extern struct Connection connections[MAX_CONNECTIONS];
extern struct Connection *new_node_conn, *pred_conn, *succ_conn, *outbound_chord_conn;

void init_connections_array(void);
struct Connection *add_connection(int socket);
int close_connection(struct Connection *connection);
struct Connection *find_connection_by_socket(int socket);
struct Connection *find_connection_by_node_id(NodeID node_id);
bool is_inbound_chord(struct Connection *conn);
int conn_printf(int socket, const char *format, ...);

#endif
