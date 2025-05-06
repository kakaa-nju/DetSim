#ifndef __FSSTATE_H
#define __FSSTATE_H

#include <stdint.h>
#include <string>
#include "cereal/archives/binary.hpp"
#include "cereal/types/string.hpp"
#include "common.h"

/* System global. Always in tracer so can be very big */
#define MAXFILEENTRY 128

#define MAXFD 128

typedef struct ptmc_filedesc {
  int fd;
  int pos;
  uint32_t flags;
  int mnt_id;
  int ino;
  std::string fname;

  template <class Archive>
    void serialize(Archive &ar) {
      ar(fd, pos, flags, mnt_id, ino, fname);
    }

} ptmc_filedesc;

#endif /* __FSSTATE_H */
