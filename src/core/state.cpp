#include "state.h"
#include "cereal/archives/binary.hpp"
#include "common.h"
#include "debug.h"
#include "fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "utils.h"
#include <assert.h>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <stack>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>

PTMC_STATE ptmc_state;
TSS state_tree;
LSS state_queue;
SSS state_set;

std::unordered_map<int, void *> rseq_struct;
std::unordered_map<int, int> rseq_len;

/* serialized process state don't include memory and mappings
 * memory will be recorded when dumping,
 * and mappings will be directly copied from procfs (?)
 */

/* ========================================================================
 * Syscall String Formatting
 * ======================================================================== */

namespace syscall_fmt {

/* Format: int fd, void *buf, size_t count (read/write style) */
static void format_fd_buf_count(char *buf, tracee_state *t, syscall_info *info)
{
  char *mem = (char *)t->read_snapshot_mem(info->args[1], info->args[2]);
  assert(mem);
  
  int pos = sprintf(buf, "%s(%ld, %p(\"", syscalls[info->nr], info->args[0],
                    (void *)info->args[1]);
  
  /* Escape non-printable characters */
  for (size_t i = 0; i < info->args[2]; i++)
  {
    uint8_t val = mem[i];
    if (isprint(mem[i]))
      pos += sprintf(buf + pos, "%c", mem[i]);
    else
      pos += sprintf(buf + pos, "\\%03u", val);
  }
  
  pos += sprintf(buf + pos, "\"), %ld) = %ld", info->args[2], info->rval);
  free(mem);
}

/* Format: int fd (close style) */
static void format_fd_only(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(%ld) = %ld", syscalls[info->nr], info->args[0], info->rval);
}

/* Format: int fd, const void *buf, size_t len, int flags, 
 *         const struct sockaddr *dest_addr, socklen_t addrlen (sendto) */
static void format_sendto(char *buf, tracee_state *t, syscall_info *info)
{
  char *mem = (char *)t->read_snapshot_mem(info->args[1], info->args[2]);
  assert(mem);
  
  int pos = sprintf(buf, "%s(%ld, \"", syscalls[info->nr], info->args[0]);
  
  for (size_t i = 0; i < info->args[2]; i++)
  {
    uint8_t val = mem[i];
    if (isprint(mem[i]))
      pos += sprintf(buf + pos, "%c", mem[i]);
    else
      pos += sprintf(buf + pos, "\\%03u", val);
  }
  
  pos += sprintf(buf + pos, "\", %ld, %ld, ", info->args[2], info->args[3]);
  free(mem);

  struct sockaddr_in *addr =
      (struct sockaddr_in *)t->read_snapshot_mem(info->args[4], info->args[5]);
  
  char ipbuf[INET_ADDRSTRLEN];
  const char *ipstr = inet_ntop(AF_INET, &(addr->sin_addr), ipbuf, sizeof(ipbuf));
  if (!ipstr) ipstr = "invalid";
  
  pos += sprintf(buf + pos, "{sin_family=AF_INET, sin_port=htons(%d), sin_addr=inet_addr(\"%s\")}",
                 ntohs(addr->sin_port), ipstr);
  free(addr);

  sprintf(buf + pos, ", %ld) = %ld", info->args[5], info->rval);
}

/* Format: recvfrom style (with output addr) */
static void format_recvfrom(char *buf, tracee_state *t, syscall_info *info)
{
  char *mem = (char *)t->read_snapshot_mem(info->args[1], info->args[2]);
  assert(mem);
  
  int pos = sprintf(buf, "%s(%ld, \"", syscalls[info->nr], info->args[0]);
  
  for (size_t i = 0; i < (size_t)info->rval; i++)
  {
    uint8_t val = mem[i];
    if (isprint(mem[i]))
      pos += sprintf(buf + pos, "%c", mem[i]);
    else
      pos += sprintf(buf + pos, "\\%03u", val);
  }
  
  pos += sprintf(buf + pos, "\", %ld, %ld, ", info->args[2], info->args[3]);
  free(mem);

  socklen_t *plen = (socklen_t *)t->read_snapshot_mem(info->args[5], sizeof(socklen_t));
  socklen_t addrlen = *plen;

  struct sockaddr_in *addr =
      (struct sockaddr_in *)t->read_snapshot_mem(info->args[4], addrlen);
  
  char ipbuf[INET_ADDRSTRLEN];
  const char *ipstr = inet_ntop(AF_INET, &(addr->sin_addr), ipbuf, sizeof(ipbuf));
  if (!ipstr) ipstr = "invalid";
  
  pos += sprintf(buf + pos, "{sin_family=AF_INET, sin_port=htons(%d), sin_addr=inet_addr(\"%s\")}",
                 ntohs(addr->sin_port), ipstr);
  free(addr);

  sprintf(buf + pos, ", [%d]) = %ld", addrlen, info->rval);
}

/* Format: void *addr (brk/mmap style) */
static void format_addr(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(0x%lx) = 0x%lx", syscalls[info->nr], 
          info->args[0], info->rval);
}

/* Format: int status (exit style) */
static void format_exit(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(%ld) = ?", syscalls[info->nr], info->args[0]);
}

/* Format: const char *pathname, int flags, mode_t mode (open style) */
static void format_openat(char *buf, tracee_state *t, syscall_info *info)
{
  char *path = (char *)t->read_snapshot_mem(info->args[1], 256);
  assert(path);
  
  const char *flag_str = "";
  int flags = info->args[2];
  if ((flags & O_RDWR) == O_RDWR) flag_str = "O_RDWR";
  else if (flags & O_WRONLY) flag_str = "O_WRONLY";
  else flag_str = "O_RDONLY";
  
  sprintf(buf, "openat(%ld, \"%s\", %s|0x%x, 0%03lo) = %ld",
          info->args[0], path, flag_str, flags & ~O_ACCMODE, info->args[3], info->rval);
  free(path);
}

/* Format: struct timeval *tv (gettimeofday style) */
static void format_gettimeofday(char *buf, tracee_state *t, syscall_info *info)
{
  struct timeval *tv = (struct timeval *)t->read_snapshot_mem(
      info->args[0], sizeof(struct timeval));
  assert(tv);
  
  sprintf(buf, "%s({tv_sec=%ld, tv_usec=%ld}, %p) = %ld",
          syscalls[info->nr], tv->tv_sec, tv->tv_usec,
          (void *)info->args[1], info->rval);
  free(tv);
}

/* Format: clock_nanosleep */
static void format_clock_nanosleep(char *buf, tracee_state *t, syscall_info *info)
{
  struct timespec *ts = (struct timespec *)t->read_snapshot_mem(
      info->args[2], sizeof(struct timespec));
  assert(ts);
  
  sprintf(buf, "clock_nanosleep(%ld, %ld, {tv_sec=%ld, tv_nsec=%ld}, %p) = %ld",
          info->args[0], info->args[1], ts->tv_sec, ts->tv_nsec,
          (void *)info->args[3], info->rval);
  free(ts);
}

/* Format: socket */
static void format_socket(char *buf, syscall_info *info)
{
  const char *domain_str = "AF_???";
  if (info->args[0] == AF_INET) domain_str = "AF_INET";
  else if (info->args[0] == AF_UNIX) domain_str = "AF_UNIX";
  
  const char *type_str = "SOCK_???";
  if (info->args[1] == SOCK_STREAM) type_str = "SOCK_STREAM";
  else if (info->args[1] == SOCK_DGRAM) type_str = "SOCK_DGRAM";
  
  sprintf(buf, "socket(%s, %s, %ld) = %ld",
          domain_str, type_str, info->args[2], info->rval);
}

/* Format: bind/listen/accept */
static void format_socket_fd(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(%ld, ...) = %ld", syscalls[info->nr], info->args[0], info->rval);
}

/* Format: default (6 arguments hex) */
static void format_default(char *buf, syscall_info *info)
{
  sprintf(buf, "%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) = %ld",
          syscalls[info->nr], info->args[0], info->args[1], info->args[2],
          info->args[3], info->args[4], info->args[5], info->rval);
}

/* Main formatting function */
void format(char *buf, tracee_state *t, syscall_info *info)
{
  switch (info->nr)
  {
    /* Network */
    case SYS_sendto:
      format_sendto(buf, t, info);
      break;
    case SYS_recvfrom:
      format_recvfrom(buf, t, info);
      break;
    case SYS_socket:
      format_socket(buf, info);
      break;
    case SYS_bind:
    case SYS_listen:
    case SYS_accept:
    case SYS_accept4:
      format_socket_fd(buf, info);
      break;
    case SYS_connect:
      format_socket_fd(buf, info);
      break;

    /* File I/O */
    case SYS_read:
    case SYS_write:
      format_fd_buf_count(buf, t, info);
      break;
    case SYS_openat:
      format_openat(buf, t, info);
      break;
    case SYS_close:
      format_fd_only(buf, info);
      break;

    /* Memory */
    case SYS_brk:
    case SYS_mmap:
      format_addr(buf, info);
      break;

    /* Time */
    case SYS_gettimeofday:
      format_gettimeofday(buf, t, info);
      break;
    case SYS_clock_nanosleep:
      format_clock_nanosleep(buf, t, info);
      break;

    /* Process */
    case SYS_exit:
    case SYS_exit_group:
      format_exit(buf, info);
      break;

    /* Simple fd-only */
    case SYS_sched_yield:
    case SYS_set_tid_address:
      format_fd_only(buf, info);
      break;

    /* Default */
    default:
      format_default(buf, info);
      break;
  }
}

} // namespace syscall_fmt

/* ========================================================================
 * Tracee State Operations
 * ======================================================================== */

static bytes *membuf;
static int membufsz = 4096;

void log_syscall(tracee_state *t)
{
  char buf[1024];
  syscall_fmt::format(buf, t, &t->si);
  LOG_TRACE("%s", buf);
}

void tracee_state::show_syscall(syscall_info *info)
{
  char buf[1024];
  syscall_fmt::format(buf, this, info);
  printf("%s\n", buf);
}

/* ========================================================================
 * Global State Operations
 * ======================================================================== */

sys_state::sys_state(struct syscall_info *info)
{
  /* construct all tracee for init_state */
  for (int j = 0; j < NP; j++)
  {
    child[j] = tracee_state(j, &info[j]);
    child[j].save_proc_full_data();
    exited[j] = ptmc_state.exited[j];
    ts_hash[j] = child[j].ts_hash;
    LOG_DEBUG("Create tstate " HASH_FORMAT, ts_hash[j]);
  }

  /* calc md5 xor */
  hash_type ss_hash_tmp = 0;
  for (int j = 0; j < NP; j++)
  {
    ss_hash_tmp ^= ts_hash[j];
  }

  /* handle hash collision */
  while (true)
  {
    if (!state_set.count(ss_hash_tmp))
      break;

    sys_state s(ss_hash_tmp);
    int i = 0;
    for (i = 0; i < NP; i++)
    {
      if (s.ts_hash[i] != ts_hash[i])
        break;
    }
    if (i == NP)
      goto ret;
    ss_hash_tmp++;
  }

ret:
  ss_hash = ss_hash_tmp;
  LOG_DEBUG("Create sstate " HASH_FORMAT, ss_hash_tmp);
}

void state_queue_append(sys_state *s) { state_queue.push_back(s->ss_hash); }

void state_queue_append_front(sys_state *s)
{
  state_queue.push_front(s->ss_hash);
}

sys_state *state_queue_extract()
{
  if (state_queue.empty())
    return NULL;
  sys_state *s = new sys_state(state_queue.front());
  state_queue.pop_front();
  return s;
}

void sys_state::save_shared_files()
{
  for (auto &shared_file : ptmc_state.shared_files)
  {
    auto sfs_filename =
        fileutils::format_hash_filename("filesystem", ".sfs", ss_hash);
    fileutils::copy_file(shared_file, sfs_filename);
  }
}

int sys_state::save_metadata()
{
  std::string path = fileutils::format_hash_filename("sstate", ".ss", ss_hash);
  LOG_DEBUG("to store sstate %08x", ss_hash);

  if (!fileutils::file_exists(path))
  {
    LOG_DEBUG("sys_state stores %08x", ss_hash);
    auto ofs_opt = fileutils::open_ofstream(path);
    if (!ofs_opt)
    {
      LOG_CRIT("Cannot write metadata to %s", path.c_str());
      return 0;
    }
    cereal::BinaryOutputArchive outputArchive(*ofs_opt);
    outputArchive(*this);
    save_shared_files();
    return 1;
  }
  return 0;
}

void state_tree_add(sys_state *s, sys_state *t, int which, int choose)
{
  if (state_tree.count(t->ss_hash) == 0)
    state_tree[t->ss_hash] =
        std::tuple<hash_type, int, int>(s->ss_hash, which, choose);
}

/* Format syscall for history display using syscall_fmt */
static void format_syscall_for_history(char *buf, hash_type ts_hash)
{
  tracee_state ts(ts_hash);
  syscall_fmt::format(buf, &ts, &ts.si);
}

void show_syscall_history()
{
  hash_type ss = ptmc_state.dest_state.ss_hash;
  std::stack<std::string> print_stack;
  int cnt = 0;
  
  while (state_tree.count(ss))
  {
    cnt++;
    hash_type pre = std::get<0>(state_tree[ss]);
    int which = std::get<1>(state_tree[ss]);
    int choose = std::get<2>(state_tree[ss]);
    
    sys_state s = sys_state(ss);
    
    char buf[1024];
    format_syscall_for_history(buf, s.ts_hash[which]);
    print_stack.push(std::string(buf) + "\n");
    
    if (choose >= 0)
      sprintf(buf, HASH_FORMAT " => " HASH_FORMAT "\n Tracee %d (choose %d): ", 
              pre, ss, which, choose);
    else
      sprintf(buf, HASH_FORMAT " => " HASH_FORMAT "\n Tracee %d: ", 
              pre, ss, which);
    print_stack.push(buf);
    ss = pre;
  }

  while (!print_stack.empty())
  {
    printf("%s", print_stack.top().c_str());
    print_stack.pop();
  }
  printf("%d steps in total.\n", cnt);
}

/* ========================================================================
 * Memory Operations
 * ======================================================================== */

#define BUFFER_SIZE 4096

static bool maps_item_eq(maps_item &a, maps_item &b)
{
  return a.start == b.start && a.end == b.end;
}

static bool existmaps_item(maps_item &a, std::vector<maps_item> array)
{
  /* heap is managed by SYS_brk */
  if (!strcmp(a.name, "[heap]"))
    return true;

  for (auto &item : array)
  {
    if (maps_item_eq(a, item))
      return true;
  }
  return false;
}

void *read_mem(hash_type ts_hash, int pid, uint64_t addr, long size)
{
  bytes *ret = (bytes *)malloc(size + 1);
  ret[size] = 0;

  auto maps_fp = fileutils::open_map_file(ts_hash);
  if (!maps_fp)
  {
    LOG_CRIT("Cannot open map file for hash %08x", ts_hash);
    return nullptr;
  }
  std::vector<maps_item> items;
  get_maps_item(items, maps_fp.get());

  auto mem_fp = fileutils::open_mem_file(ts_hash);
  if (!mem_fp)
  {
    LOG_CRIT("Cannot open memory dump file for hash %08x", ts_hash);
    return nullptr;
  }

  for (auto &item : items)
  {
    if (item.start > addr || addr >= item.end)
    {
      if (item.flags[1] == 'w' && item.start != available_memory)
        fseek(mem_fp.get(), item.end - item.start, SEEK_CUR);
      continue;
    }

    if (item.flags[1] == 'w' && item.start != available_memory)
    {
      fseek(mem_fp.get(), addr - item.start, SEEK_CUR);
      fread(ret, 1, size, mem_fp.get());
      LOG_TRACE("read addr %p from memory dump", (void *)addr);
    }
    else
    {
      std::string proc_path = fmt::format("/proc/{}/mem", pid);
      auto proc_fp = fileutils::open_cfile(proc_path, "r");
      if (!proc_fp)
      {
        LOG_CRIT("Failed to open %s", proc_path.c_str());
        free(ret);
        return nullptr;
      }
      fseek(proc_fp.get(), addr, SEEK_SET);
      fread(ret, 1, size, proc_fp.get());
      LOG_TRACE("read addr %p from procfs", (void *)addr);
    }
    break;
  }

  return ret;
}

void *tracee_state::read_snapshot_mem(uint64_t addr, long size)
{
  return read_mem(ts_hash, pid, addr, size);
}

std::vector<maps_item> tracee_state::recover_brk_mappings()
{
  std::vector<maps_item> maps_old, maps_new;

  std::string current_path = fmt::format("/proc/{}/maps", pid);
  auto current_maps_fp = fileutils::open_cfile(current_path, "r");
  if (!current_maps_fp)
  {
    LOG_CRIT("Failed to open %s", current_path.c_str());
    return {};
  }

  auto origin_maps_fp = fileutils::open_map_file(ts_hash);
  if (!origin_maps_fp)
  {
    LOG_CRIT("Failed to open mapping file for hash %08x", ts_hash);
    return {};
  }

  get_maps_item(maps_old, current_maps_fp.get());
  get_maps_item(maps_new, origin_maps_fp.get());

  for (auto &item_old : maps_old)
  {
    if (!existmaps_item(item_old, maps_new))
    {
      tracee_do_munmap(pid, item_old.start, item_old.end);
    }
  }

  for (auto &item_new : maps_new)
  {
    if (!existmaps_item(item_new, maps_old))
    {
      tracee_do_mmap(pid, item_new.start, item_new.end);
    }
  }

  tracee_do_syscall(pid, SYS_brk, brk, 0, 0, 0, 0, 0);
  LOG_DEBUG("recovered brk = 0x%x", brk);
  return maps_new;
}

void tracee_state::recover_file_descriptors(int index)
{
  return;
}

void tracee_state::recover_mem_reg_snapshot(std::vector<maps_item> &maps)
{
  auto dump_fp = fileutils::open_mem_file(ts_hash);
  if (!dump_fp)
  {
    LOG_CRIT("Cannot open dump memory for %08x", ts_hash);
    return;
  }

  std::string mem_path = fmt::format("/proc/{}/mem", pid);
  auto mem_fp = fileutils::open_cfile(mem_path, "r+");
  if (!mem_fp)
  {
    LOG_CRIT("Cannot open /proc/%d/mem for write", pid);
    return;
  }

  void *rseq = malloc(rseq_len[pid]);
  fseek(mem_fp.get(), (uintptr_t)rseq_struct[pid], SEEK_SET);
  fread(rseq, rseq_len[pid], 1, mem_fp.get());

  for (auto &item : maps)
  {
    if (item.flags[1] != 'w' || item.start == available_memory)
      continue;
    ssize_t size = item.end - item.start;

    if (size > membufsz)
    {
      free(membuf);
      membufsz = size;
      membuf = (bytes *)malloc(membufsz);
    }

    fread(membuf, 1, size, dump_fp.get());
    fseek(mem_fp.get(), item.start, SEEK_SET);
    fwrite(membuf, 1, size, mem_fp.get());

    LOG_TRACE("Restore mem %p-%p", (void *)item.start, (void *)item.end);
  }

  fseek(mem_fp.get(), (uintptr_t)rseq_struct[pid], SEEK_SET);
  fwrite(rseq, rseq_len[pid], 1, mem_fp.get());
  free(rseq);

  struct user_regs_struct regs;
  fread(&regs, sizeof(regs), 1, dump_fp.get());
  ptrace(PTRACE_SETREGS, pid, NULL, &regs);
}

void tracee_state::recover_proc_files()
{
  // TODO
}

void sys_state::recover_shared_files()
{
  for (auto &shared_file : ptmc_state.shared_files)
  {
    auto sfs_filename =
        fileutils::format_hash_filename("filesystem", ".sfs/", ss_hash) +
        shared_file;
    fileutils::copy_file(sfs_filename, shared_file);
  }
}

void tracee_state::recover_running_state(int index)
{
  auto maps = recover_brk_mappings();

  /* Goes before file descriptors */
  recover_proc_files();

  /* Goes before snapshot, for %RIP will change in recover_proc_files(). */
  recover_file_descriptors(index);

  recover_mem_reg_snapshot(maps);

  /* 3. restore msg and socket */
  ptmc_state.sock_lists[index] = sock_list;
  ptmc_state.tcp_buffer_lists[index] = tcp_buffer_list;
  ptmc_state.udp_buffer_lists[index] = udp_buffer_list;
  ptmc_state.fs_states[index] = fs_state;

  /* 4. time */
  ptmc_state.time[index] = tv;
}

sys_state::sys_state(hash_type hash)
{
  ss_hash = hash;
  std::string path = fileutils::format_hash_filename("sstate", ".ss", ss_hash);

  auto ifs_opt = fileutils::open_ifstream(path);
  if (!ifs_opt)
  {
    LOG_CRIT("Cannot read metadata from %s", path.c_str());
  }

  cereal::BinaryInputArchive inputArchive(*ifs_opt);
  inputArchive(*this);
}

void sys_state::recover_running_state()
{
  recover_shared_files();
  for (int i = 0; i < NP; i++)
  {
    child[i] = tracee_state(ts_hash[i]);

    if (exited[i] == 1)
      continue;

    child[i].recover_running_state(i);
  }
}

tracee_state::tracee_state(hash_type hash)
{
  ts_hash = hash;
  LOG_DEBUG("restore tracee_state = " HASH_FORMAT, hash);

  std::string path = fileutils::format_hash_filename("tstate", ".ts", hash);
  auto ifs_opt = fileutils::open_ifstream(path);

  if (!ifs_opt)
  {
    LOG_CRIT("Open traceeState file failed");
    exit(errno);
  }
  cereal::BinaryInputArchive inputArchive(*ifs_opt);
  inputArchive(*this);

  LOG_DEBUG("Deserialize: %d buffers", udp_buffer_list.size());
  for (auto &b : udp_buffer_list)
  {
    LOG_DEBUG("Deserialize: %d messages", b.second.size());
  }
}

uint64_t crc32(FILE *fp);
void tracee_state::create_mem_reg_snapshot()
{
  std::string maps_path = fmt::format("/proc/{}/maps", pid);
  std::string mem_path = fmt::format("/proc/{}/mem", pid);

  auto maps_fp = fileutils::open_cfile(maps_path, "r");
  auto mem_fp = fileutils::open_cfile(mem_path, "r");
  if (!maps_fp || !mem_fp)
  {
    LOG_CRIT("Failed to open /proc/{}/maps or mem", pid);
    exit(1);
  }

  FILE *dump = create_anonymous_tmp("dump", "r+b");
  assert(dump);

  char line[1024];
  while (fgets(line, sizeof(line), maps_fp.get()))
  {
    uint64_t start, end;
    char flags[5];
    uint32_t offset;
    int a, b, inode;
    char name[64] = {};
    sscanf(line, "%lx-%lx %s %x %d:%d %d %s", &start, &end, flags, &offset, &a,
           &b, &inode, name);

    if (flags[1] != 'w')
      continue;
    if (start == available_memory)
      continue;

    size_t region_size = end - start;
    if (region_size > (size_t)membufsz)
    {
      free(membuf);
      membufsz = region_size;
      membuf = (bytes *)malloc(membufsz);
    }

    fseek(mem_fp.get(), start, SEEK_SET);
    fread(membuf, region_size, 1, mem_fp.get());

    if ((uintptr_t)rseq_struct[pid] >= start &&
        (uintptr_t)rseq_struct[pid] < end)
    {
      uintptr_t offset = (uintptr_t)rseq_struct[pid] - start;
      memset(membuf + offset, 0x55, rseq_len[pid]);
    }

    fwrite(membuf, region_size, 1, dump);
    LOG_TRACE("dump %lx-%lx, %s", start, end, name);
  }

  struct user_regs_struct regs;
  ptrace(PTRACE_GETREGS, pid, NULL, &regs);
  fwrite(&regs, sizeof(regs), 1, dump);

  save_structure_data(dump);

  int footprint = rand();
  std::string tmp_name = fmt::format("/tmp/{:08x}.mem.zstd", footprint);
  hash_type hash = compress_tmp_file(dump, tmp_name.c_str(), 1);
  fclose(dump);

  while (true)
  {
    std::string dest =
        fileutils::format_hash_filename("memory", ".mem.zstd", hash);
    if (!fileutils::file_exists(dest))
      break;
    if (!filecmp(dest.c_str(), tmp_name.c_str()))
      goto done;
    hash++;
  }

  fileutils::copy_file(
      tmp_name, fileutils::format_hash_filename("memory", ".mem.zstd", hash));

done:
  fileutils::remove_file(tmp_name);
  ts_hash = hash;
}

void tracee_state::get_file_descriptors()
{
  return;
}

tracee_state::tracee_state(int which, struct syscall_info *info)
{
  /* 1. save syscallinfo to struct */
  si = *info;
  pid = ptmc_state.pids[which];

  /* 2. save fds */
  get_file_descriptors();

  /* 3. sockets in state are maintained while executing */
  sock_list = ptmc_state.sock_lists[which];
  tcp_buffer_list = ptmc_state.tcp_buffer_lists[which];
  udp_buffer_list = ptmc_state.udp_buffer_lists[which];
  fs_state = ptmc_state.fs_states[which];

  /* 4. time */
  tv = ptmc_state.time[which];
}

void tracee_state::save_structure_data(FILE *fp)
{
  std::stringstream ofs;
  cereal::BinaryOutputArchive outputArchive(ofs);
  outputArchive(*this);
  fwrite(ofs.str().c_str(), ofs.str().length(), 1, fp);
}

void tracee_state::save_structure_data()
{
  std::string path = fileutils::format_hash_filename("tstate", ".ts", ts_hash);
  auto ofs_opt = fileutils::open_ofstream(path);
  if (!ofs_opt)
  {
    LOG_CRIT("failed to open tstate file for ts_hash = %08x", ts_hash);
    return;
  }
  cereal::BinaryOutputArchive outputArchive(*ofs_opt);
  outputArchive(*this);
}

void tracee_state::save_mappings()
{
  std::string src = fmt::format("/proc/{}/maps", pid);
  std::string dest =
      fileutils::format_hash_filename("mappings", ".maps", ts_hash);

  fileutils::copy_file(src, dest);
}

void tracee_state::save_proc_files()
{
  // TODO
}

void tracee_state::save_proc_full_data()
{
  /* save brk */
  brk = tracee_do_syscall(pid, SYS_brk, 0, 0, 0, 0, 0, 0);
  LOG_DEBUG("recorded brk = 0x%x", brk);

  /* set ts_hash here */
  create_mem_reg_snapshot();

  /* save mappings */
  save_mappings();

  /* serialize */
  save_structure_data();

  /* save files */
  save_proc_files();
}

__attribute__((constructor)) void init_membuf()
{
  membuf = (bytes *)malloc(membufsz);
}
