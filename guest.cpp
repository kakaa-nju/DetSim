/* guest.cpp: interacting with tracee directly */

#include "common.h"
#include "emu.h"
#include "guest.h"
#include "debug.h"
#include "monitor.h"
#include <cstdint>
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/reg.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <libdwarf/libdwarf.h>
#include <dwarf.h>

/* ptrace with error check */
// Call this function to get a backtrace.
__attribute__((unused)) void print_call_stack() {
  unw_cursor_t cursor;
  unw_context_t context;

  // Initialize cursor to current frame for local unwinding.
  unw_getcontext(&context);
  unw_init_local(&cursor, &context);

  // Unwind frames one by one, going up the frame stack.
  while (unw_step(&cursor) > 0) 
  {
    unw_word_t offset, pc;
    unw_get_reg(&cursor, UNW_REG_IP, &pc);
    if (pc == 0) 
      break;
    printf("0x%lx:", pc);

    char sym[256];
    if (unw_get_proc_name(&cursor, sym, sizeof(sym), &offset) == 0) 
      printf(" (%s+0x%lx)\n", sym, offset);
    else 
      printf(" -- error: unable to obtain symbol name for this frame\n");
  }
}

#define def_tracee_set(reg)                                                    \
  void tracee_set_##reg(int pid, uint64_t val) {                               \
    struct user_regs_struct uregs;                                             \
    ptrace_right(PTRACE_GETREGS, pid, NULL, &uregs);                                 \
    uregs.reg = val;                                                           \
    ptrace_right(PTRACE_SETREGS, pid, NULL, &uregs);                                 \
  }
def_tracee_set(rax);
def_tracee_set(orig_rax);

#define def_tracee_get(reg)                                                    \
  uint64_t tracee_get_##reg(int pid) {                                         \
    struct user_regs_struct uregs;                                             \
    ptrace_right(PTRACE_GETREGS, pid, NULL, &uregs);                                 \
    return uregs.reg;                                                          \
  }

__attribute__((unused)) def_tracee_get(rip);
def_tracee_get(rbp);
def_tracee_get(rsp);

#define AT_NULL 0
#define AT_IGNORE 1
#define AT_SYSINFO_EHDR 33

void remove_vdso(int pid) {
  size_t pos;
  int zeroCount;
  long val;

  pos = (size_t)ptrace(PTRACE_PEEKUSER, pid, sizeof(long) * RSP, NULL);

  /* skip to auxiliary vector */
  zeroCount = 0;
  while (zeroCount < 2) 
  {
    val = ptrace(PTRACE_PEEKDATA, pid, pos += 8, NULL);
    if (val == 0)
      zeroCount++;
  }

  /* search the auxiliary vector for AT_SYSINFO_EHDR... */
  val = ptrace(PTRACE_PEEKDATA, pid, pos += 8, NULL);
  while (1) 
  {
    if (val == AT_NULL)
      break;
    if (val == AT_SYSINFO_EHDR) 
    {
      /* ... and overwrite it */
      ptrace(PTRACE_POKEDATA, pid, pos, AT_IGNORE);
      break;
    }
    val = ptrace(PTRACE_PEEKDATA, pid, pos += 16, NULL);
  }
}

void tracee_write_mem(int pid, void *addr, const void *data, int len) {
  char filename[32];
  sprintf(filename, "/proc/%d/mem", pid);
  int mem = open(filename, O_WRONLY);
  lseek(mem, (uintptr_t)addr, SEEK_SET);
  write(mem, data, len);
  close(mem);
}

__attribute__((unused)) void tracee_read_mem(int pid, const void *addr, void *data, int len) {
  char filename[32];
  sprintf(filename, "/proc/%d/mem", pid);
  int mem = open(filename, O_RDONLY);
  lseek(mem, (uintptr_t)addr, SEEK_SET);
  int size = read(mem, data, len);
  assert(size == len);
  close(mem);
}

__attribute__((unused)) uint8_t tracee_read_byte(int pid, void *addr) {
  uint8_t data[8];
  *((uint64_t *)data) = ptrace_right(PTRACE_PEEKDATA, pid, addr, NULL);
  return data[0];
}

uint64_t tracee_read_word(int pid, void *addr) {
  return ptrace_right(PTRACE_PEEKDATA, pid, addr, NULL);
}

int tracee_write_word(int pid, void *addr, long data) {
  return ptrace_right(PTRACE_POKEDATA, pid, addr, data);
}

void apply_choose(const syscall_info &info, choose_out *out) {
  for (int i = 0; i < 6; i++)
  {
    if (out->len[i])
      memcpy_host2guest((void *)info.args[i], out->args[i], out->len[i]);
  }
}

void tracee_switch_syscall(
    int pid, 
    int SYS_which, 
    uint64_t rdi,
    uint64_t rsi, 
    uint64_t rdx, 
    uint64_t r10,
    uint64_t r8, 
    uint64_t r9) {
  struct user_regs_struct syscall_regs;
  ptrace_right(PTRACE_GETREGS, pid, NULL, &syscall_regs);
  LOG_TRACE("rip syscall enter: %p", (void *)syscall_regs.rip);
  syscall_regs.orig_rax = SYS_which;
  syscall_regs.rdi = rdi;
  syscall_regs.rsi = rsi;
  syscall_regs.rdx = rdx;
  syscall_regs.r10 = r10;
  syscall_regs.r8 = r8;
  syscall_regs.r9 = r9;
  ptrace_right(PTRACE_SETREGS, pid, NULL, &syscall_regs);
}

void show_regs(struct user_regs_struct *regs) {
  LOG_TRACE("rax = %016llx rbx = %016llx rcx = %016llx rdx = %016llx\n"
      "rsp = %016llx rbp = %016llx rdi = %016llx rsi = %016llx\n"
      "r8  = %016llx r9  = %016llx r10 = %016llx rip = %016llx\n",
      regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsp, regs->rbp,
      regs->rdi, regs->rsi, regs->r8, regs->r9, regs->r10, regs->rip);
  LOG_TRACE("orig_rax = %016lx", regs->orig_rax);
}

#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)
uint64_t tracee_do_syscall(
    int pid, 
    int SYS_which, 
    uint64_t rdi,
    uint64_t rsi, 
    uint64_t rdx, 
    uint64_t r10,
    uint64_t r8, 
    uint64_t r9) {
  /* after last syscall exits */
  /* I don't care */
  /* scanf("%*c"); */

  /* printf("%d\n", pid); */
  struct user_regs_struct restore;
  struct user_regs_struct syscall_regs;
  
  ptrace_right(PTRACE_GETREGS, pid, NULL, &restore);
  syscall_regs = restore;

  syscall_regs.rip -= 2;
  LOG_TRACE("rip syscall enter: %p", (void *)syscall_regs.rip);
  syscall_regs.orig_rax = SYS_which;
  syscall_regs.rax = SYS_which;
  syscall_regs.rdi = rdi;
  syscall_regs.rsi = rsi;
  syscall_regs.rdx = rdx;
  syscall_regs.r10 = r10;
  syscall_regs.r8 = r8;
  syscall_regs.r9 = r9;
  ptrace_right(PTRACE_SETREGS, pid, NULL, &syscall_regs);

  int wstatus = 0;
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  if (WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) != PTRACE_TRAP_SIG)) 
  {
    LOG_INFO("ptrace_syscall has wrong stop status. WIFSTOPPED=%s and WSTOPSIG=%s.",
        WIFSTOPPED(wstatus) ? "true" : "false", strsignal(WSTOPSIG(wstatus)));
    
    ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
    waitpid(pid, &wstatus, 0);
    assert(WIFSTOPPED(wstatus) && (WSTOPSIG(wstatus) == PTRACE_TRAP_SIG));
  }

  /* before syscall enter */
  /* syscall modified already */
  LOG_TRACE("To do syscall:");
  show_regs(&syscall_regs);

  /* push syscall to exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  assert(WIFSTOPPED(wstatus) && WSTOPSIG(wstatus) == PTRACE_TRAP_SIG);

  ptrace_right(PTRACE_GETREGS, pid, NULL, &syscall_regs);
  LOG_TRACE("Syscall returns %p", (void *)syscall_regs.rax);
  LOG_TRACE("rip syscall return: %p", (void *)syscall_regs.rip);

  /* restore for another syscall */
  ptrace_right(PTRACE_SETREGS, pid, NULL, &restore);
  /* it may stop */

  return syscall_regs.rax;
}

void tracee_do_munmap(int pid, uint64_t start, uint64_t end) {
  tracee_do_syscall(pid, SYS_munmap, start, end - start, 0, 0, 0, 0);
}

void *tracee_do_mmap(int pid, uint64_t start, uint64_t end) {
  void *ret = (void *)tracee_do_syscall(
      pid, SYS_mmap, start, end - start, PROT_EXEC | PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  assert(ret != MAP_FAILED);
  return ret;
}

__attribute__((unused))
static bool addr_executable(uint64_t addr, const std::vector<maps_item> &items) {
  for (auto &item : items) 
  {
    if (item.start <= addr && addr < item.end) 
    {
      if (item.flags[2] == 'x')
        return true;
      else
        return false;
    }
  }
  return false;
}

__attribute__((unused))
static bool addr_on_stack(uint64_t addr, const std::vector<maps_item> &items) {
  for (auto &item : items) 
  {
    if (strcmp(item.name, "[stack]"))
      continue;
    if (addr >= item.start && addr < item.end)
      return true;
  }
  return false;
}

void get_maps_item(std::vector<maps_item>&items, FILE *maps) {
  maps_item item;
  char line[1024];
  while (fgets(line, 1024, maps) != NULL) 
  {
    sscanf(line, "%lx-%lx %s %x %d:%d %d %s", &item.start, &item.end,
           item.flags, &item.offset, &item.a, &item.b,
           &item.inode, item.name);
    items.emplace_back(item);
  }
}

int resolve_rip_func(const char *exefile, uintptr_t rip);

void tracee_backtrace(int pid) {
  uintptr_t rip = tracee_get_rip(pid);
  printf("#0: "); 
  resolve_rip_func(ptmc_state.tracee[ptmc_state.cursor].executable, rip);

  uintptr_t rbp = tracee_get_rbp(pid);
  int frame_index = 1;

  char maps_file[64];
  sprintf(maps_file, "/proc/%d/maps", pid);
  FILE *maps_fp = fopen(maps_file, "r");
  std::vector<maps_item> items;
  get_maps_item(items, maps_fp);
  fclose(maps_fp);

  while (rbp != 0 && frame_index < 64) 
  {
    uintptr_t ret_addr = tracee_read_word(pid, (void *)(rbp + 8));
    if (!addr_executable(ret_addr, items))
      break;
    printf("#%d: ", frame_index);
    resolve_rip_func(ptmc_state.tracee[ptmc_state.cursor].executable, ret_addr);

    uintptr_t next_rbp = tracee_read_word(pid, (void *)rbp);
    if (next_rbp == 0) 
    {
      LOG_TRACE("next rbp == 0");
      break;
    }
    if (next_rbp <= rbp)
    {
      LOG_TRACE("next rbp <= rbp (0x%lx, 0x%lx)", next_rbp, rbp);
      break;
    }
    if (!addr_on_stack(next_rbp, items)) 
    {
      LOG_TRACE("next_rbp 0x%lx not on stack", next_rbp);
      break;
    }

    rbp = next_rbp;
    frame_index++;
  }
}

const uintptr_t available_memory = 0x0000600000000000;

/* Only once for each process */
void tracee_get_freepage(int pid) {
  tracee_do_mmap(pid, available_memory, available_memory + PAGE_SIZE);
}

int tracee_do_open(int pid, const char *filename, uint64_t flags) {
  /* need tracee memory to place filename */
  /* mappings has been restored */
  /* LOG_TRACE("open filename, flags = %s, %o", filename, flags); */
  assert(available_memory);
  tracee_write_mem(pid, (void *)available_memory, filename, strlen(filename) + 1);
  return tracee_do_syscall(pid, SYS_open, (uint64_t)available_memory, flags, 0,
                           0, 0, 0);
}

extern struct Tracee tracee[NP];

struct var_info {
  std::string type_name;
  Dwarf_Addr address;
};

std::unordered_map<std::string, var_info> global_vars;


uintptr_t get_var_addr(const char *varname) {
  return global_vars[varname].address;
}

std::string get_var_type(const char *varname) {
  return global_vars[varname].type_name;
}

void *memcpy_guest2host(void *dest, const void *src, size_t n) {
  int pid = ptmc_state.pids[ptmc_state.cursor];
  tracee_read_mem(pid, src, dest, n);
  return dest;
}

void *memcpy_host2guest(void *dest, const void *src, size_t n) {
  int pid = ptmc_state.pids[ptmc_state.cursor];
  tracee_write_mem(pid, dest, src, n);
  return dest;
}

void tracee_show_regs(int pid) {
  struct user_regs_struct uregs;
  ptrace_right(PTRACE_GETREGS, pid, NULL, &uregs);
  show_regs(&uregs);
}

std::string get_type_name(Dwarf_Debug dbg, Dwarf_Die type_die) {
  Dwarf_Half tag;
  Dwarf_Error err;
  if (dwarf_tag(type_die, &tag, &err) != DW_DLV_OK) 
    return "unknown";

  char* type_name = nullptr;
  if (dwarf_diename(type_die, &type_name, &err) != DW_DLV_OK) 
    type_name = nullptr;

  if (tag == DW_TAG_base_type || tag == DW_TAG_structure_type 
      || tag == DW_TAG_union_type || tag == DW_TAG_enumeration_type) 
  {
    return type_name ? type_name : "anonymous";
  } 
  else if 
    (tag == DW_TAG_pointer_type) 
    {
    Dwarf_Attribute attr;
    if (dwarf_attr(type_die, DW_AT_type, &attr, &err) == DW_DLV_OK) 
    {
      Dwarf_Off offset;
      if (dwarf_global_formref(attr, &offset, &err) == DW_DLV_OK) 
      {
        Dwarf_Die next_die;
        if (dwarf_offdie(dbg, offset, &next_die, &err) == DW_DLV_OK) 
        {
          return get_type_name(dbg, next_die) + " *";
        }
      }
    }
    return "pointer to void";
  } 
  else if (tag == DW_TAG_const_type) 
  {
    Dwarf_Attribute attr;
    if (dwarf_attr(type_die, DW_AT_type, &attr, &err) == DW_DLV_OK) 
    {
      Dwarf_Off offset;
      if (dwarf_global_formref(attr, &offset, &err) == DW_DLV_OK) 
      {
        Dwarf_Die next_die;
        if (dwarf_offdie(dbg, offset, &next_die, &err) == DW_DLV_OK) 
        {
          return "const " + get_type_name(dbg, next_die);
        }
      }
    }
    return "const unknown";
  } else {
    return type_name ? type_name : "unknown";
  }
}

bool get_variable_address(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Addr& addr) {
  Dwarf_Error err;
  Dwarf_Attribute loc_attr;

  if (dwarf_attr(die, DW_AT_location, &loc_attr, &err) != DW_DLV_OK)
    return false;

  Dwarf_Unsigned expr_len;
  Dwarf_Ptr expr_bytes;

  if (dwarf_formexprloc(loc_attr, &expr_len, &expr_bytes, &err) != DW_DLV_OK)
    return false;

  const unsigned char* data = reinterpret_cast<const unsigned char*>(expr_bytes);

  if (expr_len > 0 && data[0] == DW_OP_addr)  
  {
    if (expr_len >= 1 + sizeof(Dwarf_Addr)) 
    {
      memcpy(&addr, data + 1, sizeof(Dwarf_Addr));
      return true;
    }
  }

  return false;
}

void init_dwarf() {
  int fd = open(ptmc_state.tracee[0].executable, O_RDONLY);
  if (fd < 0) 
  {
    panic("Failed open file");
  }

  Dwarf_Debug dbg = 0;
  Dwarf_Error err;
  if (dwarf_init(fd, DW_DLC_READ, nullptr, nullptr, &dbg, &err) != DW_DLV_OK) 
  {
    LOG_ERROR("Failed initializing libdwarf\n");
    close(fd);
  }

  Dwarf_Unsigned cu_header_length, abbrev_offset, next_cu_header;
  Dwarf_Half version_stamp, address_size;

  while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
        &abbrev_offset, &address_size, &next_cu_header, &err) == DW_DLV_OK) 
  {
    Dwarf_Die no_die = 0;
    Dwarf_Die cu_die;
    if (dwarf_siblingof(dbg, no_die, &cu_die, &err) == DW_DLV_OK) 
    {
      Dwarf_Die child_die;
      if (dwarf_child(cu_die, &child_die, &err) == DW_DLV_OK) 
      {
        Dwarf_Die current_die = child_die;
        do 
        {
          Dwarf_Half tag;
          if (dwarf_tag(current_die, &tag, &err) != DW_DLV_OK) 
            continue;

          if (tag == DW_TAG_variable) 
          {
            Dwarf_Attribute attr;
            Dwarf_Bool is_external;
            if (dwarf_attr(current_die, DW_AT_external, &attr, &err) == DW_DLV_OK) 
            {
              if (dwarf_formflag(attr, &is_external, &err) == DW_DLV_OK && is_external) 
              {
                char* var_name = nullptr;
                if (dwarf_diename(current_die, &var_name, &err) != DW_DLV_OK) 
                  continue;

                Dwarf_Attribute type_attr;
                if (dwarf_attr(current_die, DW_AT_type, &type_attr, &err) != DW_DLV_OK) 
                  continue;

                Dwarf_Off type_offset;
                if (dwarf_global_formref(type_attr, &type_offset, &err) != DW_DLV_OK) 
                  continue;

                Dwarf_Die type_die;
                if (dwarf_offdie(dbg, type_offset, &type_die, &err) != DW_DLV_OK) 
                  continue;

                std::string type_str = get_type_name(dbg, type_die);
                Dwarf_Addr addr = 0;
                if (get_variable_address(dbg, current_die, addr)) 
                {
                  global_vars[var_name] = {type_str, addr};
                }
              }
            }
          }
        } while (dwarf_siblingof(dbg, current_die, &current_die, &err) == DW_DLV_OK);
      }
    }
  }

  dwarf_finish(dbg, &err);
  close(fd);
}

