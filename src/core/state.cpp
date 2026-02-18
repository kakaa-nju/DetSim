/*
 * state.cpp - State management
 *
 * This module handles:
 * - Global state variables (state tree, queue, set)
 * - State creation from running processes
 * - State persistence (save/load to disk)
 * - State recovery (restore running process from snapshot)
 * - Memory operations for state capture/restore
 */

#include "state.h"
#include "syscall_fmt.h"
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
#include <sys/uio.h>
#include <sys/user.h>

/* ======================================================================
 * Section 1: Global State Variables
 * ====================================================================== */

PTMC_STATE ptmc_state;
TSS state_tree;
LSS state_queue;
SSS state_set;

std::unordered_map<int, void *> rseq_struct;
std::unordered_map<int, int> rseq_len;

static bytes *membuf;
static int membufsz = 4096;

/* ======================================================================
 * Section 2: State Logging and Display
 * ====================================================================== */

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

/* Format syscall for history display */
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

/* ======================================================================
 * Section 3: State Creation (from running processes)
 * ====================================================================== */

/* Create sys_state from running processes' syscall info */
sys_state::sys_state(struct syscall_info *info)
{
  /* Create all tracee states */
  for (int j = 0; j < NP; j++)
  {
    child[j] = tracee_state(j, &info[j]);
    child[j].save_full_state();
    exited[j] = ptmc_state.exited[j];
    ts_hash[j] = child[j].ts_hash;
    LOG_DEBUG("Create tstate " HASH_FORMAT, ts_hash[j]);
  }

  /* Calculate hash by XORing all tracee hashes */
  hash_type ss_hash_tmp = 0;
  for (int j = 0; j < NP; j++)
  {
    ss_hash_tmp ^= ts_hash[j];
  }

  /* Handle hash collision */
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

/* ======================================================================
 * Section 4: State Persistence (save/load to disk)
 * ====================================================================== */

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

/* Load sys_state from disk by hash */
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

/* Serialize to stream (for memory dump) */
void tracee_state::serialize_to_stream(FILE *fp)
{
  std::stringstream ofs;
  cereal::BinaryOutputArchive outputArchive(ofs);
  outputArchive(*this);
  fwrite(ofs.str().c_str(), ofs.str().length(), 1, fp);
}

/* Serialize to disk file */
void tracee_state::serialize_to_disk()
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

/* ======================================================================
 * Section 5: State Recovery (restore running process from snapshot)
 * ====================================================================== */

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

/* Load tracee_state from disk */
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

/* Recover memory mappings for brk */
void tracee_state::restore_memory_mappings(std::vector<maps_item> &maps_out)
{
  std::vector<maps_item> maps_old;
  maps_out.clear();

  std::string current_path = fmt::format("/proc/{}/maps", pid);
  auto current_maps_fp = fileutils::open_cfile(current_path, "r");
  if (!current_maps_fp)
  {
    LOG_CRIT("Failed to open %s", current_path.c_str());
    return;
  }

  auto origin_maps_fp = fileutils::open_map_file(ts_hash);
  if (!origin_maps_fp)
  {
    LOG_CRIT("Failed to open mapping file for hash %08x", ts_hash);
    return;
  }

  get_maps_item(maps_old, current_maps_fp.get());
  get_maps_item(maps_out, origin_maps_fp.get());

  for (auto &item_old : maps_old)
  {
    if (!mapping_exists(item_old, maps_out))
    {
      tracee_do_munmap(pid, item_old.start, item_old.end);
    }
  }

  for (auto &item_new : maps_out)
  {
    if (!mapping_exists(item_new, maps_old))
    {
      tracee_do_mmap(pid, item_new.start, item_new.end);
    }
  }

  tracee_do_syscall(pid, SYS_brk, brk, 0, 0, 0, 0, 0);
  LOG_DEBUG("recovered brk = 0x%x", brk);
}

/* Recover file descriptors */
void tracee_state::recover_file_descriptors(int index)
{
  return;
}

/* Recover memory and registers from snapshot */
void tracee_state::recover_mem_reg_snapshot(std::vector<maps_item> &maps)
{
  auto dump_fp = fileutils::open_mem_file(ts_hash);
  if (!dump_fp)
  {
    LOG_CRIT("Cannot open dump memory for %08x", ts_hash);
    return;
  }

  /* Read current rseq structure to preserve it */
  void *rseq = malloc(rseq_len[pid]);
  tracee_read_mem(pid, rseq_struct[pid], rseq, rseq_len[pid]);

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
    tracee_write_mem(pid, (void *)item.start, membuf, size);

    LOG_TRACE("Restore mem %p-%p", (void *)item.start, (void *)item.end);
  }

  /* Restore the preserved rseq structure */
  tracee_write_mem(pid, rseq_struct[pid], rseq, rseq_len[pid]);
  free(rseq);

  struct user_regs_struct regs;
  fread(&regs, sizeof(regs), 1, dump_fp.get());
  ptrace(PTRACE_SETREGS, pid, NULL, &regs);
}

void tracee_state::recover_proc_files()
{
  // TODO
}

/* Main entry: recover running state for a process */
void tracee_state::recover_running_state(int index)
{
  std::vector<maps_item> maps;
  restore_memory_mappings(maps);

  /* Goes before file descriptors */
  recover_proc_files();

  /* Goes before snapshot, for %RIP will change in recover_proc_files(). */
  recover_file_descriptors(index);

  recover_mem_reg_snapshot(maps);

  /* Restore subsystems */
  ptmc_state.sock_lists[index] = sock_list;
  ptmc_state.tcp_buffer_lists[index] = tcp_buffer_list;
  ptmc_state.udp_buffer_lists[index] = udp_buffer_list;
  ptmc_state.fs_states[index] = fs_state;

  /* Restore time */
  ptmc_state.time[index] = tv;
}

/* ======================================================================
 * Section 6: State Queue & Tree Management
 * ====================================================================== */

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

void state_tree_add(sys_state *s, sys_state *t, int which, int choose)
{
  if (state_tree.count(t->ss_hash) == 0)
    state_tree[t->ss_hash] =
        std::tuple<hash_type, int, int>(s->ss_hash, which, choose);
}

/* ======================================================================
 * Section 7: Memory Capture
 * ====================================================================== */

uint64_t crc32(FILE *fp);

/* Capture memory and registers, create snapshot */
void tracee_state::capture_memory_state()
{
  std::string maps_path = fmt::format("/proc/{}/maps", pid);

  auto maps_fp = fileutils::open_cfile(maps_path, "r");
  if (!maps_fp)
  {
    LOG_CRIT("Failed to open /proc/{}/maps", pid);
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

    /* Use process_vm_readv instead of /proc/pid/mem */
    struct iovec local = { membuf, region_size };
    struct iovec remote = { (void *)start, region_size };
    ssize_t n = syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0);
    if ((size_t)n != region_size)
    {
      LOG_ERROR("process_vm_readv failed for region %p-%p: %s", 
                (void *)start, (void *)end, strerror(errno));
      memset(membuf, 0, region_size);
    }

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

  serialize_to_stream(dump);

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

/* ======================================================================
 * Section 8: Tracee State Lifecycle
 * ====================================================================== */

/* Create tracee_state from running process (capture) */
tracee_state::tracee_state(int which, struct syscall_info *info)
{
  /* Save syscall info */
  si = *info;
  pid = ptmc_state.pids[which];

  /* Save file descriptors */
  get_file_descriptors();

  /* Save subsystems */
  sock_list = ptmc_state.sock_lists[which];
  tcp_buffer_list = ptmc_state.tcp_buffer_lists[which];
  udp_buffer_list = ptmc_state.udp_buffer_lists[which];
  fs_state = ptmc_state.fs_states[which];

  /* Save time */
  tv = ptmc_state.time[which];
}

void tracee_state::get_file_descriptors()
{
  return;
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

/* Full save: capture memory, mappings, and serialize */
void tracee_state::save_full_state()
{
  /* Record brk */
  brk = tracee_do_syscall(pid, SYS_brk, 0, 0, 0, 0, 0, 0);
  LOG_DEBUG("recorded brk = 0x%x", brk);

  /* Capture memory state */
  capture_memory_state();

  /* Save mappings */
  save_mappings();

  /* Serialize structure */
  serialize_to_disk();

  /* Save files */
  save_proc_files();
}

/* ======================================================================
 * Section 9: Memory Operations
 * ====================================================================== */

/* Read memory from snapshot by hash */
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
      /* Use process_vm_readv instead of /proc/pid/mem */
      tracee_read_mem(pid, (void *)addr, ret, size);
      LOG_TRACE("read addr %p from process_vm_readv", (void *)addr);
    }
    break;
  }

  return ret;
}

void *tracee_state::read_snapshot_mem(uint64_t addr, long size)
{
  return read_mem(ts_hash, pid, addr, size);
}

/* ======================================================================
 * Section 10: Helper Functions
 * ====================================================================== */

void get_maps_item(std::vector<maps_item> &items, FILE *maps)
{
  maps_item item;
  char line[1024];
  while (fgets(line, 1024, maps) != NULL)
  {
    sscanf(line, "%lx-%lx %s %x %d:%d %d %s", &item.start, &item.end,
           item.flags, &item.offset, &item.a, &item.b, &item.inode, item.name);
    items.emplace_back(item);
  }
}

static bool maps_item_eq(maps_item &a, maps_item &b)
{
  return a.start == b.start && a.end == b.end;
}

bool mapping_exists(maps_item &a, std::vector<maps_item> array)
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

/* ======================================================================
 * Section 11: Statistics
 * ====================================================================== */

void state_stats_print()
{
  printf("=== State Statistics ===\n");
  printf("Queue size:   %zu\n", state_queue.size());
  printf("Tree nodes:   %zu\n", state_tree.size());
  printf("States found: %zu\n", state_set.size());
  printf("=======================\n");
}

/* ======================================================================
 * Initialization
 * ====================================================================== */

__attribute__((constructor)) void init_membuf()
{
  membuf = (bytes *)malloc(membufsz);
}
