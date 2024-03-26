#ifndef NODE_SERVER_H
#define NODE_SERVER_H

extern int ns_socket;

typedef struct NodeArray {
	int length;
	Node nodes[MAX_NODES];
} NodeArray;

// The node list returned by the node server
extern NodeArray node_arr;

// Determines what to do when we receive a node list
enum NodeListAction {
	UNEXPECTED_NODE_LIST,
	JOIN_ACTION,
	CHORD_ACTION
};
extern enum NodeListAction node_list_action;

int init_ns(char *ns_addr_str, char *ns_port_str);
void send_ns_message(const char *message, int length);
void request_node_list(char *ring_id_str);

#endif
