#include <string.h>
#include <unistd.h>

#include "read-lines.h"

enum RLResult read_lines(int fd, char *buffer, int *buffer_index, int buffer_size, void (*handler)(int fd, char *line)) {
	int len = read(fd, buffer + *buffer_index, buffer_size - *buffer_index);
	if (len == -1) {
		return RL_ERROR;
	} else if (len == 0) {
		*buffer_index = 0;
		return RL_END;
	}

	// The recieved bytes may contain several messages
	int message_start_index = 0;
	for (int i = 0; i < len; i++) {
		if (buffer[i] == '\n') {
			buffer[i] = '\0';
			handler(fd, buffer + message_start_index);
			message_start_index = i + 1;
		}
	}

	int remainder_len = *buffer_index + len - message_start_index;
	if (remainder_len == buffer_size) {
		*buffer_index = 0;
		return RL_OVERFLOW;
	} else {
		// Move the incomplete message to the beginning of the buffer
		memmove(buffer, buffer + message_start_index, remainder_len);
		*buffer_index = remainder_len;
		return RL_OK;
	}
}
