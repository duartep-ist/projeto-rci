# Tell make not to treat the name of these targets as filenames
.PHONY: clean

# The program is built without debug features unless the user sets DEBUG to 1 via the environmet variable
DEBUG ?= 0

CC = gcc
# -Wall: Enables warnings
# -std=c99: Enables the correct compilation of C99 programs
COMMON_CFLAGS = -Wall -std=c99 

ifeq ($(DEBUG), 1)
# -ggdb: Includes debugging information in the binary
# -Og: Optimizes the code for debugging
# -Wpedantic: Warns if non-standard C extensions are used
	CFLAGS = $(COMMON_CFLAGS) -ggdb -Og -Wpedantic -Wextra -Wformat=2 -Wundef -Wunreachable-code -Wlogical-op -Wstrict-prototypes -Wmissing-prototypes
else
# -O3: Optimizes the code for performance
	CFLAGS = $(COMMON_CFLAGS) -O3
endif

OBJECTS = main ring node-server connections routing read-lines util

COR: Makefile $(OBJECTS:=.c) $(OBJECTS:=.h)
	$(CC) -Wall -O3 -o COR $(OBJECTS:=.c)

clean:
	rm COR
