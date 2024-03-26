#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>

#include "main.h"

struct Connection connections[MAX_CONNECTIONS];
struct Connection *new_node_conn, *pred_conn, *succ_conn, *outbound_chord_conn;

extern fd_set select_inputs;

void init_connections_array(void) {
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		connections[i].socket = -1;
	}
}

struct Connection *add_connection(int socket) {
	FD_SET(socket, &select_inputs);
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		struct Connection *conn = &connections[i];
		if (conn->socket == -1) {
			conn->socket = socket;
			conn->node_id = -1;
			conn->buffer_index = 0;
			conn->ip_addr[0] = '\0';
			conn->tcp_port[0] = '\0';
			return conn;
		}
	}
	error("add_connection(): no space left!\n");
}

int close_connection(struct Connection *connection) {
	if (connection == NULL || connection->socket == -1) return 0;
	int ret = close(connection->socket);
	FD_CLR(connection->socket, &select_inputs);
	connection->socket = -1;
	if (new_node_conn == connection) new_node_conn = NULL;
	if (pred_conn == connection) pred_conn = NULL;
	if (succ_conn == connection) succ_conn = NULL;
	if (outbound_chord_conn == connection) outbound_chord_conn = NULL;
	return ret;
}

struct Connection *find_connection_by_socket(int socket) {
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i].socket == socket) {
			return &connections[i];
		}
	}
	return NULL;
}
struct Connection *find_connection_by_node_id(NodeID node_id) {
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i].socket != -1 && connections[i].node_id == node_id) {
			return &connections[i];
		}
	}
	return NULL;
}

bool is_inbound_chord(struct Connection *conn) {
	return conn != new_node_conn && conn != pred_conn && conn != succ_conn && conn != outbound_chord_conn;
}

int conn_printf(int socket, const char *format, ...) {
	va_list args;
	va_start(args, format);
	char message[MAX_NODE_MESSAGE_SIZE];
	vsnprintf(message, MAX_NODE_MESSAGE_SIZE, format, args);
	if (verbose_level >= 2) {
		struct Connection *conn = find_connection_by_socket(socket);
		if (conn != NULL && conn->node_id != -1) {
			vv_printf("Sending message to node "NODE_ID_OUT": %s", conn->node_id, message);
		} else {
			vv_printf("Sending message to the new client node: %s", message);
		}
	}
	int ret = dprintf(socket, "%s", message);
	if (ret < 0) {
		handle_broken_socket(socket);
	}
	va_end(args);
	return ret;
}
