#ifndef __UTILS_H
#define __UTILS_H

#include "state.h"
#include "common.h"
#include <string>

hash_type compress_file(const char *in_path, const char *out_path, int level);
int decompress_file(const char *in_path, const char *out_path);
void fcopy(char *source_filename, char *destination_filename);
int filecmp(const char *file1, const char *file2);
#endif
