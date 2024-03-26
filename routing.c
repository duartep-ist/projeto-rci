#include <string.h>

#include "routing.h"

// The ID arrays indicate which nodes the rows and columns of the routing table correspond to. They
// contain `NO_NODE_ID` if the index is not allocated and the node ID if it is. The index at which an ID is
// is used for indexing the routing and forwarding tables. For example,
// `forwarding_table[recipient_ids[10]]` contains the index of the neighbor to which menssages sent
// to the node with ID 10 should be forwarded.
//
// Neighbor indices are allocated when the first ROUTE message is received and deallocated when a
// connection is established or destroyed, respectively.
//
// Recipient indices are allocated when the node ID is first given to the
// `update_routing_given_new_path()` function and are deallocated whenever the
// corresponding row of the routing table contains only invalid paths.
NodeID recipient_ids[MAX_RECIPIENTS];
NodeIndex neighbor_ids[MAX_NEIGHBORS];

RoutingTable routing_table;
ForwardingTable forwarding_table;

// Gets the recipient index for a specific node. A new index is allocated if needed.
NodeIndex get_recipient_index(NodeID recipient_id, bool add_if_missing) {
	for (int i = 0; i < MAX_RECIPIENTS; i++) {
		if (recipient_ids[i] == recipient_id) {
			return i;
		}
	}
	if (!add_if_missing) {
		return -1;
	}

	// This node wasn't in the list. Adding it to the list.
	for (int i = 0; i < MAX_RECIPIENTS; i++) {
		if (recipient_ids[i] == -1) {
			// Initializing the data structures.
			recipient_ids[i] = recipient_id;
			for (int j = 0; j < MAX_NEIGHBORS; j++) {
				routing_table[i][j].hop_count = INVALID_PATH;
			}
			forwarding_table[i] = -1;
			return i;
		}
	}
	error("Ran out of space for recipients in the routing tables!\n");
}
NodeIndex get_neighbor_index(NodeID neighbor_id, bool add_if_missing) {
	for (int i = 0; i < MAX_NEIGHBORS; i++) {
		if (neighbor_ids[i] == neighbor_id) {
			return i;
		}
	}
	if (!add_if_missing) {
		return -1;
	}

	// This node wasn't in the list. Adding it to the list.
	for (int i = 0; i < MAX_NEIGHBORS; i++) {
		if (neighbor_ids[i] == -1) {
			// Initializing the data structures.
			neighbor_ids[i] = neighbor_id;
			for (int j = 0; j < MAX_RECIPIENTS; j++) {
				routing_table[j][i].hop_count = INVALID_PATH;
			}
			return i;
		}
	}
	error("Ran out of space for nodes in the routing tables!");
}


static bool are_paths_equal(const Path *p1, const Path *p2) {
	return (
		p1->hop_count == p2->hop_count && (
			p1->hop_count <= 0 ||
			memcmp(p1->nodes, p2->nodes, (p1->hop_count) * sizeof(NodeID)) == 0
		)
	);
}
static void copy_path(Path *dest, const Path *src) {
	dest->hop_count = src->hop_count;
	if (src->hop_count > 0) {
		memcpy(dest->nodes, src->nodes, (src->hop_count) * sizeof(NodeID));
	}
}


void remove_routing_neighbor(NodeID neighbor_id) {
	NodeIndex neighbor = get_neighbor_index(neighbor_id, false);
	if (neighbor == -1) {
		return;
	}

	for (int recipient = 0; recipient < MAX_RECIPIENTS; recipient++) {
		NodeID recipient_id = recipient_ids[recipient];
		if (recipient_id != -1) {
			update_routing_and_announce_given_new_path(neighbor_id, recipient_id, NULL);
		}
	}
	neighbor_ids[neighbor] = -1;
}

// Updates the routing tables given the shortest path between a neighbor and a recipient.
// If `path == NULL || path->hop_count == INVALID_PATH`, the path is considered invalid.
// Returns `true` if the shortest path from this node to the recipient node was changed, `false` otherwise.
bool update_routing_given_new_path(NodeID neighbor_id, NodeID recipient_id, const Path *path_in) {
	if (recipient_id == self.id) return false;
	if (neighbor_id == self.id) {
		dbg_warn("update_routing_given_new_path(): neighbor_id == self.id");
		return false;
	}

	NodeIndex neighbor = get_neighbor_index(neighbor_id, true);
	NodeIndex recipient = get_recipient_index(recipient_id, true);

	Path path;
	if (path_in == NULL || path_in->hop_count == INVALID_PATH) {
		path.hop_count = INVALID_PATH;
	} else {
		for (NodeIndex i = 0; i < path_in->hop_count; i++) {
			if (path_in->nodes[i] == self.id) {
				// The shortest path crosses this node, so it's considered invalid.
				path.hop_count = INVALID_PATH;
				goto invalid_path;
			}
		}

		copy_path(&path, path_in);
		invalid_path:;
	}

	// Here, `path` is either invalid (path.hop_count == INVALID_PATH) or contains the full path, including the neighbor.

	// Store the current shortest path so we know whether it changed
	NodeIndex old_closest_neighbor = forwarding_table[recipient];
	Path old_shortest_path;
	if (old_closest_neighbor != -1) {
		copy_path(&old_shortest_path, &routing_table[recipient][old_closest_neighbor]);
	} else {
		old_shortest_path.hop_count = INVALID_PATH;
	}

	// Update the entry
	Path *entry = &routing_table[recipient][neighbor];
	copy_path(entry, &path);

	// Find the new shortest path
	NodeIndex closest_neighbor = -1;
	for (NodeIndex ni = 0; ni < MAX_NEIGHBORS; ni++) {
		if (
			neighbor_ids[ni] != -1 && // the neighbor exists
			routing_table[recipient][ni].hop_count != INVALID_PATH && // there is a valid path to the recipient via the neighbor
			(
				closest_neighbor == -1 ||
				// the path is shorter than the shortest one found so far
				routing_table[recipient][ni].hop_count < routing_table[recipient][closest_neighbor].hop_count
			)
		) {
			closest_neighbor = ni;
		}
	}

	// Avoid changing the forwarding table if there are multiple paths with the smallest hop count
	if (
		old_closest_neighbor != -1 &&
		closest_neighbor != -1 &&
		routing_table[recipient][old_closest_neighbor].hop_count == routing_table[recipient][closest_neighbor].hop_count
	) {
		closest_neighbor = old_closest_neighbor;
	}

	// Free the recipient index if the row is empty
	if (closest_neighbor == -1) {
		vv_printf("There are no valid paths to the recipient "NODE_ID_OUT". Removing the row from the routing table.\n", recipient_id);
		recipient_ids[recipient] = -1;
	}

	forwarding_table[recipient] = closest_neighbor;

	return (
		old_closest_neighbor != closest_neighbor || (
			closest_neighbor != -1 &&
			!are_paths_equal(&old_shortest_path, &routing_table[recipient][closest_neighbor])
		)
	);
}


// Writes the string representation of `path` into `str`.
int path_to_string(char *str, NodeID recipient_id, Path *path) {
	if (path->hop_count == INVALID_PATH) {
		str[0] = '\0';
		return 0;
	} else if (path->hop_count == -1) {
		error("Assertion (path->hop_count != -1) failed!");
	} else {
		char *s = str;
		s += sprintf(s, ""NODE_ID_OUT"-", self.id);
		for (NodeIndex i = 0; i < path->hop_count; i++) {
			s += sprintf(s, ""NODE_ID_OUT"-", path->nodes[i]);
		}
		s += sprintf(s, ""NODE_ID_OUT"", recipient_id);
		return s - str;
	}
}

static void get_route_message(char *msg, NodeID recipient_id, Path *path) {
	if (path == NULL || path->hop_count == INVALID_PATH) {
		sprintf(msg, "ROUTE "NODE_ID_OUT" "NODE_ID_OUT"\n", self.id, recipient_id);
	} else {
		char *s = msg;
		s += sprintf(s, "ROUTE "NODE_ID_OUT" "NODE_ID_OUT" ", self.id, recipient_id);
		s += path_to_string(s, recipient_id, path);
		s += sprintf(s, "\n");
	}
}

static void announce_new_path(NodeID recipient_id) {
	NodeIndex recipient = get_recipient_index(recipient_id, false);
	Path *path;
	if (recipient == -1) {
		path = NULL;
	} else {
		NodeIndex neighbor = forwarding_table[recipient];
		path = &routing_table[recipient][neighbor];
	}

	char route_msg[14+MAX_PATH_STR_LENGTH];
	get_route_message(route_msg, recipient_id, path);
	v_printf("Announcing new shortest path: %s", route_msg);
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i].socket != -1) {
			conn_printf(connections[i].socket, "%s", route_msg);
		}
	}
}

int send_shortest_paths(struct Connection *conn) {
	v_printf("Sending our shortest path table to node "NODE_ID_OUT".\n", conn->node_id);
	if (conn_printf(conn->socket, "ROUTE "NODE_ID_OUT" "NODE_ID_OUT" "NODE_ID_OUT"\n", self.id, self.id, self.id) < 0) {
		return -1;
	}
	for (NodeIndex recipient = 0; recipient < MAX_RECIPIENTS; recipient++) {
		NodeID recipient_id = recipient_ids[recipient];
		if (recipient_id != -1) {
			NodeIndex neighbor = forwarding_table[recipient];
			char route_msg[14+MAX_PATH_STR_LENGTH];
			get_route_message(route_msg, recipient_id, neighbor == -1 ? NULL : &routing_table[recipient][neighbor]);
			if (conn_printf(conn->socket, "%s", route_msg) < 0) {
				return -1;
			}
		}
	}
	return 0;
}

void update_routing_and_announce_given_new_path(NodeID neighbor_id, NodeID recipient_id, const Path *path) {
	if (update_routing_given_new_path(neighbor_id, recipient_id, path)) {
		announce_new_path(recipient_id);
	}
}

bool forward_message(NodeIndex sender_id, NodeIndex recipient_id, const char *chat_message) {
	NodeIndex recipient_index = get_recipient_index(recipient_id, false);
	if (recipient_index == -1) {
		v_printf("There are no valid paths to the node "NODE_ID_OUT". Dropping the message.\n", recipient_id);
		return false;
	} else {
		NodeID neighbor_id = neighbor_ids[forwarding_table[recipient_index]];
		v_printf("Forwarding message "NODE_ID_OUT"->"NODE_ID_OUT" \"%s\" via neighbor "NODE_ID_OUT".\n", sender_id, recipient_id, chat_message, neighbor_id);
		struct Connection *neighbor_conn = find_connection_by_node_id(neighbor_id);
		if (neighbor_conn == NULL) {
			warn("Couldn't forward message to node "NODE_ID_OUT" via neighbor "NODE_ID_OUT" because the connection with the neighbor was closed.\n", recipient_id, neighbor_id);
		}
		if (conn_printf(neighbor_conn->socket, "CHAT "NODE_ID_OUT" "NODE_ID_OUT" %s\n", sender_id, recipient_id, chat_message) < 0) {
			return false;
		}
		return true;
	}
}

void init_routing(void) {
	for (int i = 0; i < MAX_RECIPIENTS; i++) {
		recipient_ids[i] = -1;
	}
	for (int i = 0; i < MAX_NEIGHBORS; i++) {
		neighbor_ids[i] = -1;
	}
}
