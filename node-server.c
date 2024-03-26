#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#include "main.h"

// Node server socket
int ns_socket;

// Node server address
static struct addrinfo *ns_addrinfo;

// The node list returned by the node server
NodeArray node_arr;

enum NodeListAction node_list_action;

int init_ns(char *ns_addr_str, char *ns_port_str) {
	ns_socket = socket(AF_INET, SOCK_DGRAM, 0); // UDP over IPv4
	if (ns_socket == -1)
		error("Couldn't create UDP socket: %s\n", strerror(errno));

	struct addrinfo hints = {0};
	hints.ai_family = AF_INET;      // IPv4
	hints.ai_socktype = SOCK_DGRAM; // UDP socket

	int errcode = getaddrinfo(ns_addr_str, ns_port_str, &hints, &ns_addrinfo);
	if (errcode != 0)
		error("Couldn't get the node server address: %s\n", gai_strerror(errcode));

	return ns_socket;
}


void send_ns_message(const char *message, int length) {
	vv_printf("Sending message to node server: %s\n", message);
	ssize_t n = sendto(ns_socket, message, length, 0, ns_addrinfo->ai_addr, ns_addrinfo->ai_addrlen);
	if (n == -1) {
		error("Couldn't send message to node server: %s\n", strerror(errno));
	}
}

static void parse_node_list(char *message, ssize_t length, bool chord_mode) {
	// Check for a newline after "NODESLIST <ring id>".
	// This doesn't guarantee the header is right but it will only happen if it's malformed or missing.
	if (message[13] != '\n')
		error("Malformed node list header.\n");

	if (message[length - 1] != '\n')
		error("Node list doesn't end with a line feed character.\n");

	int n = 0;
	for (int i = 14; i < length;) {
		int successful_assignments = sscanf(message + i, ""NODE_ID_IN" %15s %5s\n", &node_arr.nodes[n].id, node_arr.nodes[n].ip_addr, node_arr.nodes[n].tcp_port);
		if (successful_assignments != 3) {
			error("Failed to parse node list.\n");
		}
		if (!chord_mode || (self.id != node_arr.nodes[n].id && find_connection_by_node_id(node_arr.nodes[n].id) == NULL)) {
			n++;
		}

		// Find the next line
		while (message[i] != '\n')
			i++;
		i++;
	}
	node_arr.length = n;
}

static void print_node_table(void) {
	printf(
	    "\
+------------------------------+\n\
| ID | IP address      | Port  |\n\
+------------------------------+\n\
");
	for (int i = 0; i < node_arr.length; i++) {
		Node *node = &node_arr.nodes[i];
		printf("| "NODE_ID_OUT" | %-15s | %-5s |\n", node->id, node->ip_addr, node->tcp_port);
	}
	printf("+------------------------------+\n");
}

void request_node_list(char *ring_id_str) {
	char nodes_msg[10];
	sprintf(nodes_msg, "NODES %s", ring_id_str);
	send_ns_message(nodes_msg, 10);

	fd_set set;
	FD_ZERO(&set);
	FD_SET(ns_socket, &set);
	int ret = select(FD_SETSIZE, &set, NULL, NULL, &(struct timeval) { .tv_sec = 1 });
	if (ret == -1) {
		error("select() error: %s\n", strerror(errno));
	}
	if (ret == 0) {
		printf("Timeout while waiting for the node list response from the node server. Connection aborted.\n");
		leave_ring();
		return;
	}

	char ns_response_buffer[MAX_UDP_SIZE];
	ssize_t len = recvfrom(ns_socket, ns_response_buffer, MAX_UDP_SIZE, 0, NULL, 0);
	if (strncmp(ns_response_buffer, "NODESLIST ", 10) == 0) {
		parse_node_list(ns_response_buffer, len, node_list_action == CHORD_ACTION);

		if (node_list_action == JOIN_ACTION) {
			if (node_arr.length == 0) {
				printf("There are no nodes in node list for the ring %s. We are the only node in the ring.\n", ring_id_str);
				copy_node(&succ, &self);
				copy_node(&second_succ, &self);
				init_routing();
				on_join_end();
				return;
			}

			printf("Nodes currently in the ring:\n");
			print_node_table();

			// Check whether the given ID is already in use and change it if needed
			for (int i = 0; i < node_arr.length; i++) {
				if (node_arr.nodes[i].id == self.id) {
					NodeID new_id;
					for (new_id = 0; new_id < 100; new_id++) {
						for (int j = 0; j < node_arr.length; j++) {
							if (node_arr.nodes[j].id == new_id) {
								goto try_next_id;
							}
						}
						// `new_id` now contains an unused node ID
						break;

						try_next_id:;
					}
					if (new_id >= 100) {
						printf("No available node IDs left in the ring. Joining procedure aborted.\n");
						connection_state = DISCONNECTED;
					}
					warn("The node ID "NODE_ID_OUT" is already in use, so "NODE_ID_OUT" will be used instead.\n", self.id, new_id);
					self.id = new_id;
					break;
				}
			}
			connection_state = AWAITING_USER_SELECTION;
			input_state = JOIN_NODE_SELECTION;

		} else if (node_list_action == CHORD_ACTION) {
			if (node_arr.length == 0) {
				printf("There are no nodes to which we can create a chord.\n");
				return;
			}
			printf("Nodes you can create a chord to:\n");
			print_node_table();
			input_state = CHORD_NODE_SELECTION;
		}

		printf("Please select a node ID to use as the %s: ", node_list_action == JOIN_ACTION ? "successor" : "chord neighbor");
		fflush(stdout);
	}
}
