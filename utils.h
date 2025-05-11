#ifndef __UTILS_H
#define __UTILS_H

#include "state.h"
#include "common.h"
#include <string>

int is_dynamically_linked(const char *filename);
hash_type compress_tmp_file(FILE *fin, const char *out_path, int level);
FILE *decompress_file_tmp(const char *in_path);
void fcopy(char *source_filename, char *destination_filename);
int filecmp(const char *file1, const char *file2);
FILE *create_anonymous_tmp(const char *id, const char *mode);
#endif
