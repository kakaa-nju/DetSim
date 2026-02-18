/*
 * state.cpp - State management
 */

#include "state.h"
#include "syscall_fmt.h"
#include "cereal/archives/binary.hpp"
#include "debug.h"
#include "fsstate.h"
#include "guest.h"
#include "monitor.h"
#include "utils.h"
#include "state_store.h"
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
  buf[1023] = '\0';  /* Ensure null termination */
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

void tracee_state::serialize_to_disk()
{
  // Now integrated into capture_memory_state, no separate file needed
  // This function is kept for compatibility but does nothing
}

/* ======================================================================
 * Section 5: State Recovery (restore running process from snapshot)
 * ====================================================================== */

/* Load tracee_state from integrated StateStore data */
tracee_state::tracee_state(hash_type hash)
{
  // Load integrated data from StateStore
  std::vector<uint8_t> data;
  ssize_t data_size = StateStore::instance().load(hash, data);
  
  if (data_size < 0) {
    LOG_CRIT("Cannot load tracee_state for hash %08x", hash);
    return;
  }
  
  // Parse header
  if (data.size() < sizeof(StateDataHeader)) {
    LOG_CRIT("Invalid state data for hash %08x: too small", hash);
    return;
  }
  
  StateDataHeader* header = (StateDataHeader*)data.data();
  if (header->magic != 0x4453544D) {
    LOG_CRIT("Invalid state data for hash %08x: bad magic", hash);
    return;
  }
  
  // Deserialize tracee_state from integrated data
  if (header->version >= 2 && header->tstate_size > 0) {
    if (header->tstate_offset + header->tstate_size <= data.size()) {
      std::istringstream iss(std::string(
        (char*)data.data() + header->tstate_offset, header->tstate_size));
      try {
        cereal::BinaryInputArchive ar(iss);
        ar(*this);
      } catch (...) {
        LOG_ERROR("Failed to deserialize tracee_state for hash %08x", hash);
      }
    }
  } else {
    // Fallback: load from separate .ts file (legacy)
    std::string path = fileutils::format_hash_filename("tstate", ".ts", hash);
    auto ifs_opt = fileutils::open_ifstream(path);
    if (ifs_opt) {
      try {
        cereal::BinaryInputArchive inputArchive(*ifs_opt);
        inputArchive(*this);
      } catch (...) {
        LOG_ERROR("Failed to load tracee_state from legacy file %s", path.c_str());
      }
    }
  }
  
  ts_hash = hash;  // Set the hash explicitly
}

/* Load sys_state from disk by hash */
sys_state::sys_state(hash_type hash)
{
  ss_hash = hash;
  std::string path = fileutils::format_hash_filename("sstate", ".ss", hash);
  auto ifs_opt = fileutils::open_ifstream(path);
  
  if (ifs_opt) {
    try {
      cereal::BinaryInputArchive inputArchive(*ifs_opt);
      inputArchive(*this);
    } catch (...) {
      LOG_CRIT("Failed to load sys_state from %s", path.c_str());
    }
  }
  
  // Load child states from integrated data
  for (int i = 0; i < NP; i++) {
    if (!exited[i]) {
      child[i] = tracee_state(ts_hash[i]);
    }
  }
}

void tracee_state::restore_memory_mappings(std::vector<maps_item> &maps_out)
{
  /* Load saved mappings from StateStore */
  std::vector<maps_item> maps_saved;
  {
    std::vector<uint8_t> data;
    ssize_t size = StateStore::instance().load(ts_hash, data);
    
    if (size > 0 && data.size() >= sizeof(StateDataHeader)) {
      StateDataHeader* header = (StateDataHeader*)data.data();
      /* Try to read from integrated maps text (version 3+) */
      if (header->magic == 0x4453544D && header->version >= 3 && header->maps_size > 0) {
        if (header->maps_offset + header->maps_size <= data.size()) {
          const char* maps_text = (const char*)data.data() + header->maps_offset;
          std::istringstream iss(std::string(maps_text, header->maps_size));
          std::string line;
          while (std::getline(iss, line)) {
            maps_item item = {};
            memset(&item, 0, sizeof(item));
            uint32_t offset, a, b, inode;
            if (sscanf(line.c_str(), "%lx-%lx %4s %x %d:%d %d %511s",
                       &item.start, &item.end, item.flags, &offset, &a, &b, &inode, item.name) >= 7) {
              maps_saved.push_back(item);
            }
          }
        }
      }
    }
  }
  
  if (maps_saved.empty()) {
    /* Fallback: load from separate .maps file */
    LOG_WARN("StateStore includes no mappings for tshash = %x", ts_hash);
    std::string maps_path = fileutils::format_hash_filename("mappings", ".maps", ts_hash);
    auto maps_fp = fileutils::open_cfile(maps_path, "r");
    if (maps_fp) {
      get_maps_item(maps_saved, maps_fp.get());
    }
  }
  
  /* Get current mappings */
  std::string maps_path = fmt::format("/proc/{}/maps", pid);
  auto maps_fp = fileutils::open_cfile(maps_path, "r");
  std::vector<maps_item> maps_old;
  if (maps_fp) {
    get_maps_item(maps_old, maps_fp.get());
  }
  
  LOG_DEBUG("new maps has %d and old maps has %d items", maps_saved.size(), maps_old.size());
  
  /* Unmap regions that exist in current but not in saved */
  for (auto &item_old : maps_old)
  {
    LOG_TRACE("old item: [%lx-%lx] %s", item_old.start, item_old.end, item_old.name);
    if (!mapping_exists(item_old, maps_saved))
    {
      tracee_do_munmap(pid, item_old.start, item_old.end);
    }
  }
  
  /* Map regions that exist in saved but not in current */
  for (auto &item_new : maps_saved)
  {
    LOG_TRACE("new item: [%lx-%lx] %s", item_new.start, item_new.end, item_new.name);
    if (!mapping_exists(item_new, maps_old))
    {
      tracee_do_mmap(pid, item_new.start, item_new.end);
    }
  }
  
  /* Return saved mappings for further processing */
  maps_out = maps_saved;
}

void tracee_state::recover_file_descriptors(int index)
{
  // TODO
}

/* Recover memory and registers from snapshot using StateStore */
void tracee_state::recover_mem_reg_snapshot(std::vector<maps_item> &maps)
{
  // Load integrated data from StateStore
  std::vector<uint8_t> data;
  ssize_t data_size = StateStore::instance().load(ts_hash, data);
  
  if (data_size < 0) {
    LOG_CRIT("Cannot load state for hash %08x", ts_hash);
    return;
  }
  
  // Parse header
  if (data.size() < sizeof(StateDataHeader)) {
    LOG_CRIT("Invalid state data for hash %08x: too small", ts_hash);
    return;
  }
  
  StateDataHeader* header = (StateDataHeader*)data.data();
  if (header->magic != 0x4453544D) {
    LOG_CRIT("Invalid state data format for hash %08x", ts_hash);
    return;
  }
  
  RegionInfo* regions = (RegionInfo*)(data.data() + sizeof(StateDataHeader));
  
  /* Read current rseq structure to preserve it */
  void *rseq = malloc(rseq_len[pid]);
  tracee_read_mem(pid, rseq_struct[pid], rseq, rseq_len[pid]);

  // Restore each region using the header info
  for (uint32_t i = 0; i < header->num_regions; i++) {
    uint64_t start = regions[i].start;
    uint64_t end = regions[i].end;
    uint64_t offset = regions[i].offset;
    ssize_t size = end - start;
    
    if (size <= 0) continue;
    
    // Check if this region is still valid in current maps
    bool valid = false;
    for (auto &item : maps) {
      if (item.start <= start && end <= item.end && item.flags[1] == 'w') {
        valid = true;
        break;
      }
    }
    if (!valid) {
      LOG_WARN("Region %lx-%lx not found in current maps, skipping", start, end);
      continue;
    }

    if (size > membufsz)
    {
      free(membuf);
      membufsz = size;
      membuf = (bytes *)malloc(membufsz);
    }

    // Read from loaded data
    if (offset + size <= (ssize_t)data.size()) {
      memcpy(membuf, data.data() + offset, size);
      tracee_write_mem(pid, (void *)start, membuf, size);
    }

    LOG_TRACE("Restore mem %p-%p", (void *)start, (void *)end);
  }

  /* Restore the preserved rseq structure */
  tracee_write_mem(pid, rseq_struct[pid], rseq, rseq_len[pid]);
  free(rseq);

  // Read registers from data
  if (header->regs_offset + sizeof(struct user_regs_struct) <= data.size()) {
    struct user_regs_struct regs;
    memcpy(&regs, data.data() + header->regs_offset, sizeof(regs));
    ptrace(PTRACE_SETREGS, pid, NULL, &regs);
    LOG_TRACE("Restored registers for hash %08x", ts_hash);
  } else {
    LOG_ERROR("Invalid register offset in state data for hash %08x", ts_hash);
  }
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

  /* Restore brk (heap) - must be done before restoring memory content */
  tracee_do_syscall(pid, SYS_brk, brk, 0, 0, 0, 0, 0);
  LOG_DEBUG("restored brk = 0x%x", brk);

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

/* Capture memory, registers, and tracee_state, store via StateStore */
void tracee_state::capture_memory_state()
{
  std::string maps_path = fmt::format("/proc/{}/maps", pid);

  auto maps_fp = fileutils::open_cfile(maps_path, "r");
  if (!maps_fp)
  {
    LOG_CRIT("Failed to open /proc/{}/maps", pid);
    exit(1);
  }

  // First pass: collect region info
  std::vector<RegionInfo> regions;
  std::vector<std::vector<uint8_t>> region_data;
  
  char line[1024];
  while (fgets(line, sizeof(line), maps_fp.get()))
  {
    uint64_t start, end;
    char flags[5];
    uint32_t offset;
    int a, b, inode;
    char name[64] = {};
    sscanf(line, "%lx-%lx %4s %x %d:%d %d %63s", &start, &end, flags, &offset, &a,
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
      uintptr_t rseq_offset = (uintptr_t)rseq_struct[pid] - start;
      memset(membuf + rseq_offset, 0x55, rseq_len[pid]);
    }

    RegionInfo info;
    info.start = start;
    info.end = end;
    regions.push_back(info);
    
    std::vector<uint8_t> data(region_size);
    memcpy(data.data(), membuf, region_size);
    region_data.push_back(std::move(data));
    
    LOG_TRACE("dump %lx-%lx, %s", start, end, name);
  }

  // Serialize tracee_state (without ts_hash - will be set after save)
  std::vector<uint8_t> tstate_data;
  {
    std::ostringstream oss;
    {
      cereal::BinaryOutputArchive ar(oss);
      ar(*this);
    }
    std::string str = oss.str();
    tstate_data.assign(str.begin(), str.end());
  }
  
  // Read mappings data from /proc/PID/maps
  // Note: procfs files don't support fseek/ftell, must read until EOF
  std::vector<uint8_t> maps_data;
  {
    std::string maps_path = fmt::format("/proc/{}/maps", pid);
    LOG_TRACE("Reading maps from %s", maps_path.c_str());
    FILE* maps_fp = fopen(maps_path.c_str(), "r");
    if (maps_fp) {
      char buffer[4096];
      size_t total_read = 0;
      while (!feof(maps_fp)) {
        size_t n = fread(buffer, 1, sizeof(buffer), maps_fp);
        if (n > 0) {
          maps_data.insert(maps_data.end(), buffer, buffer + n);
          total_read += n;
        }
        if (ferror(maps_fp) && !feof(maps_fp)) {
          LOG_ERROR("Error reading maps file");
          break;
        }
      }
      fclose(maps_fp);
      LOG_TRACE("Maps data read: %zu bytes, data size: %zu", total_read, maps_data.size());
    } else {
      LOG_ERROR("Failed to open %s", maps_path.c_str());
    }
  }
  
  // Get registers
  struct user_regs_struct regs;
  ptrace(PTRACE_GETREGS, pid, NULL, &regs);
  
  // Build final buffer with header
  size_t header_size = sizeof(StateDataHeader);
  size_t regions_size = regions.size() * sizeof(RegionInfo);
  size_t tstate_size = tstate_data.size();
  size_t maps_size = maps_data.size();
  size_t data_size = 0;
  for (auto& rd : region_data) {
    data_size += rd.size();
  }
  
  size_t total_size = header_size + regions_size + tstate_size + maps_size + data_size + sizeof(regs);
  std::vector<uint8_t> all_data(total_size);
  
  // Fill header
  StateDataHeader* header = (StateDataHeader*)all_data.data();
  header->magic = 0x4453544D;  // 'DSTM'
  header->version = 3;          // Version 3 with integrated mappings
  header->num_regions = regions.size();
  header->tstate_offset = header_size + regions_size;
  header->tstate_size = tstate_size;
  header->maps_offset = header_size + regions_size + tstate_size;
  header->maps_size = maps_size;
  header->regs_offset = header_size + regions_size + tstate_size + maps_size + data_size;
  
  // Fill region info
  RegionInfo* region_infos = (RegionInfo*)(all_data.data() + header_size);
  size_t current_offset = header_size + regions_size + tstate_size + maps_size;
  for (size_t i = 0; i < regions.size(); i++) {
    region_infos[i].start = regions[i].start;
    region_infos[i].end = regions[i].end;
    region_infos[i].offset = current_offset;
    
    // Copy region data
    memcpy(all_data.data() + current_offset, region_data[i].data(), region_data[i].size());
    current_offset += region_data[i].size();
  }
  
  // Copy tstate data
  memcpy(all_data.data() + header->tstate_offset, tstate_data.data(), tstate_size);
  
  // Copy maps data
  if (maps_size > 0) {
    memcpy(all_data.data() + header->maps_offset, maps_data.data(), maps_size);
    LOG_TRACE("Copied maps data: offset=%lu, size=%zu", header->maps_offset, maps_size);
  }
  
  // Copy registers
  memcpy(all_data.data() + header->regs_offset, &regs, sizeof(regs));

  // Store via StateStore - returns immediately with hash
  // Background thread handles compression and disk write
  ts_hash = StateStore::instance().save(all_data.data(), all_data.size());
  
  LOG_DEBUG("StateStore saved hash %08x, size=%zu, regions=%zu, tstate=%zu, maps=%zu", 
            ts_hash, all_data.size(), regions.size(), tstate_size, maps_size);
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
  LOG_TRACE("tracee_state constructor: which=%d, pid=%d", which, pid);

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
  // Mappings are now integrated into the main state data by capture_memory_state()
  // This function is kept for API compatibility but does nothing
  LOG_TRACE("Mappings already integrated into state data for hash %08x", ts_hash);
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

  /* Capture memory state (includes tstate) */
  capture_memory_state();

  /* Save mappings via StateStore */
  save_mappings();

  /* Serialize structure - now integrated into capture_memory_state */
  serialize_to_disk();

  /* Save files */
  save_proc_files();
}

/* ======================================================================
 * Section 9: Memory Operations
 * ====================================================================== */

/* Read memory from snapshot by hash using StateStore */
void *read_mem(hash_type ts_hash, int pid, uint64_t addr, long size)
{
  bytes *ret = (bytes *)malloc(size + 1);
  ret[size] = 0;

  // Load state data from StateStore
  std::vector<uint8_t> data;
  ssize_t data_size = StateStore::instance().load(ts_hash, data);
  
  if (data_size < 0) {
    LOG_CRIT("Cannot load state for hash %08x in read_mem", ts_hash);
    // Fallback: try reading from live process
    tracee_read_mem(pid, (void *)addr, ret, size);
    return ret;
  }
  
  // Parse header
  if (data.size() < sizeof(StateDataHeader)) {
    LOG_CRIT("Invalid state data for hash %08x", ts_hash);
    tracee_read_mem(pid, (void *)addr, ret, size);
    return ret;
  }
  
  StateDataHeader* header = (StateDataHeader*)data.data();
  if (header->magic != 0x4453544D) {
    LOG_CRIT("Invalid state data format for hash %08x", ts_hash);
    tracee_read_mem(pid, (void *)addr, ret, size);
    return ret;
  }
  
  RegionInfo* regions = (RegionInfo*)(data.data() + sizeof(StateDataHeader));
  
  // Find region containing addr
  for (uint32_t i = 0; i < header->num_regions; i++) {
    if (regions[i].start <= addr && addr < regions[i].end) {
      uint64_t offset_in_region = addr - regions[i].start;
      uint64_t data_offset = regions[i].offset + offset_in_region;
      size_t bytes_available = regions[i].end - addr;
      size_t bytes_to_copy = std::min((size_t)size, bytes_available);
      
      if (data_offset + bytes_to_copy <= data.size()) {
        memcpy(ret, data.data() + data_offset, bytes_to_copy);
        LOG_TRACE("read addr %p from state data (region %u)", (void *)addr, i);
      } else {
        LOG_ERROR("Data offset out of bounds for addr %p", (void *)addr);
      }
      return ret;
    }
  }
  
  // Region not found in saved state, try live process
  LOG_TRACE("addr %p not in saved state, reading from live process", (void *)addr);
  tracee_read_mem(pid, (void *)addr, ret, size);
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
  while (fgets(line, sizeof(line), maps) != NULL)
  {
    memset(&item, 0, sizeof(item));
    /* Use width-limited format specifiers to prevent buffer overflow */
    if (sscanf(line, "%lx-%lx %4s %x %d:%d %d %511s", &item.start, &item.end,
           item.flags, &item.offset, &item.a, &item.b, &item.inode, item.name) >= 7) {
      items.emplace_back(item);
    }
  }
}

static bool maps_item_eq(maps_item &a, maps_item &b)
{
  return a.start == b.start && a.end == b.end;
}

bool mapping_exists(maps_item &a, std::vector<maps_item> &array)
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
 * Section 12: Statistics
 * ====================================================================== */

void show_state_stats()
{
  printf("State set size: %zu\n", state_set.size());
  printf("Queue size:     %zu\n", state_queue.size());
}

/* ======================================================================
 * Section 11: sys_state Recovery
 * ====================================================================== */

void sys_state::recover_shared_files()
{
  // TODO
}

void sys_state::recover_running_state()
{
  for (int i = 0; i < NP; i++) {
    if (!exited[i]) {
      child[i].recover_running_state(i);
    }
  }
  recover_shared_files();
}
