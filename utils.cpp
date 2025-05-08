#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ptrace.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>
#include <stdlib.h>
#include <libgen.h>
#include "debug.h"
#include "common.h"
#include "state.h"

#define CHUNK_SIZE 16 KiB
#define BUFFER_SIZE 8 KiB

hash_type crc32(FILE *fp);
hash_type compress_file(const char* in_path, const char* out_path, int level) {
  FILE* fin = fopen(in_path, "rb");
  FILE* fout = fopen(out_path, "wb+");
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

  fseek(fout, 0, SEEK_SET);
  int ret = crc32(fout);

  fclose(fin);
  fclose(fout);
  free(in_buf);
  free(out_buf);
  return ret;
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

int filecmp(const char *file1, const char *file2) {
    FILE *fp1 = fopen(file1, "rb");
    FILE *fp2 = fopen(file2, "rb");

    if (!fp1 || !fp2) {
        fprintf(stderr, "Error opening files.\n");
        if (fp1) fclose(fp1);
        if (fp2) fclose(fp2);
        return -1;
    }

    unsigned char buf1[BUFFER_SIZE];
    unsigned char buf2[BUFFER_SIZE];

    size_t bytes_read1, bytes_read2;
    int result = 0;

    while (1) {
        bytes_read1 = fread(buf1, 1, BUFFER_SIZE, fp1);
        bytes_read2 = fread(buf2, 1, BUFFER_SIZE, fp2);

        if (bytes_read1 != bytes_read2) {
            result = 1; // size mismatch
            break;
        }

        if (bytes_read1 == 0) {
            // both files reached EOF
            break;
        }

        if (memcmp(buf1, buf2, bytes_read1) != 0) {
            result = 1; // content mismatch
            break;
        }
    }

    fclose(fp1);
    fclose(fp2);
    return result;
}

int mkdir_p(const char *path) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) 
    {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

void fcopy(char *source_filename, char *destination_filename) {
  int source_fd = open(source_filename, O_RDONLY);
  if (source_fd == -1) 
  {
    perror("Error opening source file");
    return;
  }

  char *dup = strdup(destination_filename);
  char *dir = dirname(dup);
  mkdir_p(dir);
  free(dup);

  int dest_fd = open(
      destination_filename, 
      O_WRONLY | O_CREAT | O_TRUNC,
      S_IRUSR | S_IWUSR
      );

  if (dest_fd == -1) 
  {
    perror("Error opening destination file");
    close(source_fd);
    return;
  }

  char buffer[BUFFER_SIZE];
  ssize_t bytes_read, bytes_written;

  while ((bytes_read = read(source_fd, buffer, BUFFER_SIZE)) > 0) 
  {
    bytes_written = write(dest_fd, buffer, bytes_read);
    if (bytes_written != bytes_read) 
    {
      perror("Error writing to destination file");
      close(source_fd);
      close(dest_fd);
      return;
    }
  }

  if (bytes_read == -1) 
  {
    perror("Error reading from source file");
    close(source_fd);
    close(dest_fd);
    return;
  }

  close(source_fd);
  close(dest_fd);
}

