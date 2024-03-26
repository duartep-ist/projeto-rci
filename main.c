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
#include <stdarg.h>

#include "main.h"

enum InputState input_state = COMMAND;

fd_set select_inputs;
int public_socket = -1;

static bool should_exit = false;
static char stdin_buffer[USER_COMMAND_BUF_SIZE];
static int stdin_buffer_index;

void copy_node(Node *dest, Node *src) {
	dest->id = src->id;
	strcpy(dest->ip_addr, src->ip_addr);
	strcpy(dest->tcp_port, src->tcp_port);
}


// Handling user commands
// sizeof(cmd_name) returns the size of the cmd_name string plus one (for the null character)
#define COMPARE_COMMAND(cmd_name) (strncmp(input_lowercase, cmd_name, sizeof(cmd_name) - 1) == 0 && (input_lowercase[sizeof(cmd_name) - 1] == '\0' || isspace(input_lowercase[sizeof(cmd_name) - 1])))
#define sscanf_alt(cmd_name, short_cmd_name, args, arg_count, ...) (sscanf(input, cmd_name " " args, __VA_ARGS__) == arg_count || sscanf(input, short_cmd_name " " args, __VA_ARGS__) == arg_count)
static void handle_user_input(int fd, char *input) {
	(void) fd; // Unused but part of the read_lines API

	if (input_state == JOIN_NODE_SELECTION || input_state == CHORD_NODE_SELECTION) {
		NodeID id = -1;
		if (sscanf(input, NODE_ID_IN, &id) != 1 && input_state == JOIN_NODE_SELECTION) {
			// Cancel the operation if the input is invalid
			goto invalid_id;
		}

		if (input_state == CHORD_NODE_SELECTION && (self.id == id || find_connection_by_node_id(id) != NULL)) {
			goto invalid_id;
		}
		for (int i = 0; i < node_arr.length; i++) {
			Node node = node_arr.nodes[i];
			if (node.id == id) {
				if (input_state == JOIN_NODE_SELECTION) {
					copy_node(&succ, &node);
					v_printf("Joining ring %s with my ID as "NODE_ID_OUT" using the successor with ID "NODE_ID_OUT" at %s:%s.\n", ring_id_str, self.id, succ.id, succ.ip_addr, succ.tcp_port);
					join_ring();
				} else if (input_state == CHORD_NODE_SELECTION) {
					create_outbound_chord(&node);
				}
				input_state = COMMAND;
				return;
			}
		}
		// Execution exits the loop if the node ID wasn't in the table

		invalid_id:
		printf("Invalid ID. Operation cancelled.\n");
		if (input_state == JOIN_NODE_SELECTION) {
			connection_state = DISCONNECTED;
		}
		fflush(stdout);
		input_state = COMMAND;
		return;
	}

	char input_lowercase[USER_COMMAND_BUF_SIZE];
	for (int i = 0;; i++) {
		input_lowercase[i] = tolower(input[i]);
		if (input[i] == '\0')
			break;
	}

	if (COMPARE_COMMAND("join") || COMPARE_COMMAND("j")) {
		if (sscanf(input, "%*s %3s " NODE_ID_IN, ring_id_str, &self.id) != 2) {
			printf("Missing parameters for join command.\n");
			return;
		}
		if (strlen(ring_id_str) != 3) {
			printf("Wrong length for ring ID.\n");
			return;
		}

		if (connection_state != DISCONNECTED) {
			printf("We are already connected to a ring or connecting to one. Use the leave command first.\n");
		} else {
			connection_state = AWAITING_NODE_LIST;
			node_list_action = JOIN_ACTION;
			request_node_list(ring_id_str);
		}
	} else if (COMPARE_COMMAND("leave") || COMPARE_COMMAND("l")) {
		if (connection_state == DISCONNECTED) {
			printf("We are not connected to a ring.\n");
		} else {
			leave_ring();
			printf("Left the ring.\n");
		}

	} else if (COMPARE_COMMAND("direct join") || COMPARE_COMMAND("dj")) {
		if (sscanf(input, COMPARE_COMMAND("dj") ? "%*s "NODE_ID_IN" "NODE_ID_IN" %15s %5s" : "%*s %*s "NODE_ID_IN" "NODE_ID_IN" %15s %5s", &self.id, &succ.id, succ.ip_addr, succ.tcp_port) != 4) {
			printf("Missing parameters for direct join command.\n");
			return;
		}

		if (succ.id == self.id) {
			v_printf("Initializing an empty ring without registering with the node server. My ID is "NODE_ID_OUT".\n", self.id);
			ring_id_str[0] = '\0';
			copy_node(&succ, &self);
			copy_node(&second_succ, &self);
			connection_state = CONNECTED;
			awaiting_pred = false;
			awaiting_succ = false;
		} else {
			v_printf("Joining ring with my ID as "NODE_ID_OUT" the successor with ID "NODE_ID_OUT" at %s:%s without registering with the node server.\n", self.id, succ.id, succ.ip_addr, succ.tcp_port);
			ring_id_str[0] = '\0';
			join_ring();
		}

	} else if (COMPARE_COMMAND("chord") || COMPARE_COMMAND("c")) {
		if (connection_state != CONNECTED) {
			printf("We are not connected to a ring.\n");
		} else if (outbound_chord_conn != NULL) {
			printf("There is already an outbound chord.\n");
		} else {
			node_list_action = CHORD_ACTION;
			request_node_list(ring_id_str);
		}

	} else if (COMPARE_COMMAND("remove chord") || COMPARE_COMMAND("rc")) {
		if (connection_state != CONNECTED) {
			printf("We are not connected to a ring.\n");
		} else if (outbound_chord_conn == NULL) {
			printf("There is currently no outbound chord.\n");
		} else {
			NodeID id = outbound_chord_conn->node_id;
			close_connection(outbound_chord_conn);
			if (!find_connection_by_node_id(id)) {
				remove_routing_neighbor(id);
			}
			printf("Outbound chord with node "NODE_ID_OUT" removed.\n", id);
		}

	} else if (COMPARE_COMMAND("exit") || COMPARE_COMMAND("x")) {
		should_exit = true;

	} else if (COMPARE_COMMAND("show topology") || COMPARE_COMMAND("st")) {
		if (connection_state == CONNECTED) {
			printf("\
+----------------+----+-----------------+-------+\n\
| Node           | ID | IP address      | Port  |\n\
+----------------+----+-----------------+-------+\n\
");
			if (pred_conn != NULL && pred_conn->node_id != -1)
				printf("| Predecessor    | "NODE_ID_OUT" | %-15s |   -   |\n", pred_conn->node_id, pred_conn->ip_addr);
			printf("| This node      | "NODE_ID_OUT" | %-15s | %-5s |\n", self.id, self.ip_addr, self.tcp_port);
			printf("| Successor      | "NODE_ID_OUT" | %-15s | %-5s |\n", succ.id, succ.ip_addr, succ.tcp_port);
			printf("| Second succ.   | "NODE_ID_OUT" | %-15s | %-5s |\n", second_succ.id, second_succ.ip_addr, second_succ.tcp_port);
			if (outbound_chord_conn != NULL) {
				printf("| Outbound chord | "NODE_ID_OUT" | %-15s | %-5s |\n", outbound_chord_conn->node_id, outbound_chord_conn->ip_addr, outbound_chord_conn->tcp_port);
			}
			for (int i = 0; i < MAX_CONNECTIONS; i++) {
				struct Connection *conn = &connections[i];
				if (conn->socket != -1 && is_inbound_chord(conn)) {
					printf("| Inbound chord  | "NODE_ID_OUT" | %-15s |   -   |\n", conn->node_id, conn->ip_addr);
				}
			}
			printf("+----------------+----+-----------------+-------+\n");
		} else {
			printf("The node isn't connected.\n");
		}

	} else if (COMPARE_COMMAND("show routing") || COMPARE_COMMAND("sr")) {
		NodeID recipient_id;
		if (sscanf(input, COMPARE_COMMAND("sr") ? "%*s "NODE_ID_IN"" : "%*s %*s "NODE_ID_IN"", &recipient_id) != 1) {
			printf("Missing parameters for the show routing command.\n");
			return;
		}

		if (recipient_id == self.id) {
			printf("There's no need for routing when you're sending messages to yourself.\n");
			return;
		}

		NodeIndex recipient = get_recipient_index(recipient_id, false);
		if (recipient == -1) {
			printf("There are no known valid paths to the node "NODE_ID_OUT". It might not be in the ring.\n", recipient_id);
			return;
		}

		printf("Possible paths from the node "NODE_ID_OUT" to the node "NODE_ID_OUT":\n", self.id, recipient_id);

		for (NodeIndex neighbor = 0; neighbor < MAX_NEIGHBORS; neighbor++) {
			NodeID neighbor_id = neighbor_ids[neighbor];
			if (neighbor_id != -1) {
				printf("    Via "NODE_ID_OUT": ", neighbor_id);
				Path *path = &routing_table[recipient][neighbor];
				if (path->hop_count == INVALID_PATH) {
					printf("(no valid path)\n");
				} else {
					char path_str[MAX_PATH_STR_SIZE];
					path_to_string(path_str, recipient_id, path);
					printf("%s\n", path_str);
				}
			}
		}

	} else if (COMPARE_COMMAND("show path") ||  COMPARE_COMMAND("sp")) {
		NodeID recipient_id;
		if (sscanf(input, COMPARE_COMMAND("sp") ? "%*s "NODE_ID_IN"" : "%*s %*s "NODE_ID_IN"", &recipient_id) != 1) {
			printf("Missing parameters for the show routing command.\n");
			return;
		}

		NodeIndex recipient = get_recipient_index(recipient_id, false);
		if (recipient == -1) {
			printf("There are no known valid paths to the node "NODE_ID_OUT". It might not be in the ring.\n", recipient_id);
			return;
		}

		NodeIndex neighbor = forwarding_table[recipient];
		Path *path = &routing_table[recipient][neighbor];
		char path_str[MAX_PATH_STR_SIZE];
		path_to_string(path_str, recipient_id, path);
		printf("Shortest path to "NODE_ID_OUT": %s\n", recipient_id, path_str);

	} else if (COMPARE_COMMAND("message") ||  COMPARE_COMMAND("m")) {
		NodeID recipient_id;
		int chat_message_start = -1;
		if (
			sscanf(input, "%*s "NODE_ID_IN"%n", &recipient_id, &chat_message_start) != 1 ||
			chat_message_start == -1 ||
			input[chat_message_start] != ' '
		) {
			printf("Missing parameters for the message command.\n");
			return;
		}

		// Make sure we get the entire message even if it starts with a whitespace character
		char *chat_message = input + chat_message_start + 1;

		if (recipient_id == self.id) {
			printf("Node "NODE_ID_OUT" said: \"%s\"\n", self.id, chat_message);
			return;
		}

		if (forward_message(self.id, recipient_id, chat_message)) {
			printf("Message sent.\n");
		} else {
			printf("Couldn't send a message to the node "NODE_ID_IN" because there are no known valid paths to that node. Check if you entered the correct ID.\n", recipient_id);
		}

	} else {
		printf("Unrecognized command: %s\n", input);
	}
	fflush(stdout);
}


static struct timespec timeout_instant;
static void (*timeout_handler)(void) = NULL;

void set_timeout(long int ms, void (*handler)(void)) {
	if (timeout_handler != NULL) {
		dbg_warn("set_timeout(): There's already a timer running.");
	}

	// Get the current time
	if (clock_gettime(CLOCK_MONOTONIC, &timeout_instant) < 0) {
		warn("Couldn't get current time: %s", strerror(errno));
		return;
	}

	// Add the timeout
	timeout_instant.tv_sec += ms / (long int) 1e3;
	timeout_instant.tv_nsec += ms * (long int) 1e6;
	if (timeout_instant.tv_nsec > 1e9) {
		timeout_instant.tv_sec++;
		timeout_instant.tv_nsec -= 1e9;
	}

	timeout_handler = handler;
}
void cancel_timeout(void) {
	if (timeout_handler == NULL) {
		return;
	}
	timeout_handler = NULL;
}


// Função principal
int main(int argc, char **argv) {
	// Prevent the process from terminating immediately when it tries to write to a broken socket
	if (sigaction(SIGPIPE, &(struct sigaction) { .sa_handler = SIG_IGN }, NULL) < 0)
		error("sigaction(): %s\n", strerror(errno));

	// Argument parsing

	char *initial_command = NULL;

	while (true) {
		int opt = getopt(argc, argv, "x:v:");
		if (opt == -1) break;
		switch (opt) {
			case 'x':
				initial_command = optarg;
				break;

			case 'v':
				verbose_level = atoi(optarg);
				if (verbose_level < 0) verbose_level = 0;
				break;

			default:
				fprintf(stderr, "Usage: COR [-x <command>] [-v <verbosity level>] <own IP> <own TCP port> [<node server IP> <node server UDP port>]\n");
				exit(1);
				break;
		}
	}

	// Verificar se o número de argumentos é válido
	if (argc < optind+2) {
		fprintf(stderr, "Usage: COR [-x <command>] [-v <verbosity level>] <own IP> <own TCP port> [<node server IP> <node server UDP port>]\n");
		exit(1);
	}

	strcpy(self.ip_addr, argv[optind+0]);
	strcpy(self.tcp_port, argv[optind+1]);
	char *ns_addr_str = (argc >= optind+4) ? argv[optind+2] : "193.136.138.142";
	char *ns_port_str = (argc >= optind+4) ? argv[optind+3] : "59000";


	init_connections_array();

	// Connection to node server
	int ns_socket = init_ns(ns_addr_str, ns_port_str);

	// TCP Server
	{
		public_socket = socket(AF_INET, SOCK_STREAM, 0); // TCP over IPv4
		if (public_socket == -1)
			error("Couldn't create TCP socket: %s\n", strerror(errno));

		#if CONFIG_SKIP_TIME_WAIT
		int val = 1;
		setsockopt(public_socket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int));
		#endif

		struct addrinfo hints = {0};
		hints.ai_family = AF_INET;       // IPv4
		hints.ai_socktype = SOCK_STREAM; // TCP socket
		hints.ai_flags = AI_PASSIVE;

		struct addrinfo *ai;
		int errcode = getaddrinfo(NULL, self.tcp_port, &hints, &ai);
		if (errcode != 0)
			error("Couldn't get the node server address: %s\n", gai_strerror(errcode));
		ssize_t n = bind(public_socket, ai->ai_addr, ai->ai_addrlen);
		freeaddrinfo(ai);
		if (n == -1)
			error("Couldn't bind TCP server to port %s: %s\n", self.tcp_port, strerror(errno));
		if (listen(public_socket, 10) == -1)
			error("Couldn't listen for connections to the TCP server: %s\n", strerror(errno));

		printf("TCP server listening on port %s.\n", self.tcp_port);
	}

	int stdin_fd = fileno(stdin);

	FD_ZERO(&select_inputs);
	FD_SET(stdin_fd, &select_inputs);
	FD_SET(ns_socket, &select_inputs);
	FD_SET(public_socket, &select_inputs);
	/*FD_SET(to_read_pipe, &select_inputs);*/

	if (initial_command != NULL) {
		printf("%s\n", initial_command);
		handle_user_input(stdin_fd, initial_command);
	}

	// Main select loop
	while (!should_exit) {
		struct timespec now;
		struct timeval select_timeout;
		struct timeval *select_timeout_ptr;

		// Calculate time until timeout
		if (timeout_handler != NULL) {
			// Get the current time
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
				warn("Couldn't get current time: %s", strerror(errno));
				select_timeout_ptr = NULL;
			} else {
				if (now.tv_sec > timeout_instant.tv_sec || (now.tv_sec == timeout_instant.tv_sec && now.tv_nsec > timeout_instant.tv_nsec)) {
					// If the timeout has passed, make select return immediately
					select_timeout.tv_sec = 0;
					select_timeout.tv_usec = 0;
				} else {
					// Calculate the time left
					select_timeout.tv_sec = timeout_instant.tv_sec - now.tv_sec;
					select_timeout.tv_usec = ((timeout_instant.tv_nsec - now.tv_nsec) / (long) 1e3) + 1;
					if (select_timeout.tv_usec < 0) {
						select_timeout.tv_sec--;
						select_timeout.tv_usec += 1e6;
					}
				}
				select_timeout_ptr = &select_timeout;
			}
		} else {
			select_timeout_ptr = NULL;
		}

		fd_set readable = select_inputs; // Reload mask
		int readable_count = select(FD_SETSIZE, &readable, NULL, NULL, select_timeout_ptr);

		if (timeout_handler != NULL) {
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
				warn("Couldn't get current time: %s", strerror(errno));
			} else if (now.tv_sec > timeout_instant.tv_sec || (now.tv_sec == timeout_instant.tv_sec && now.tv_nsec > timeout_instant.tv_nsec)) {
				// If the timeout has passed, invoke the handler and unset the timeout
				timeout_handler();
				timeout_handler = NULL;
			}
		}

		if (readable_count == -1) {
			error("select() error: %s\n", strerror(errno));
		} else {
			if (FD_ISSET(stdin_fd, &readable)) {
				// Received data from stdin
				enum RLResult result = read_lines(0, stdin_buffer, &stdin_buffer_index, USER_COMMAND_BUF_SIZE, handle_user_input);
				if (result == RL_END) {
					v_printf("Reached end of stdin. Exiting.\n");
					break;
				} else if (result == RL_ERROR) {
					error("Couldn't read from stdin: %s\n", strerror(errno));
				} else if (result == RL_OVERFLOW) {
					warn("User command too big.\n");
				}
			}
			if (FD_ISSET(ns_socket, &readable)) {
				// Received a message from the node server
				char ns_response_buffer[MAX_UDP_SIZE];
				ssize_t len = recvfrom(ns_socket, ns_response_buffer, MAX_UDP_SIZE, 0, NULL, 0);
				if (len == -1)
					error("Couldn't receive message from node server: %s\n", strerror(errno));

				if (strncmp(ns_response_buffer, "OKREG", 5) == 0) {
					if (connection_state != CONNECTED) {
						warn("Got unexpected OKREG response. Ignoring.\n");
					} else {
						v_printf("Node server confirmed our registration.\n");
					}
				} else if (strncmp(ns_response_buffer, "OKUNREG", 7) == 0) {
					if (connection_state != DISCONNECTED) {
						warn("Got unexpected OKUNREG response. Ignoring.\n");
					} else {
						v_printf("Node server confirmed our unregistration.\n");
					}
				} else {
					v_printf("Unrecognized node server message: %s\n", ns_response_buffer);
				}
			}
			if (FD_ISSET(public_socket, &readable)) {
				// Received a request for a TCP connection

				struct sockaddr_in addr;
				socklen_t addrlen = sizeof(addr);

				int socket = accept(public_socket, (struct sockaddr *)&addr, &addrlen);
				char *src_ip_addr = inet_ntoa(addr.sin_addr);
				if (socket == -1) {
					warn("Couldn't accept a TCP connection from %s: %s\n", src_ip_addr, strerror(errno));
					continue;
				}

				if (connection_state != CONNECTING && connection_state != CONNECTED) {
					close(socket);
					warn("Unexpectedly received a TCP connection from %s.\n", src_ip_addr);
					continue;
				}

				if (new_node_conn != NULL) {
					close(socket);
					warn("Couldn't accept a TCP connection from %s because we are already handling a node connection.\n", src_ip_addr);
					continue;
				}

				struct Connection *conn = add_connection(socket);
				strcpy(conn->ip_addr, src_ip_addr);
				v_printf("Accepted TCP connection from %s.\n", src_ip_addr);
				new_node_conn = conn;
			}
			for (int i = 0; i < MAX_CONNECTIONS; i++) {
				int socket = connections[i].socket;
				if (socket != -1 && FD_ISSET(socket, &readable)) {
					enum RLResult result = read_lines(socket, connections[i].buffer, &connections[i].buffer_index, MAX_NODE_MESSAGE_SIZE, handle_message);
					if (result == RL_END) {
						handle_broken_socket(socket);
					} else if (result == RL_ERROR) {
						handle_broken_socket(socket);
					} else if (result == RL_OVERFLOW) {
						warn("A node is sending too big of a message. Discarding some bytes.\n");
					}
				}
			}
		}
	}

	return 0;
}
