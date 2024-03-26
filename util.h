#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

// ALLOCATION
// Like `malloc`, but terminates the program if allocation fails
void *malloc_f(size_t size);

// LOGGING
extern int verbose_level;
#define error(...) do { fprintf(stderr, "ERROR: " __VA_ARGS__); exit(1); } while (0)
#define warn(...) do { fprintf(stderr, "WARNING: " __VA_ARGS__); fflush(stderr); } while (0)
#define dbg_warn(...) do { fprintf(stderr, "DEBUG WARNING: " __VA_ARGS__); fflush(stderr); } while (0)
#define v_printf(...) do { if (verbose_level >= 1) { printf(__VA_ARGS__); fflush(stderr); } } while (0)
#define vv_printf(...) do { if (verbose_level >= 2) { printf(__VA_ARGS__); fflush(stderr); } } while (0)

// MACROS
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#endif
