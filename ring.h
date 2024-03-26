#ifndef RING_H
#define RING_H

#include "connections.h"

enum ConnectionState {
	// Not in a ring and not trying to connect
	DISCONNECTED,
	// Asked the node server for the node list, awaiting response
	AWAITING_NODE_LIST,
	// Got the node list, waiting for the user to select the preferred successor node
	AWAITING_USER_SELECTION,
	// Tried connecting to the successor node via TCP, awaiting SUCC message from the successor and the connection from the predecessor
	CONNECTING,
	// In a ring
	CONNECTED
};
extern enum ConnectionState connection_state;


// The successor's ID is also stored in `succ.id`
extern Node self, succ, second_succ;

// CONNECTING only
// Whether we are waiting for the successor to tell us who our second successor is via a SUCC message
extern bool awaiting_succ;
// Whether we are waiting for the predecessor to tell us what its ID is via a PRED message
extern bool awaiting_pred;

// This is an empty string if we connected to another node or another node connected to us using the direct join command.
extern char ring_id_str[4];


struct Connection *connect_to_node(struct Node *node);
void leave_ring(void);
void join_ring(void);
void create_outbound_chord(struct Node *node);
void handle_message(int socket, char *message);
void handle_broken_socket(int socket);
void on_join_end(void);

#endif