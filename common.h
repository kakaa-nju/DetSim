#ifndef __COMMON_H
#define __COMMON_H

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdint.h>

#ifndef NP
#define NP 1
#endif

/* Define this to enable detailed file metadata (timestamps, owner, etc.)
 * This will cause state explosion due to timestamp changes on every write.
 * When undefined, only file size is tracked. */
// #define FSSTATE_DETAILED_METADATA

#define HASH_LEN 8

#define KiB *1024
#define MiB KiB * 1024
typedef uint8_t bytes;
// #define DEBUG

#endif /* __COMMON_H */
