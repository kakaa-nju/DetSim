#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>
#include <stdlib.h>
#include "debug.h"
#include "common.h"

/* ptrace with error check */
long ptrace_right(enum __ptrace_request op, pid_t pid, void *addr, void *data) {
  /* ptrace(2): On success, the PTRACE_PEEK* operations return the requested *
   * data ... and other operations return zero. On error, all operations     *
   * return -1, and errno is set to indicate the error. Since the value      *
   * returned by a successful PTRACE_PEEK* operation may be -1, the caller   *
   * must clear errno before the call, and then check it ...                 */
  errno = 0;
  int result = ptrace(op, pid, addr, data);
  if (errno)
    panic("ptrace: %s", strerror(errno));
  return result;
}

#define CHUNK_SIZE 16 KiB

int compress_file(const char* in_path, const char* out_path, int level) {
  FILE* fin = fopen(in_path, "rb");
  FILE* fout = fopen(out_path, "wb");
  if (!fin || !fout) {
    perror("fopen");
    return 1;
  }

  void* in_buf = malloc(CHUNK_SIZE);
  void* out_buf = malloc(ZSTD_compressBound(CHUNK_SIZE));
  if (!in_buf || !out_buf) {
    fprintf(stderr, "Memory allocation failed\n");
    return 1;
  }

  size_t read_size;
  while ((read_size = fread(in_buf, 1, CHUNK_SIZE, fin)) > 0) {
    size_t out_size = ZSTD_compress(out_buf, ZSTD_compressBound(read_size),
        in_buf, read_size, level);
    if (ZSTD_isError(out_size)) {
      fprintf(stderr, "Compression error: %s\n", ZSTD_getErrorName(out_size));
      return 1;
    }
    fwrite(&out_size, sizeof(size_t), 1, fout);
    fwrite(out_buf, 1, out_size, fout);
  }

  fclose(fin);
  fclose(fout);
  free(in_buf);
  free(out_buf);
  return 0;
}

int decompress_file(const char* in_path, const char* out_path) {
  FILE* fin = fopen(in_path, "rb");
  FILE* fout = fopen(out_path, "wb");
  if (!fin || !fout) {
    perror("fopen");
    return 1;
  }

  void* in_buf = malloc(ZSTD_compressBound(CHUNK_SIZE));
  void* out_buf = malloc(CHUNK_SIZE);
  if (!in_buf || !out_buf) {
    fprintf(stderr, "Memory allocation failed\n");
    return 1;
  }

  size_t chunk_size;
  while (fread(&chunk_size, sizeof(size_t), 1, fin) == 1) {
    if (fread(in_buf, 1, chunk_size, fin) != chunk_size) {
      fprintf(stderr, "Unexpected EOF\n");
      return 1;
    }
    size_t out_size = ZSTD_decompress(out_buf, CHUNK_SIZE, in_buf, chunk_size);
    if (ZSTD_isError(out_size)) {
      fprintf(stderr, "Decompression error: %s\n", ZSTD_getErrorName(out_size));
      return 1;
    }
    fwrite(out_buf, 1, out_size, fout);
  }

  fclose(fin);
  fclose(fout);
  free(in_buf);
  free(out_buf);
  return 0;
}
