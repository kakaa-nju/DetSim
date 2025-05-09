#include "common.h"
#include "guest.h"
#include "state.h"
#include "debug.h"
#include "monitor.h"
#include "utils.h"
#include <assert.h>
#include <bits/types/cookie_io_functions_t.h>
#include "cereal/types/list.hpp"
#include "cereal/types/unordered_map.hpp"
#include <cereal/archives/binary.hpp>
#include <ctime>
#include <spdlog/spdlog.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <fstream>
#include <stack>
#include <libgen.h>

PTMC_STATE ptmc_state;
TSS state_tree;
LSS state_queue;
SSS state_set;

struct user_regs_struct regs_on_entry;
struct user_regs_struct regs;
void fcopy(char *source_filename, char *destination_filename);

std::map<int, void *> rseq_struct;
std::map<int, int> rseq_len;

/* serialized process state don't include memory and mappings 
 * memory will be recorded when dumping,
 * and mappings will be directly copied from procfs (?)
 */
char printbuf[1024];

static void construct_syscall_string(hash_type ts_hash);
static void construct_syscall_string(tracee_state *t);

void log_syscall(tracee_state *t) {
  construct_syscall_string(t);
  LOG_TRACE("%s", printbuf);
}

/* only one tracee_state changed comparing to source_state */
sys_state::sys_state(struct syscall_info *info) {
  /* construct all tracee for init_state */

  // int index = ptmc_state.cursor;
  // if (index == -1)
  // {
    for (int j = 0; j < NP; j++)
    {
      child[j] = tracee_state(j, &info[j]);
      child[j].save_proc_full_data();
      exited[j] = ptmc_state.exited[j];
      ts_hash[j] = child[j].ts_hash;
      LOG_DEBUG("Create tstate " HASH_FORMAT, ts_hash[j]);
    }
  // }
  // else 
  // {
  //   /* cursor */
  //   child[index] = new tracee_state(index, &info[index]);
  //   child[index]->save_proc_full_data();
  //   log_syscall(child[index]);
  //   exited[index] = ptmc_state.exited[index];
  //   ts_hash[index] = child[index]->ts_hash;
  //   LOG_DEBUG("Create tstate " HASH_FORMAT, ts_hash[index]);

  //   /* not cursor: inherit */
  //   for (int j = 0; j < NP; j++)
  //   {
  //     if (j == index) continue;
  //     child[j] = ptmc_state.source_state.child[j];
  //     exited[j] = ptmc_state.source_state.exited[j];
  //     ts_hash[j] = ptmc_state.source_state.ts_hash[j];
  //   }
  // }

  /* calc md5 xor */
  hash_type ss_hash_tmp = 0ULL;
  for (int j = 0; j < NP; j++) 
  {
    ss_hash_tmp ^= ts_hash[j];
  }

  while (true)
  {
    if (!state_set.count(ss_hash_tmp)) /* hash doesn't exist */
      break; /* to use it */

    sys_state s(ss_hash_tmp);
    int i = 0;
    for (i = 0; i < NP; i++)
    {
      if (s.ts_hash[i] != ts_hash[i])
        break;
    }
    if (i == NP) /* all the same */
      goto ret;
    /* it's different */
    ss_hash_tmp++;
    continue;
  }

ret:
  ss_hash = ss_hash_tmp;
  LOG_DEBUG("Create sstate " HASH_FORMAT, ss_hash_tmp);
}

void state_queue_append(sys_state *s) {
  state_queue.push_back(s->ss_hash);
}

void state_queue_append_front(sys_state *s) {
  state_queue.push_front(s->ss_hash);
}

sys_state *state_queue_extract() {
  if (state_queue.empty())
    return NULL;
  sys_state *s = new sys_state(state_queue.front());
  state_queue.pop_front();
  return s;
}

void sys_state::save_shared_files() {
  for (auto &shared_file: ptmc_state.shared_files) 
  {
    char dump_filename[128];
    sprintf(dump_filename, "filesystem/" HASH_FORMAT ".sfs/%s", ss_hash, shared_file.c_str());
    fcopy((char *)shared_file.c_str(), dump_filename);
  }
}

int sys_state::save_metadata() {
  char filename[64];
  sprintf(filename, "sstate/" HASH_FORMAT ".ss", ss_hash);
  LOG_DEBUG("to store sstate " HASH_FORMAT, ss_hash);
  if (access(filename, R_OK) != 0)
  {
    LOG_DEBUG("sys_state stores " HASH_FORMAT " traceeState " HASH_FORMAT " and something else ...", ss_hash, ts_hash[0]);
    std::ofstream ofs(filename, std::ios::binary);
    cereal::BinaryOutputArchive outputArchive(ofs);
    outputArchive(*this);
    ofs.close();
    save_shared_files();
    return 1;
  }

  return 0;
}

void state_tree_add(sys_state *s, sys_state *t, int which, int choose) {
  if (state_tree.count(t->ss_hash) == 0)
    state_tree[t->ss_hash] = std::tuple<hash_type, int, int>(s->ss_hash, which, choose);
}

void show_syscall_history() {
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
    construct_syscall_string(s.ts_hash[which]);
    print_stack.push(std::string(printbuf) + "\n");
    if (choose >= 0)
      sprintf(printbuf, HASH_FORMAT " => " HASH_FORMAT "\n Tracee %d (choose %d): ",
          pre, ss, which, choose);
    else 
      sprintf(printbuf, HASH_FORMAT " => " HASH_FORMAT "\n Tracee %d: ",
          pre, ss, which);
    print_stack.push(printbuf);
    ss = pre;
  }

  while (!print_stack.empty())
  {
    printf("%s", print_stack.top().c_str());
    print_stack.pop();
  }
  printf("%d steps in total.\n", cnt);
}


static bytes *membuf;
static int membufsz = 4096;

#define BUFFER_SIZE 4096
/* NO CONSIDER ABOUT PERFORMANCE */
static bool maps_item_eq(maps_item &a, maps_item &b) {
  return a.start == b.start && a.end == b.end;
}

static bool existmaps_item(maps_item &a, std::vector<maps_item> array) {
  /* which will be managed by SYS_brk */
  if (!strcmp(a.name, "[heap]"))
    return true;

  for (auto &item: array)
  {
    if (maps_item_eq(a, item))
      return true;
  }
  return false;
}

/* NOTICE: addr may point read only memory, in which *
 * case has no dump, and need to be read from process */
void *read_mem(hash_type ts_hash, int pid, uint64_t addr, long size) {
  bytes *ret = (bytes *)malloc(size + 1);
  ret[size] = 0;
  char filename[64];
  sprintf(filename, "mappings/" HASH_FORMAT ".maps", ts_hash);
  FILE *maps = fopen(filename, "r");
  assert(maps);
  std::vector<maps_item> items;
  get_maps_item(items, maps);
  fclose(maps);

  sprintf(filename, "memory/" HASH_FORMAT ".mem.zstd", ts_hash);
  int footprint = rand();
  char tmpfile[64];
  sprintf(tmpfile, "/tmp/dump.%08x", footprint);
  decompress_file(filename, tmpfile);

  FILE *mem = fopen(tmpfile, "r");
  assert(mem);
  for (auto &item: items)
  {
    if (item.start > addr || addr >= item.end) 
    {
      if (item.flags[1] == 'w' && item.start != available_memory)
        fseek(mem, item.end - item.start, SEEK_CUR);
      continue;
    }
    /* or else: within a mapped area */
    if (item.flags[1] == 'w' && item.start != available_memory) /* with dump */
    {
      fseek(mem, addr - item.start, SEEK_CUR);
      int offset = ftell(mem);
      assert(fread(ret, 1, size, mem) == size);
      LOG_TRACE("read addr %p from %s at offset %d", (void *)addr, filename, offset);
    }
    else /* no dump */
    {
      char proc_mem_file[64];
      sprintf(proc_mem_file, "/proc/%d/mem", pid);
      FILE *proc_mem = fopen(proc_mem_file, "r");
      assert(proc_mem);
      fseek(proc_mem, addr, SEEK_SET);
      assert(fread(ret, 1, size, proc_mem) == size);
      fclose(proc_mem);
      LOG_TRACE("read addr %p from procfs", (void *)addr);
    }
    break;
  }
  fclose(mem);
  unlink(tmpfile);
  return ret;
}

void *tracee_state::read_snapshot_mem(uint64_t addr, long size) {
  return read_mem(ts_hash, pid, addr, size);
}

std::vector<maps_item> tracee_state::recover_brk_mappings() {
  char filename[64];
  std::vector<maps_item> maps_old, maps_new;
  {
    sprintf(filename, "/proc/%d/maps", pid);
    FILE *current_maps = fopen(filename, "r");
    assert(current_maps);
    sprintf(filename, "mappings/" HASH_FORMAT ".maps", ts_hash);
    LOG_DEBUG("open mapping file %s", filename);
    FILE *origin_maps = fopen(filename, "r");
    assert(origin_maps);

    /* diff */
    get_maps_item(maps_old, current_maps);
    get_maps_item(maps_new, origin_maps);
    fclose(current_maps);
    fclose(origin_maps);
    for (auto &item_old: maps_old)
      if (!existmaps_item(item_old, maps_new)) 
      {
        tracee_do_munmap(pid, item_old.start, item_old.end);
      }
    for (auto &item_new: maps_new)
      if (!existmaps_item(item_new, maps_old)) 
      {
        tracee_do_mmap(pid, item_new.start, item_new.end);
      }
  }
  tracee_do_syscall(pid, SYS_brk, brk, 0, 0, 0, 0, 0);
  LOG_DEBUG("recovered brk = 0x%x", brk);
  return maps_new;
}

void tracee_state::recover_file_descriptors(int index) {
  sys_state &last = ptmc_state.dest_state;
  tracee_state &old_state = last.child[index];

  // close all
  for (auto &fd : old_state.fd_list) 
  {
    LOG_TRACE("close fd %d", fd.fd);
    tracee_do_syscall(pid, SYS_close, fd.fd, 0, 0, 0, 0, 0);
  }

  // open all
  for (auto &fd : fd_list)
  {
    LOG_TRACE("restore fd, name = %d, %s", fd.fd, fd.fname);
    int dfd = tracee_do_open(pid, fd.fname.c_str(), fd.flags);
    if (dfd != fd.fd) 
    {
      tracee_do_syscall(pid, SYS_dup2, dfd, fd.fd, 0, 0, 0, 0);
      tracee_do_syscall(pid, SYS_close, dfd, 0, 0, 0, 0, 0);
    }
    tracee_do_syscall(pid, SYS_lseek, fd.fd, fd.pos, SEEK_SET, 0, 0, 0);
  }
}

void tracee_state::recover_mem_reg_snapshot(std::vector<maps_item> &maps) {
  char filename[64];

  sprintf(filename, "memory/" HASH_FORMAT ".mem.zstd", ts_hash);
  LOG_TRACE("restore from %s", filename);
  
  int footprint = rand();
  char tmpfile[64];
  sprintf(tmpfile, "/tmp/dump.%08x", footprint);
  decompress_file(filename, tmpfile);

  FILE *dump = fopen(tmpfile, "r");
  assert(dump);

  /* read memory content */
  sprintf(filename, "/proc/%d/mem", pid);
  FILE *mem = fopen(filename, "r+");
  assert(mem);

  /* prepare rseq */
  void *rseq = malloc(rseq_len[pid]);
  fseek(mem, (uintptr_t)rseq_struct[pid], SEEK_SET);
  fread(rseq, rseq_len[pid], 1, mem);

  /* recover mem */
  for (auto &item: maps)
  {
    if (item.flags[1] != 'w') continue;
    if (item.start == available_memory) continue;
    ssize_t size = item.end - item.start;

    int offset = ftell(dump);
    LOG_TRACE("Restore mem %p-%p %d pages from offset %d",
        (void *)item.start, (void *)item.end, size / PAGE_SIZE, offset);

    if (size > membufsz) 
    {
      free(membuf);
      membufsz = size;
      membuf = (bytes *)Lmalloc(membufsz);
    }
    assert(fread(membuf, 1, size, dump) == size); 
    fseek(mem, item.start, SEEK_SET);
    assert(fwrite(membuf, 1, size, mem) == size);
  }

  /* rseq */
  fseek(mem, (uintptr_t)rseq_struct[pid], SEEK_SET);
  fwrite(rseq, rseq_len[pid], 1, mem);
  free(rseq);

  /* restore registers */
  assert(sizeof(regs) == fread(&regs, 1, sizeof(regs), dump));
  ptrace(PTRACE_SETREGS, pid, NULL, &regs);
  show_regs(&regs);
  fclose(mem);
  fclose(dump);

  unlink(tmpfile);
}

void tracee_state::recover_proc_files() {
  // TODO
}

void sys_state::recover_shared_files() {
  for (auto &shared_file: ptmc_state.shared_files) 
  {
    char dump_filename[128];
    sprintf(dump_filename, "filesystem/" HASH_FORMAT ".sfs/%s", ss_hash, shared_file.c_str());
    fcopy(dump_filename, (char *)shared_file.c_str());
  }
}

/* Process[index] <- tracee_state s */
/* Need index for configure which tracee in last state */
void tracee_state::recover_running_state(int index) {
  auto maps = recover_brk_mappings();

  /* Goes before file descriptors */
  recover_proc_files();

  /* Goes before snapshot, *
   * for %RIP will change in recover_proc_files(). */
  recover_file_descriptors(index);

  recover_mem_reg_snapshot(maps);

  /* 3. restore msg and socket */ 
  ptmc_state.sock_lists[index] = sock_list;
  ptmc_state.tcp_buffer_lists[index] = tcp_buffer_list;
  ptmc_state.udp_buffer_lists[index] = udp_buffer_list;

  /* 4. time */
  ptmc_state.time[index] = tv;
}

sys_state::sys_state(hash_type hash) {
  ss_hash = hash;
  char filename[64];
  sprintf(filename, "sstate/" HASH_FORMAT ".ss", hash);

  std::ifstream ifs(filename, std::ios::binary);
  assert(ifs.is_open());

  cereal::BinaryInputArchive inputArchive(ifs);
  inputArchive(*this);
  ifs.close();
}

void sys_state::recover_running_state() {
  recover_shared_files();
  for (int i = 0; i < NP; i++) 
  {
    child[i] = tracee_state(ts_hash[i]);

    if (exited[i] == 1) /* exited process need no running state */
      continue;

    // if ((ptmc_state.dest_state).ts_hash[i] != ts_hash[i])
    child[i].recover_running_state(i);
    /* to avoid probable fs bugs */
  }
}

tracee_state::tracee_state(hash_type hash) {
  ts_hash = hash;
  LOG_DEBUG("restore tracee_state = " HASH_FORMAT, hash);
  
  char filename[64];
  sprintf(filename, "tstate/" HASH_FORMAT ".ts", hash);

  std::ifstream ifs(filename, std::ios::binary);
  if (!ifs.is_open())
  {
    LOG_CRIT("Open traceeState file failed");
    exit(errno);
  }
  cereal::BinaryInputArchive inputArchive(ifs);
  inputArchive(*this);
  ifs.close();

  LOG_DEBUG("Deserialize: %d buffers", udp_buffer_list.size());
  for (auto &b: udp_buffer_list) {
    LOG_DEBUG("Deserialize: %d messages", b.second.size());
  }
}

uint64_t crc32(FILE *fp);
void tracee_state::create_mem_reg_snapshot() {
  char line[1024];
  /* save memory image to file, and calculate process state hash */

  /* create new temporary dump file */
  int footprint = rand();
  char tmpfile[64];
  sprintf(tmpfile, "/tmp/dump.%08x", footprint);
  FILE *dump = fopen(tmpfile, "w+");
  assert(dump);

  /* memory mappings */
  char filename[64];
  sprintf(filename, "/proc/%d/maps", pid);
  FILE *maps = fopen(filename, "r");
  assert(maps);

  /* memory file */
  sprintf(filename, "/proc/%d/mem", pid);
  FILE *mem = fopen(filename, "r");
  assert(mem);

  /* dump memory */
  uint64_t start, end;
  char flags[5];
  uint32_t offset;
  int a, b, inode;
  char name[51];
  while (fgets(line, 1024, maps) != NULL) 
  {
    sscanf(line, "%lx-%lx %s %x %d:%d %d %s", &start, &end, flags, &offset, &a,
           &b, &inode, name);
    if (flags[1] != 'w') continue;
    if (start == available_memory) continue;
   
    if (end - start > membufsz) 
    {
      free(membuf);
      membufsz = end - start;
      membuf = (bytes *)Lmalloc(membufsz);
    }
    fseek(mem, start, SEEK_SET);
    fread(membuf, end - start, 1, mem);

    /* mask rseq */
    if ((uintptr_t)rseq_struct[pid] >= start && (uintptr_t)rseq_struct[pid] < end)
    {
      uintptr_t offset = (uintptr_t)rseq_struct[pid] - start;
      memset(membuf + offset, 0x55, rseq_len[pid]);
    }

    fwrite(membuf, end - start, 1, dump);
    LOG_TRACE("dump %lx-%lx, %s", start, end, name);
  }

  /* dump registers */
  ptrace(PTRACE_GETREGS, pid, NULL, &regs);
  fwrite(&regs, sizeof(regs), 1, dump);

  fclose(maps);
  fclose(mem);
  save_structure_data(dump);

  fclose(dump);

  /* compress first, then calculate hash */
  sprintf(filename, "/tmp/%08x.mem.zstd", footprint);
  hash_type hash = compress_file(tmpfile, filename, 1);
  
  char dest_filename[64];
  while(true) 
  {
    sprintf(dest_filename, "memory/" HASH_FORMAT ".mem.zstd", hash);
    if (access(dest_filename, R_OK)) /* file doesn't exist */
      break; /* to create it */

    if (!filecmp(dest_filename, filename))/* it's all the same */
      goto ret;
    /* it's different */
    hash++;
    continue;
  }
  fcopy(filename, dest_filename);

ret:
  unlink(filename);
  unlink(tmpfile);
  ts_hash = hash;
}

void tracee_state::get_file_descriptors() {
  char link[256];
  char fdinfo_dir[128];
  char fdfile[128]; /* symbolic link */
  sprintf(fdinfo_dir, "/proc/%d/fdinfo", pid);

  int fd_fdinfo = open(fdinfo_dir, O_RDONLY | O_DIRECTORY);
  DIR *fdinfo = fdopendir(fd_fdinfo);
  assert(fdinfo);

  struct dirent *de;
  while ((de = readdir(fdinfo)) != NULL) 
  {
    if (de->d_name[0] == '.')
      continue;

    ptmc_filedesc new_fd;
    /* get opened file name */
    sprintf(fdfile, "/proc/%d/fd/%s", pid, de->d_name);

    /* NOTE: `readlink` will NOT set a nullbyte after filename */
    int linkLen = readlink(fdfile, link, 256);
    link[linkLen] = 0;
    sscanf(de->d_name, "%d", &new_fd.fd);

    new_fd.fname = std::string(link, link + linkLen);

    int fd = openat(fd_fdinfo, de->d_name, O_RDONLY);
    assert(fd >= 0);
    FILE *fp = fdopen(fd, "r");
    assert(fp);

    fscanf(fp, "pos: %d flags: %o mnt_id: %d ino: %d", &new_fd.pos,
           &new_fd.flags, &new_fd.mnt_id, &new_fd.ino);
    fclose(fp);

    fd_list.emplace_back(new_fd);
  }
  closedir(fdinfo);
}

/* from system process to data structure *
 * At this moment, nothing should be written into disk. *
 * And because MD5 is not calculated, ts_hash is not set. */
tracee_state::tracee_state(int which, struct syscall_info *info) {
  /* 1. save syscallinfo to struct 
   * here info is on stack. copy it */
  si = *info;
  pid = ptmc_state.pids[which];

  /* 2. save fds */
  get_file_descriptors();

  /* 3. sockets in state are maintained while executing */
  sock_list = ptmc_state.sock_lists[which];
  tcp_buffer_list = ptmc_state.tcp_buffer_lists[which];
  udp_buffer_list = ptmc_state.udp_buffer_lists[which];

  /* 4. time */
  tv = ptmc_state.time[which];
}

void tracee_state::save_structure_data(FILE *fp) {
  std::stringstream ofs;
  // if (!ofs.is_open())
  //   LOG_CRIT("failed to open tstate file for ts_hash = %s", ts_hash);
  cereal::BinaryOutputArchive outputArchive(ofs);
  outputArchive(*this);
  fwrite(ofs.str().c_str(), ofs.str().length(), 1, fp);
}

void tracee_state::save_structure_data() {
  char filename[64];
  sprintf(filename, "tstate/" HASH_FORMAT ".ts", ts_hash);
  std::ofstream ofs(filename, std::ios::binary);
  if (!ofs.is_open())
    LOG_CRIT("failed to open tstate file for ts_hash = %s", ts_hash);
  cereal::BinaryOutputArchive outputArchive(ofs);
  outputArchive(*this);
  ofs.close();
}

void tracee_state::save_brk_mappings() {
  char src[256];
  char dest[256];

  sprintf(src, "/proc/%d/maps", pid);
  sprintf(dest, "mappings/" HASH_FORMAT ".maps", ts_hash);

  fcopy(src, dest);

  brk = tracee_do_syscall(pid, SYS_brk, 0, 0, 0, 0, 0, 0);
  LOG_DEBUG("recorded brk = 0x%x", brk);
}

/* Dump open files to filesystem/#state.fs *
 * Well, actually... Open files may not be enough for *
 * processes. Should specify all needed files in config */
void tracee_state::save_proc_files() {
  // TODO();
}

void tracee_state::save_proc_full_data() {
  /* set ts_hash here */
  create_mem_reg_snapshot();

  /* save mappings */
  save_brk_mappings();

  /* serialize */
  save_structure_data();

  /* save files */
  save_proc_files();
}


__attribute__((constructor)) void init_membuf() {
  membuf = (bytes *)Lmalloc(membufsz);
}

int format_sockaddr_in(char* dest, struct sockaddr_in* addr) {
    char ipbuf[INET_ADDRSTRLEN];
    const char* ipstr = inet_ntop(AF_INET, &(addr->sin_addr), ipbuf, sizeof(ipbuf));
    if (!ipstr) ipstr = "invalid";

    return sprintf(dest, "{sin_family=AF_INET, sin_port=htons(%d), sin_addr=inet_addr(\"%s\")}",
             ntohs(addr->sin_port), ipstr);
}

int construct_mem2str(char *str, char *mem, int len) {
  int pos = 0;
  for (int i = 0; i < len; i++) 
  {
    uint8_t val = mem[i];
    if (isprint(mem[i]))
      pos += sprintf(str + pos, "%c", mem[i]);
    else 
      pos += sprintf(str + pos, "\\%03u", val);
  }
  return pos;
}

static void construct_sys_d_s_d_d(char *str, tracee_state *t, syscall_info *info) {
  char *mem = (char *)t->read_snapshot_mem(info->args[1], info->args[2]);
  assert(mem);
  str += sprintf(str, "%s(%ld, %p(\"", syscalls[info->nr], info->args[0], (void *)info->args[1]);
  str += construct_mem2str(str, mem, info->args[2]);
  str += sprintf(str, "\"), %ld) = %ld", info->args[2], info->rval);
  free(mem);
}

static void construct_sys_sendto(char *str, tracee_state *t, syscall_info *info) {
  char *mem = (char *)t->read_snapshot_mem(info->args[1], info->args[2]);
  assert(mem);
  str += sprintf(str, "%s(%ld, \"", syscalls[info->nr], info->args[0]);
  str += construct_mem2str(str, mem, info->args[2]);
  str += sprintf(str, "\", %ld, %ld, ", info->args[2], info->args[3]);
  free(mem);

  struct sockaddr_in* addr = (struct sockaddr_in*)t->read_snapshot_mem(info->args[4], info->args[5]);
  str += format_sockaddr_in(str, addr);
  free(addr);

  str += sprintf(str, ", %ld) = %ld", info->args[5], info->rval);
}

static void construct_sys_recvfrom(char *str, tracee_state *t, syscall_info *info) {
  char *mem = (char *)t->read_snapshot_mem(info->args[1], info->args[2]);
  assert(mem);
  str += sprintf(str, "%s(%ld, \"", syscalls[info->nr], info->args[0]);
  str += construct_mem2str(str, mem, info->rval);
  str += sprintf(str, "\", %ld, %ld, ", info->args[2], info->args[3]);
  free(mem);

  socklen_t* plen = (socklen_t*)t->read_snapshot_mem(info->args[5], sizeof(socklen_t));
  socklen_t addrlen = *plen;

  struct sockaddr_in* addr = (struct sockaddr_in*)t->read_snapshot_mem(info->args[4], addrlen);
  str += format_sockaddr_in(str, addr);
  free(addr);

  str += sprintf(str, ", [%d]) = %ld", addrlen, info->rval);
}

static void construct_sys_clock_nanosleep(char *str, tracee_state *t, syscall_info *info) {
  struct timespec *mem = (struct timespec *)t->read_snapshot_mem(info->args[2], sizeof(struct timespec));
  assert(mem);
  str += sprintf(str, "%s(%ld nanosec) = %ld", syscalls[info->nr], mem->tv_sec * 1000000000 + mem->tv_nsec, info->rval);
  free(mem);
}

static void construct_sys_brk(char *str, syscall_info *info) {
  str += sprintf(str, "brk(0x%lx) = 0x%lx", info->args[0], info->rval);
}

static void construct_sys_exit(char *str, syscall_info *info) {
  str += sprintf(str, "%s(%ld) = ?", syscalls[info->nr], info->args[0]);
}

static void construct_sys_default(char *str, syscall_info *info) {
  str += sprintf(str, "%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) = %ld", syscalls[info->nr],
      info->args[0], info->args[1], info->args[2], 
      info->args[3], info->args[4], info->args[5], info->rval); 
}

static void construct_sys_d_d(char *str, syscall_info *info) {
  str += sprintf(str, "%s(%ld) = %ld", syscalls[info->nr], info->args[0], info->rval);
}

static void construct_sys_gettimeofday(char *str, tracee_state *t, syscall_info *info) {
  struct timeval *tv = (struct timeval *)t->read_snapshot_mem(info->args[0], sizeof(struct timeval));
  assert(tv);
  str += sprintf(str, "%s((struct timeval *){ .sec = %ld, .usec = %ld }, 0x%lx) = %ld", 
      syscalls[info->nr], tv->tv_sec, tv->tv_usec, info->args[1], info->rval);
  free(tv);
}


static void construct_syscall_string(tracee_state *t) {
  auto info = &t->si;
  switch (info->nr) 
  {
    case SYS_sendto:
      construct_sys_sendto(printbuf, t, info); break;
    case SYS_recvfrom:
      construct_sys_recvfrom(printbuf, t, info); break;
    case SYS_clock_nanosleep:
      construct_sys_clock_nanosleep(printbuf, t, info); break;
    case SYS_brk:
      construct_sys_brk(printbuf, info); break;

    case SYS_write:
    case SYS_read:
      construct_sys_d_s_d_d(printbuf, t, info); break;

    case SYS_exit_group:
    case SYS_exit:
      construct_sys_exit(printbuf, info); break;

    case SYS_sched_yield:
    case SYS_close:
      construct_sys_d_d(printbuf, info); break;

    case SYS_gettimeofday:
      construct_sys_gettimeofday(printbuf, t, info); break;
      /*
    case SYS_open:
      contruct_sys_open(printbuf, t, info); break;
      */

    default:
      construct_sys_default(printbuf, info); break;
  }
}

static void construct_syscall_string(hash_type ts_hash) {
  tracee_state ts(ts_hash);
  construct_syscall_string(&ts);
}

void tracee_state::show_syscall(syscall_info *info) {
  construct_syscall_string(this);
  printf("%s\n", printbuf);
}
