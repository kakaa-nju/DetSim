#ifndef __COMMON_H
#define __COMMON_H

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdint.h>

#ifndef NP
#define NP 1
#endif

#define HASH_LEN 8

#define KiB *1024
#define MiB KiB * 1024
typedef uint8_t bytes;
// #define DEBUG

#endif /* __COMMON_H */
