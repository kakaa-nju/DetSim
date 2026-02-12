#include "fsstate.h"
#include "cereal/archives/binary.hpp"
#include "cereal/cereal.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/vector.hpp"
#include "debug.h"
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <libgen.h>
#include <algorithm>
#include <fstream>
#include <iostream>

/* --- Serialization Implementations --- */

template <class Archive> void VFSNode::serialize(Archive &ar) {
  ar(CEREAL_NVP(content), CEREAL_NVP(metadata.st_dev),
     CEREAL_NVP(metadata.st_ino), CEREAL_NVP(metadata.st_mode),
     CEREAL_NVP(metadata.st_nlink), CEREAL_NVP(metadata.st_uid),
     CEREAL_NVP(metadata.st_gid), CEREAL_NVP(metadata.st_rdev),
     CEREAL_NVP(metadata.st_size), CEREAL_NVP(metadata.st_blksize),
     CEREAL_NVP(metadata.st_blocks), CEREAL_NVP(metadata.st_atime),
     CEREAL_NVP(metadata.st_mtime), CEREAL_NVP(metadata.st_ctime));
}

template <class Archive> void OpenFileDescription::serialize(Archive &ar) {
  ar(CEREAL_NVP(path), CEREAL_NVP(offset), CEREAL_NVP(flags));
}

template <class Archive> void FileSystemState::serialize(Archive &ar) {
  ar(CEREAL_NVP(filesystem), CEREAL_NVP(open_files), CEREAL_NVP(next_fd));
}

/* --- FileSystemState Method Implementations --- */

int FileSystemState::get_new_fd() {
  while (open_files.count(next_fd)) {
    next_fd++;
  }
  return next_fd;
}

static std::string join_paths(const std::string &base, const std::string &rel) {
  if (rel.empty())
    return base;
  if (rel[0] == '/')
    return rel;
  if (base.empty() || base == "/")
    return std::string("/") + rel;
  std::string out = base;
  if (out.back() != '/')
    out += '/';
  out += rel;
  return out;
}

void FileSystemState::init_from_mappings(const std::vector<std::pair<std::string, std::string>> &maps) {
  mappings = maps;
  // Load files from host mapping into in-memory VFS. We do a shallow load
  // (files directly under host base and subdirectories) to provide examples
  // for tests. This is intentionally simple.
  for (const auto &m : mappings) {
    const std::string &host_base = m.first;
    const std::string &target_base = m.second;
    DIR *d = opendir(host_base.c_str());
    if (!d)
      continue;
    struct dirent *de;
    std::vector<std::string> stack;
    stack.push_back(host_base);
    while (!stack.empty()) {
      std::string cur = stack.back();
      stack.pop_back();
      DIR *cd = opendir(cur.c_str());
      if (!cd) continue;
      while ((de = readdir(cd)) != NULL) {
        if (de->d_name[0] == '.')
          continue;
        std::string host_p = cur + "/" + de->d_name;
        struct stat st;
        if (stat(host_p.c_str(), &st) < 0)
          continue;
        if (S_ISDIR(st.st_mode)) {
          stack.push_back(host_p);
          continue;
        }
        // file: load content
        std::ifstream ifs(host_p, std::ios::binary);
        std::vector<char> content;
        if (ifs) {
          ifs.seekg(0, std::ios::end);
          std::streamsize sz = ifs.tellg();
          ifs.seekg(0, std::ios::beg);
          if (sz > 0) {
            content.resize((size_t)sz);
            ifs.read(content.data(), sz);
          }
        }
        // compute target path
        std::string rel = host_p.substr(host_base.size());
        if (!rel.empty() && rel[0] == '/')
          rel = rel.substr(1);
        std::string target_p = target_base;
        if (target_p.back() != '/')
          target_p += '/';
        target_p += rel;

        VFSNode node;
        node.content = std::move(content);
        memset(&node.metadata, 0, sizeof(struct stat));
        node.metadata.st_mode = st.st_mode;
        node.metadata.st_size = node.content.size();
        node.metadata.st_nlink = 1;
        node.metadata.st_mtime = st.st_mtime;
        filesystem[target_p] = std::move(node);
      }
      closedir(cd);
    }
    closedir(d);
  }
}

void FileSystemState::set_cwd(const std::string &path) {
  if (path.empty())
    cwd = "/";
  else if (path[0] == '/')
    cwd = path;
  else
    cwd = join_paths(cwd, path);
}

const std::string &FileSystemState::get_cwd() const { return cwd; }

std::string FileSystemState::resolve_path(int dirfd, const std::string &path) {
  if (path.empty())
    return cwd.empty() ? std::string("/") : cwd;
  if (path[0] == '/') {
    return path;
  }
  // relative path
  std::string base;
  if (dirfd == AT_FDCWD) {
    base = cwd.empty() ? "/" : cwd;
  } else {
    auto it = open_files.find(dirfd);
    if (it != open_files.end()) {
      std::string p = it->second.path;
      // if it's a file, use its directory
      size_t pos = p.find_last_of('/');
      if (pos == std::string::npos)
        base = "/";
      else if (pos == 0)
        base = "/";
      else
        base = p.substr(0, pos);
    } else {
      base = cwd.empty() ? "/" : cwd;
    }
  }
  // normalize
  std::string combined = join_paths(base, path);
  // collapse /. and /.. (simple implementation)
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < combined.size()) {
    while (i < combined.size() && combined[i] == '/') i++;
    size_t j = i;
    while (j < combined.size() && combined[j] != '/') j++;
    if (j > i) {
      std::string part = combined.substr(i, j - i);
      if (part == ".") {
        // skip
      } else if (part == "..") {
        if (!parts.empty()) parts.pop_back();
      } else {
        parts.push_back(part);
      }
    }
    i = j + 1;
  }
  std::string out = "/";
  for (size_t k = 0; k < parts.size(); k++) {
    out += parts[k];
    if (k + 1 != parts.size()) out += '/';
  }
  if (out.empty()) out = "/";
  return out;
}

int FileSystemState::do_open(const std::string &path, int flags, mode_t mode) {
  // Simplified path handling for now. Assumes absolute paths.
  auto it = filesystem.find(path);
  bool exists = (it != filesystem.end());

  if (exists) {
    // File exists. Check permissions, etc. (to be implemented)
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
      errno = EEXIST;
      return -1;
    }
  } else {
    // File does not exist.
    if (flags & O_CREAT) {
      // Create a new file node
      VFSNode newNode;
      memset(&newNode.metadata, 0, sizeof(struct stat));
      newNode.metadata.st_mode = mode;
      newNode.metadata.st_nlink = 1;
      newNode.metadata.st_size = 0;
      newNode.metadata.st_uid = getuid();
      newNode.metadata.st_gid = getgid();
      newNode.metadata.st_atime = newNode.metadata.st_mtime =
          newNode.metadata.st_ctime = time(NULL);
      filesystem[path] = newNode;
    } else {
      errno = ENOENT;
      return -1;
    }
  }

  // If we got here, the file is accessible or was created.
  // Now, create an open file description for it.
  int fd = get_new_fd();
  OpenFileDescription ofd;
  ofd.path = path;
  ofd.flags = flags;
  ofd.offset = 0; // TODO: handle O_APPEND

  open_files[fd] = ofd;

  return fd;
}

ssize_t FileSystemState::do_read(int fd, void *buf, size_t count) {
  auto it = open_files.find(fd);
  if (it == open_files.end()) {
    errno = EBADF;
    return -1;
  }

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);

  if ((ofd.flags & O_ACCMODE) == O_WRONLY) {
    errno = EBADF;
    return -1;
  }

  ssize_t readable_bytes = node.content.size() - ofd.offset;
  if (readable_bytes <= 0) {
    return 0; // EOF
  }

  ssize_t bytes_to_read = std::min((ssize_t)count, readable_bytes);
  memcpy(buf, node.content.data() + ofd.offset, bytes_to_read);
  ofd.offset += bytes_to_read;

  return bytes_to_read;
}

ssize_t FileSystemState::do_write(int fd, const void *buf, size_t count) {
  auto it = open_files.find(fd);
  if (it == open_files.end()) {
    errno = EBADF;
    return -1;
  }

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);

  if ((ofd.flags & O_ACCMODE) == O_RDONLY) {
    errno = EBADF;
    return -1;
  }

  // Handle append mode
  if (ofd.flags & O_APPEND) {
    ofd.offset = node.content.size();
  }

  size_t new_size = ofd.offset + count;
  if (new_size > node.content.size()) {
    node.content.resize(new_size);
  }

  memcpy(node.content.data() + ofd.offset, buf, count);
  ofd.offset += count;
  node.metadata.st_size = node.content.size();
  node.metadata.st_mtime = time(NULL);

  return count;
}

int FileSystemState::do_close(int fd) {
  if (open_files.erase(fd) == 0) {
    errno = EBADF;
    return -1;
  }
  return 0;
}

off_t FileSystemState::do_lseek(int fd, off_t offset, int whence) {
  auto it = open_files.find(fd);
  if (it == open_files.end()) {
    errno = EBADF;
    return -1;
  }

  OpenFileDescription &ofd = it->second;
  VFSNode &node = filesystem.at(ofd.path);
  off_t new_offset;

  switch (whence) {
  case SEEK_SET:
    new_offset = offset;
    break;
  case SEEK_CUR:
    new_offset = ofd.offset + offset;
    break;
  case SEEK_END:
    new_offset = node.content.size() + offset;
    break;
  default:
    errno = EINVAL;
    return -1;
  }

  if (new_offset < 0) {
    errno = EINVAL;
    return -1;
  }

  ofd.offset = new_offset;
  return new_offset;
}

int FileSystemState::do_stat(const std::string &path, struct stat *statbuf) {
  auto it = filesystem.find(path);
  if (it == filesystem.end()) {
    errno = ENOENT;
    return -1;
  }

  memcpy(statbuf, &it->second.metadata, sizeof(struct stat));
  return 0;
}

int FileSystemState::do_fstat(int fd, struct stat *statbuf) {
  auto it = open_files.find(fd);
  if (it == open_files.end()) {
    errno = EBADF;
    return -1;
  }

  return do_stat(it->second.path, statbuf);
}

template void VFSNode::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void VFSNode::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);

template void OpenFileDescription::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void OpenFileDescription::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);

template void FileSystemState::serialize<cereal::BinaryInputArchive>(
    cereal::BinaryInputArchive &);
template void FileSystemState::serialize<cereal::BinaryOutputArchive>(
    cereal::BinaryOutputArchive &);