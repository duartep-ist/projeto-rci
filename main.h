#ifndef MAIN_H
#define MAIN_H

// Allows the program to bind to the same port after the previous instance exits (skips TIME_WAIT)
#define CONFIG_SKIP_TIME_WAIT 1

#define MAX_UDP_SIZE 65536
#define IPV4_ADDR_STR_SIZE 16
#define TCP_PORT_STR_SIZE 6

#define USER_COMMAND_BUF_SIZE 256

#define MAX_NODE_MESSAGE_SIZE 256
#define MAX_NODES 16
#define MAX_PATH_STR_LENGTH 47 // equal to (MAX_NODES*2 + (MAX_NODES-1)*1)
#define MAX_PATH_STR_SIZE (MAX_PATH_STR_LENGTH + 1)

// The max value representable by this type must be bigger than `MAX_NODES`
typedef signed char NodeIndex;

#define MAX_NODE_ID 99
// The max value representable by this type must be bigger than `MAX_NODE_ID`
typedef signed char NodeID;
#define NODE_ID_IN "%hhd"
#define NODE_ID_OUT "%02hhd"


typedef struct Node {
	NodeID id;
	char ip_addr[IPV4_ADDR_STR_SIZE];
	char tcp_port[TCP_PORT_STR_SIZE];
} Node;


#include <stdbool.h>
#include <sys/select.h>

#include "util.h"
#include "connections.h"
#include "routing.h"
#include "ring.h"
#include "node-server.h"
#include "read-lines.h"

enum InputState {
	COMMAND,
	JOIN_NODE_SELECTION,
	CHORD_NODE_SELECTION
};
extern enum InputState input_state;

// The set of file descriptors for which select() should return when they have new data
extern fd_set select_inputs;

// The passive socket used for accepting incoming connections
extern int public_socket;

#include "connections.h"

void copy_node(Node *dest, Node *src);
void set_timeout(long int ms, void (*handler)(void));
void cancel_timeout(void);

#endif
