// Various general utility functions

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "util.h"

void *malloc_f(size_t size) {
	void *p = malloc(size);
	if (p == NULL) {
		error("Allocation failed.");
	}
	return p;
}

int verbose_level;
