#ifndef __FSSTATE_H
#define __FSSTATE_H

#include "common.h"
#include "fd_manager.h"
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

/* --- Forward declarations for guest memory operations --- */
void *memcpy_host2guest(void *dest, const void *src, size_t n);
void *memcpy_guest2host(void *dest, const void *src, size_t n);

/* --- VFS-based Syscall Handlers ---
 * These functions implement system calls using the virtual file system.
 * They follow the naming convention of other emulated syscalls (emu_*).
 */
long emu_vfs_openat(int dirfd, const char *path, int flags, mode_t mode);
long emu_vfs_read(int fd, void *buf, size_t count);
long emu_vfs_write(int fd, const void *buf, size_t count);
long emu_vfs_close(int fd);
long emu_vfs_lseek(int fd, off_t offset, int whence);
long emu_vfs_stat(const char *path, struct stat *statbuf);
long emu_vfs_fstat(int fd, struct stat *statbuf);

// Represents a node in the virtual file system. Can be a file or a directory.
struct VFSNode {
  // Complete file content. For directories, this is typically empty.
  std::vector<char> content;

  // File metadata.
  // When FSSTATE_DETAILED_METADATA is defined, full struct stat is stored.
  // Otherwise, only minimal metadata (size, mode) is kept to reduce state space.
#ifdef FSSTATE_DETAILED_METADATA
  struct stat metadata;
#else
  // Minimal metadata: only size and file type/mode
  struct {
    off_t st_size;
    mode_t st_mode;
  } metadata;
#endif

  template <class Archive> void serialize(Archive &ar);
};

// Represents an entry in a process's file descriptor table.
struct OpenFileDescription {
  // The absolute path of the file in the VFS.
  std::string path;

  // The current read/write offset into the file's content.
  off_t offset;

  // The flags used when opening the file (e.g., O_RDONLY, O_WRONLY, O_APPEND).
  int flags;

  template <class Archive> void serialize(Archive &ar);
};

// Manages the complete file system state for a single tracee.
class FileSystemState {
public:
  // The entire virtual file system, mapping absolute paths to VFSNodes.
  std::map<std::string, VFSNode> filesystem;

  // The table of open files for the process, mapping a file descriptor (int)
  // to its description.
  std::map<int, OpenFileDescription> open_files;

  // Current working directory for the tracee (in VFS namespace)
  std::string cwd;

  // Mappings from host directories into the VFS. Each pair is (host_base, target_base)
  std::vector<std::pair<std::string, std::string>> mappings;

private:
  // FdManager for unified fd allocation (shared with SockState)
  FdManagerPtr fd_manager_;

public:
  FileSystemState() = default;
  explicit FileSystemState(FdManagerPtr fd_mgr) : fd_manager_(fd_mgr) {}

  // Set the fd manager (must be called before using get_new_fd)
  void set_fd_manager(FdManagerPtr fd_mgr) { fd_manager_ = fd_mgr; }

  // Allocate a new file descriptor via FdManager
  int get_new_fd();

  template <class Archive> void serialize(Archive &ar);

  // --- Syscall Implementations ---
  // These will be implemented in fsstate.cpp
  int do_open(const std::string &path, int flags, mode_t mode);
  ssize_t do_read(int fd, void *buf, size_t count);
  ssize_t do_write(int fd, const void *buf, size_t count);
  int do_close(int fd);
  off_t do_lseek(int fd, off_t offset, int whence);
  int do_stat(const std::string &path, struct stat *statbuf);
  int do_fstat(int fd, struct stat *statbuf);

  // Initialize VFS from host mappings (may load files into memory)
  void init_from_mappings(const std::vector<std::pair<std::string, std::string>> &maps);

  // Resolve a pathname according to dirfd and cwd. Returns an absolute VFS path.
  std::string resolve_path(int dirfd, const std::string &path);

  // Set/get cwd
  void set_cwd(const std::string &path);
  const std::string &get_cwd() const;
};

#endif /* __FSSTATE_H */