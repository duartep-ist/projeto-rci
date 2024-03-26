#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "main.h"
#include "connections.h"
#include "util.h"
#include "routing.h"

enum ConnectionState connection_state = DISCONNECTED;

// The successor's ID is also stored in `succ.id`
Node self, succ, second_succ;

// CONNECTING only
// Whether we are waiting for the successor to tell us who our second successor is via a SUCC message
bool awaiting_succ;
// Whether we are waiting for the predecessor to tell us what its ID is via a PRED message
bool awaiting_pred;

// This is an empty string if we connected to another node or another node connected to us using the direct join command.
char ring_id_str[4];


struct Connection *connect_to_node(struct Node *node) {
	int s = socket(AF_INET, SOCK_STREAM, 0); // TCP over IPv4
	if (s == -1) {
		printf("Connection error: Couldn't create TCP socket to connect to node: %s\n", strerror(errno));
		return NULL;
	}

	struct Connection *conn = add_connection(s);
	conn->node_id = node->id;
	strcpy(conn->ip_addr, node->ip_addr);
	strcpy(conn->tcp_port, node->tcp_port);

	struct addrinfo hints = {
		.ai_family = AF_INET,      // IPv4
		.ai_socktype = SOCK_STREAM // TCP
	};

	struct addrinfo *ai;

	int ret;
	ret = getaddrinfo(node->ip_addr, node->tcp_port, &hints, &ai);
	if (ret != 0) {
		printf("Connection error: Invalid node IP address: %s\n", gai_strerror(ret));
		return NULL;
	}

	ret = connect(conn->socket, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	if (ret != 0) {
		printf("Couldn't connect to the node (%s:%s) via TCP: %s\n", node->ip_addr, node->tcp_port, strerror(errno));
		return NULL;
	}

	return conn;
}

static void pred_timeout(void) {
	printf("The predecessor took too long to connect. Left the ring.\n");
	leave_ring();
}

// Leaves the ring or aborts the joining procedure
void leave_ring(void) {
	if (connection_state == CONNECTED && ring_id_str[0] != '\0') {
		char unreg_msg[13];
		sprintf(unreg_msg, "UNREG %s "NODE_ID_OUT"", ring_id_str, self.id);
		send_ns_message(unreg_msg, 12);
	}

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		close_connection(&connections[i]);
	}

	connection_state = DISCONNECTED;
}

void join_ring(void) {
	init_routing();

	connection_state = CONNECTING;
	awaiting_succ = true;
	awaiting_pred = true;

	pred_conn = NULL;
	outbound_chord_conn = NULL;
	new_node_conn = NULL;
	succ_conn = connect_to_node(&succ);
	if (succ_conn == NULL) {
		printf("Join procedure aborted.\n");
		leave_ring();
		return;
	}

	if (
		conn_printf(succ_conn->socket, "ENTRY "NODE_ID_OUT" %s %s\n", self.id, self.ip_addr, self.tcp_port) < 0 ||
		send_shortest_paths(succ_conn) < 0
	) {
		return;
	}

	set_timeout(1000, pred_timeout);

	v_printf("Connected to the successor and sent the ENTRY message.\n");
}

// Executed when we recieve both the PRED and SUCC messages
void on_join_end(void) {
	printf("Join successeful. We are now in a ring.\n");
	connection_state = CONNECTED;

	// Register to the node server unless direct join was used
	if (ring_id_str[0] != '\0') {
		char reg_msg[33];
		int length = sprintf(reg_msg, "REG %s "NODE_ID_OUT" %s %s", ring_id_str, self.id, self.ip_addr, self.tcp_port);
		send_ns_message(reg_msg, length);
	}
}

void create_outbound_chord(struct Node *node) {
	if (find_connection_by_node_id(node->id) != NULL) {
		printf("We are already connected to node "NODE_ID_OUT". No chord was created.\n", node->id);
		// return;
	}

	v_printf("Establishing a chord with the node with ID "NODE_ID_OUT" at %s:%s.\n", node->id, node->ip_addr, node->tcp_port);
	outbound_chord_conn = connect_to_node(node);
	if (outbound_chord_conn == NULL) {
		printf("Chord connection procedure aborted.\n");
		return;
	}

	if (
		conn_printf(outbound_chord_conn->socket, "CHORD "NODE_ID_OUT"\n", self.id) < 0 ||
		send_shortest_paths(outbound_chord_conn) < 0
	) {
		printf("Couldn't write to the outbound chord socket. Chord connection procedure aborted.\n");
		fflush(stdout);
		return;
	}

	printf("Successfully established the chord with the node with ID "NODE_ID_OUT".\n", node->id);
	fflush(stdout);
}

static void remove_neighbor_connection(NodeID node_id) {
	// Update the routing table if there are no longer any direct connections to a node.
	if (node_id != -1 && find_connection_by_node_id(node_id) == NULL) {
		remove_routing_neighbor(node_id);
	}
}


// If `message` is a routing or application message, this handles it and returns `true`. Otherwise, it returns false.
static bool handle_message_from_any_node(char *message, struct Connection *conn) {
	// ROUTE message with path
	{
		// "ROUTE 10 30 10-20-30" is parsed as:
		//     neighbor_id = 10
		//     recipient_id = 30
		//     path = { .hop_count = 2, .nodes = { 10, 20 } }

		NodeID neighbor_id;
		NodeID recipient_id;
		char path_str[MAX_PATH_STR_SIZE];
		if (sscanf(message, "ROUTE "NODE_ID_IN" "NODE_ID_IN" %" STR(MAX_PATH_STR_LENGTH) "s", &neighbor_id, &recipient_id, path_str) == 3) {
			if (neighbor_id != conn->node_id) {
				warn("Received ROUTE message from node "NODE_ID_OUT" with wrong neighbor ID. Ignoring.\n", conn->node_id);
				return true;
			}
			if (neighbor_id == self.id) {
				warn("Received ROUTE message from a neighbor which identified itself with our ID ("NODE_ID_OUT"). Ignoring.\n", self.id);
				return true;
			}

			struct Path path;

			// Parse the path string (modifies `message` in-place)
			int id_start = 0;
			int i = 0;
			for (int c = 0; ; c++) {
				if (path_str[c] == '-' || path_str[c] == '\0') {
					bool end = path_str[c] == '\0';
					path_str[c] = '\0';
					int id = atoi(path_str + id_start);
					if (id < 0) {
						warn("Received invalid path string in ROUTE message. Ignoring the message.\n");
						return true;
					}
					path.nodes[i] = id;
					i++;
					if (end) break;
					if (false && i >= MAX_NODES) {
						warn("Received too long path in ROUTE message. Ignoring the message.\n");
						return true;
					}
					id_start = c + 1;
				} else if (!isdigit(path_str[c]) || c > id_start + 1) {
					warn("Received invalid path string in ROUTE message. Ignoring the message.\n");
					return true;
				}
			}
			path.hop_count = i - 1;

			if (neighbor_id == recipient_id && path.hop_count != 0) {
				warn("Received ROUTE message from node "NODE_ID_OUT" with hops between the neighbor and itself. Ignoring.\n", conn->node_id);
				return true;
			}
			if (neighbor_id != recipient_id && path.hop_count == 0) {
				warn("Received ROUTE message from node "NODE_ID_OUT" with no hops between different nodes. Ignoring.\n", conn->node_id);
				return true;
			}

			update_routing_and_announce_given_new_path(neighbor_id, recipient_id, &path);
			return true;
		}
	}

	// ROUTE message without path
	{
		NodeID neighbor_id;
		NodeID recipient_id;
		if (sscanf(message, "ROUTE "NODE_ID_IN" "NODE_ID_IN"", &neighbor_id, &recipient_id) == 2) {
			if (neighbor_id == self.id) {
				warn("Received ROUTE message from a neighbor which identified itself with our ID ("NODE_ID_OUT"). Ignoring.\n", self.id);
				return true;
			}
			if (neighbor_id != conn->node_id) {
				warn("Received ROUTE message from neigbor "NODE_ID_OUT" with wrong neighbor ID. Ignoring.\n", conn->node_id);
				return true;
			}
			if (recipient_id == self.id) {
				warn("Our neighbor "NODE_ID_OUT" said it has no valid path to us. This is impossible. Ignoring.\n", conn->node_id);
				return true;
			}

			update_routing_and_announce_given_new_path(neighbor_id, recipient_id, NULL);
			return true;
		}
	}

	// CHAT message
	{
		NodeID sender_id;
		NodeID recipient_id;
		int chat_message_start = -1;
		if (
			sscanf(message, "CHAT "NODE_ID_IN" "NODE_ID_IN"%n", &sender_id, &recipient_id, &chat_message_start) == 2 &&
			chat_message_start != -1 &&
			message[chat_message_start] == ' '
		) {
			// Make sure we get the entire message even if it starts with a whitespace character
			char *chat_message = message + chat_message_start + 1;
			if (recipient_id == self.id) {
				printf("Node "NODE_ID_OUT" said: \"%s\"\n", sender_id, chat_message);
			} else {
				forward_message(sender_id, recipient_id, chat_message);
			}

			return true;
		}
	}

	return false;
}

// Handles messages from the successor node
static void handle_message_from_succ(char *message) {
	vv_printf("Received message from successor: %s\n", message);

	// Extrair informações do segundo sucessor da mensagem
	NodeID id;
	char ip_addr[16];
	char tcp_port[6];

	if (sscanf(message, "SUCC "NODE_ID_IN" %15s %5s", &id, ip_addr, tcp_port) == 3) {
		if (id == succ.id) {
			warn("Successor said it is its own successor. Ignoring.");
			return;
		}

		// Armazenar as informações do segundo sucessor
		second_succ.id = id;
		strcpy(second_succ.ip_addr, ip_addr);
		strcpy(second_succ.tcp_port, tcp_port);

		// Testar os dados do segundo sucessor
		v_printf("Received second successor info.\n");

		awaiting_succ = false;
		if (connection_state == CONNECTING) {
			if (!awaiting_pred) {
				on_join_end();
			}
		} else if (connection_state != CONNECTED) {
			warn("Received unexpected SUCC message from the successor node.\n");
		}
		return;
	}
	if (sscanf(message, "ENTRY "NODE_ID_IN" %15s %5s", &id, ip_addr, tcp_port) == 3) {
		if (id == self.id || find_connection_by_node_id(id) != NULL || id == second_succ.id) {
			warn("Currently used node ID in ENTRY message from successor. Leaving the ring.\n");
			leave_ring();
			return;
		}

		v_printf("A new node is joining the ring between me and my successor. Connecting to the new node as my successor.\n");

		if (conn_printf(pred_conn->socket, "SUCC %2d %s %s\n", id, ip_addr, tcp_port) < 0) {
			return;
		}

		close_connection(succ_conn);
		remove_neighbor_connection(succ.id);

		copy_node(&second_succ, &succ);

		succ.id = id;
		strcpy(succ.ip_addr, ip_addr);
		strcpy(succ.tcp_port, tcp_port);

		succ_conn = connect_to_node(&succ);
		if (succ_conn == NULL) {
			printf("Couldn't connect to the node joining the ring. Left the ring.\n");
			leave_ring();
			return;
		}

		if (
			conn_printf(succ_conn->socket, "PRED "NODE_ID_OUT"\n", self.id) < 0 ||
			send_shortest_paths(succ_conn) < 0
		) {
			return;
		}
		return;
	}

	if (handle_message_from_any_node(message, succ_conn)) return;

	warn("Received malformed message from the successor: \"%s\"\n", message);
}

// Handles messages from the predecessor node
static void handle_message_from_pred(char *message) {
	vv_printf("Received message from predecessor: %s\n", message);

	if (strncmp(message, "ENTRY ", 6) == 0 && connection_state == CONNECTING) {
		warn("Received ENTRY message from the predecessor. This is most likely a connection to self. Aborting the connection.\n");
		leave_ring();
		return;
	}

	if (handle_message_from_any_node(message, pred_conn)) return;

	warn("Received malformed message from the predecessor node: \"%s\"\n", message);
}

// Handles messages from a node trying to join the network
static void handle_message_from_new_node(char *message) {
	vv_printf("Received message from new client node: %s\n", message);

	NodeID id;
	char ip_addr[16];
	char tcp_port[6];

	if (sscanf(message, "ENTRY "NODE_ID_IN" %15s %5s", &id, ip_addr, tcp_port) == 3) {
		if (connection_state == DISCONNECTED || (connection_state == CONNECTED && succ.id == self.id)) {
			v_printf("Received an entry request from a node. We and the other node will be the only nodes in the ring.\n");

			new_node_conn->node_id = id;

			if (connection_state == DISCONNECTED) {
				printf("Another node tried to join a ring using this node as its successor but we're not in a ring.\n");
				close_connection(new_node_conn);
				return;
			}
			if (id == self.id) {
				printf("Another node tried to join with the same ID as this onde.\n");
				close_connection(new_node_conn);
				return;
			}

			succ.id = id;
			strcpy(succ.ip_addr, ip_addr);
			strcpy(succ.tcp_port, tcp_port);
			copy_node(&second_succ, &self);

			if (conn_printf(new_node_conn->socket, "SUCC "NODE_ID_OUT" %s %s\n", succ.id, succ.ip_addr, succ.tcp_port) < 0) {
				return;
			}

			v_printf("Connecting to the other node with ID "NODE_ID_OUT" at %s:%s.\n", succ.id, succ.ip_addr, succ.tcp_port);
			succ_conn = connect_to_node(&succ);
			if (succ_conn == NULL) {
				printf("Couldn't connect to the other node. Left the ring.\n");
				leave_ring();
				return;
			}
			if (
				conn_printf(succ_conn->socket, "PRED "NODE_ID_OUT"\n", self.id) < 0 ||
				send_shortest_paths(succ_conn) < 0
			) {
				return;
			}
			v_printf("Connected to the other node as our successor and sent the PRED message.\n");

			if (pred_conn != NULL) {
				error("Assertion failed: (pred_conn == NULL) when alone and accepting an entry request.\n");
			}
			pred_conn = new_node_conn;
			new_node_conn = NULL;
		} else if (connection_state == CONNECTED) {
			// This is the case where we aren't alone in the ring
			v_printf("Received an entry request from node "NODE_ID_OUT".\n", id);
			new_node_conn->node_id = id;

			if (
				conn_printf(new_node_conn->socket, "SUCC "NODE_ID_OUT" %s %s\n", succ.id, succ.ip_addr, succ.tcp_port) < 0 ||
				conn_printf(pred_conn->socket, "ENTRY "NODE_ID_OUT" %s %s\n", id, ip_addr, tcp_port) < 0 ||
				send_shortest_paths(new_node_conn) < 0
			) {
				return;
			}

			NodeID pred_id = pred_conn->node_id;
			close_connection(pred_conn);
			remove_neighbor_connection(pred_id);
			pred_conn = new_node_conn;
			new_node_conn = NULL;
		} else {
			v_printf("Received an entry request while connecting to the ring. Closing the connection.\n");
			close_connection(new_node_conn);
		}
	} else if (sscanf(message, "PRED "NODE_ID_IN"", &id) == 1) {
		if (connection_state == DISCONNECTED) {
			warn("Received predecessor connection while disconnected. Maybe the predecessor connected after the timeout. Closed the connection.\n");
			close_connection(new_node_conn);
			return;
		}
		if (pred_conn != NULL) {
			v_printf("Received predecessor connection while we already are connected to a predecessor. Closed the old predecessor connection.\n");
			close_connection(pred_conn);
		}

		v_printf("Our predecessor said its ID is "NODE_ID_OUT" (PRED message). We are now successfully connected.\n", id);

		struct Connection *conn = find_connection_by_node_id(id);
		if (conn != NULL && (conn == outbound_chord_conn || is_inbound_chord(conn))) {
			v_printf("Closing degenerate chord with our new predecessor.\n");
			close_connection(conn);
			remove_neighbor_connection(id);
		}

		new_node_conn->node_id = id;
		pred_conn = new_node_conn;
		new_node_conn = NULL;

		cancel_timeout();

		if (
			conn_printf(pred_conn->socket, "SUCC "NODE_ID_OUT" %s %s\n", succ.id, succ.ip_addr, succ.tcp_port) < 0 ||
			send_shortest_paths(pred_conn) < 0
		) {
			return;
		}

		awaiting_pred = false;
		if (connection_state == CONNECTING && !awaiting_succ) {
			on_join_end();
		}
	} else if (sscanf(message, "CHORD "NODE_ID_IN"", &id) == 1) {
		if (find_connection_by_node_id(id) != NULL) {
			warn("Rejected an inbound chord connection request from node "NODE_ID_OUT" because we are already connected.\n", id);
			return;
		} else {
			v_printf("Received an inbound chord connection from the node with ID "NODE_ID_OUT". We are now successfully connected.\n", id);
			new_node_conn->node_id = id;
		}

		if (send_shortest_paths(new_node_conn) < 0) {
			return;
		}

		new_node_conn = NULL;
	} else {
		warn("Received malformed message from the client node: \"%s\"\n", message);
	}
}

static void handle_message_from_chord(char *message, struct Connection *conn) {
	vv_printf("Received message from chord with node "NODE_ID_OUT": %s\n", conn->node_id, message);

	if (handle_message_from_any_node(message, conn)) return;

	warn("Received malformed message from the chord neighbor "NODE_ID_OUT": \"%s\"\n", conn->node_id, message);
}

static void handle_broken_succ_socket(void) {
	if (connection_state != CONNECTED) {
		printf("The successor closed the connection before we finished joining the ring. Aborting the join procedure.\n");
		leave_ring();
		return;
	}

	awaiting_succ = true;

	copy_node(&succ, &second_succ);

	if (succ.id == self.id) {
		v_printf("The other node left. We are now alone in the ring.\n");
		return;
	}

	if (pred_conn == NULL || pred_conn->node_id == -1) {
		warn("Our successor left while we were waiting for the new predecessor to connect. Left the ring.\n");
		leave_ring();
		return;
	}

	v_printf("Our successor left. Connecting to the second successor.\n");

	if (conn_printf(pred_conn->socket, "SUCC "NODE_ID_OUT" %s %s\n", succ.id, succ.ip_addr, succ.tcp_port) < 0) {
		return;
	}

	struct Connection *conn = find_connection_by_node_id(succ.id);
	if (conn != NULL && (conn == outbound_chord_conn || is_inbound_chord(conn))) {
		v_printf("Closing degenerate chord with our new successor.\n");
		close_connection(conn);
		remove_neighbor_connection(succ.id);
	}

	succ_conn = connect_to_node(&succ);
	if (succ_conn == NULL) {
		printf("Couldn't connect to the new successor. Left the ring.\n");
		leave_ring();
		return;
	}

	if (
		conn_printf(succ_conn->socket, "PRED "NODE_ID_OUT"\n", self.id) < 0 ||
		send_shortest_paths(succ_conn) < 0
	) {
		return;
	}

	v_printf("Successfully connected to the new successor.\n");
}

static void handle_broken_pred_socket(void) {
	if (connection_state != CONNECTED) {
		printf("The predecessor closed the connection before we finished joining the ring. Aborting the join procedure.\n");
		leave_ring();
		return;
	}

	remove_neighbor_connection(pred_conn->node_id);
	pred_conn = NULL;

	if (self.id == second_succ.id) {
		v_printf("The predecessor closed the connection. We are now alone in the ring.\n");
	} else {
		v_printf("The predecessor closed the connection. Awaiting the new predecessor's connection.\n");
		set_timeout(1000, pred_timeout);
	}
}

static void handle_broken_new_node_socket(void) {
	v_printf("The new client node closed the connection.\n");
	new_node_conn = NULL;
}

static void handle_broken_chord_socket(struct Connection *conn) {
	v_printf("The node with ID "NODE_ID_OUT" closed the chord connection.\n", conn->node_id);
}

// Called when a line is read from a TCP socket
void handle_message(int socket, char *message) {
	struct Connection *conn = find_connection_by_socket(socket);
	if (conn == new_node_conn) {
		handle_message_from_new_node(message);
	} else if (conn == pred_conn) {
		handle_message_from_pred(message);
	} else if (conn == succ_conn) {
		handle_message_from_succ(message);
	} else {
		if (conn->node_id == -1) {
			error("Assertion failed: (conn->node_id != -1) for a chord connection at handle_message()\n");
		}
		handle_message_from_chord(message, conn);
	}
}

// Called when another node closes a TCP socket, but not when this program closes a socket.
void handle_broken_socket(int socket) {
	struct Connection *conn = find_connection_by_socket(socket);
	if (conn == new_node_conn) {
		handle_broken_new_node_socket();
	} else if (conn == pred_conn) {
		handle_broken_pred_socket();
	} else if (conn == succ_conn) {
		handle_broken_succ_socket();
	} else {
		if (conn->node_id == -1) {
			error("Assertion failed: (conn->node_id != -1) for a chord connection at handle_broken_socket()\n");
		}
		handle_broken_chord_socket(conn);
	}

	NodeID node_id = conn->node_id;
	close_connection(conn);
	remove_neighbor_connection(node_id);
}
