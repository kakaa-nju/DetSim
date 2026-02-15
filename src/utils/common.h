/*
 * common.h - Project-wide common definitions
 * 
 * Note: Basic types have been moved to src/core/types.h
 * This header now primarily serves as a compatibility wrapper
 * and defines project-specific macros.
 */

#ifndef __COMMON_H
#define __COMMON_H

#ifndef __USE_GNU
#define __USE_GNU
#endif

/* Include core types */
#include "types.h"

/* Debug flag (disabled by default) */
// #define DEBUG

#endif /* __COMMON_H */
