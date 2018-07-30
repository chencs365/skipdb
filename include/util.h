#ifndef __UTIL_H
#define __UTIL_H

#include "status.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int compare(const void* p1, size_t l1, const void* p2, size_t l2);
uint8_t random_level(int max_level, float p);
status_t filemmap(int fd, uint64_t size, void** mapped);
status_t filemremap(int fd, void* old_mapped, size_t old_size, size_t new_size, void** new_mapped);
status_t ommap(const char* filename, uint64_t* mapcap, void** mapped);
status_t cmmap(const char* filename, uint64_t mapcap, void** mapped);

#endif // __UTIL_H
