#ifndef __UTILS_H
#define __UTILS_H

#include "common.h"
#include <string>

int compress_file(const char *in_path, const char *out_path, int level);
int decompress_file(const char *in_path, const char *out_path);
#endif
