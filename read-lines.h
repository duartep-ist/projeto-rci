#ifndef READ_LINES_H
#define READ_LINES_H

enum RLResult {
	// All good
	RL_OK,
	// End of file (end of connection for TCP sockets)
	RL_END,
	// read() error
	RL_ERROR,
	// The line is bigger than the buffer
	RL_OVERFLOW,
};

// Reads lines from a file descriptor
// fd: file descriptor
// buffer: opaque buffer
// buffer_index: pointer to opaque index
// buffer_size: size of buffer
// handler: function called for every line received. The line string will be null-terminated.
enum RLResult read_lines(int fd, char *buffer, int *buffer_index, int buffer_size, void (*handler)(int fd, char *line));

#endif
