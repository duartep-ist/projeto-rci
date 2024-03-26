#ifndef ROUTING_H
#define ROUTING_H

#include <stdbool.h>

#include "main.h"

#define MAX_RECIPIENTS (MAX_NODES-1)
#define MAX_NEIGHBORS (MAX_NODES-1)

#define INVALID_PATH ((NodeIndex) -2)
#define NO_NODE_INDEX ((NodeIndex) -1)
#define NO_NODE_ID ((NodeID) -1)

typedef struct Path {
	// Equal to `INVALID_PATH` if the path doesn't exist. May be `-1` if the sender and the recipient are the same node.
	NodeIndex hop_count;
	// The IDs of the nodes in the path excluding the sender (ourselves) and the recipient
	// Data is stored in `nodes[0]` through `nodes[hop_count-2]`, inclusive.
	NodeID nodes[MAX_NODES];
} Path;

// Indexed by [recipient_index][neighbor_index]
typedef Path RoutingTable[MAX_RECIPIENTS][MAX_NEIGHBORS];
typedef NodeIndex ForwardingTable[MAX_RECIPIENTS];

#define shortest_path_to(recipient_index) (routing_table[recipient_index][forwarding_table[recipient_index]])

extern NodeID recipient_ids[MAX_RECIPIENTS];
extern NodeIndex neighbor_ids[MAX_NEIGHBORS];

extern RoutingTable routing_table;
extern ForwardingTable forwarding_table;


void init_routing(void);
NodeIndex get_recipient_index(NodeID recipient_id, bool add_if_missing);
NodeIndex get_neighbor_index(NodeID neighbor_id, bool add_if_missing);
void remove_routing_neighbor(NodeID neighbor_id);
int path_to_string(char *str, NodeID recipient_id, Path *path);
bool update_routing_given_new_path(NodeID neighbor_id, NodeID recipient_id, const Path *path_in);
void update_routing_and_announce_given_new_path(NodeID neighbor_id, NodeID recipient_id, const Path *path);
bool forward_message(NodeIndex sender_id, NodeIndex recipient_id, const char *chat_message);

// Sends the shortest path table to a connection. Returns -1 if there was an error sending the messages.
int send_shortest_paths(Connection *conn);

#endif
