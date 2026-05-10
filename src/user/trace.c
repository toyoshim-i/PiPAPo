#include "common/syscall_nr.h"
#include "lib/uclib.h"

#define put_str(s) fputs((s), stdout)
#define put_chr putchar
#define put_u32(v) printf("%u", (unsigned)(v))
#define put_hex32(v) printf("0x%08x", (unsigned)(v))
#define put_hex16(v) printf("0x%04x", (unsigned)(v))

static void put_nl(void) { putchar('\n'); }

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static const char *event_name(uint32_t event) {
  switch (event) {
    case PPAP_TRACE_EVENT_EXEC:
      return "exec";
    case PPAP_TRACE_EVENT_SYSCALL_ENTER:
      return "enter";
    case PPAP_TRACE_EVENT_SYSCALL_EXIT:
      return "exit";
    case PPAP_TRACE_EVENT_SUBSYS_ENTER:
      return "sub-enter";
    case PPAP_TRACE_EVENT_SUBSYS_EXIT:
      return "sub-exit";
    default:
      return "event";
  }
}

static const char *abi_name(uint32_t abi) {
  switch (abi) {
    case PPAP_TRACE_ABI_PPAP:
      return "ppap";
    case PPAP_TRACE_ABI_H68K_DOS:
      return "h68k-dos";
    case PPAP_TRACE_ABI_H68K_IOCS:
      return "h68k-iocs";
    case PPAP_TRACE_ABI_CPM_BDOS:
      return "cpm-bdos";
    case PPAP_TRACE_ABI_CPM_BIOS:
      return "cpm-bios";
    case PPAP_TRACE_ABI_SOS:
      return "sos";
    default:
      return "unknown";
  }
}

static const char *sys_name(uint32_t nr) {
  switch (nr) {
    case SYS_EXIT:
      return "exit";
    case SYS_EXECVE:
      return "execve";
    case SYS_WAITPID:
      return "waitpid";
    case SYS_GETPID:
      return "getpid";
    case SYS_PTRACE:
      return "ptrace";
    case SYS_READ:
      return "read";
    case SYS_WRITE:
      return "write";
    case SYS_OPEN:
      return "open";
    case SYS_CLOSE:
      return "close";
    case SYS_DUP:
      return "dup";
    case SYS_DUP2:
      return "dup2";
    case SYS_PIPE:
      return "pipe";
    case SYS_IOCTL:
      return "ioctl";
    case SYS_READV:
      return "readv";
    case SYS_WRITEV:
      return "writev";
    case SYS_LSEEK:
      return "lseek";
    case SYS_STAT:
      return "stat";
    case SYS_FSTAT:
      return "fstat";
    case SYS_ACCESS:
      return "access";
    case SYS_GETCWD:
      return "getcwd";
    case SYS_MKDIR:
      return "mkdir";
    case SYS_RMDIR:
      return "rmdir";
    case SYS_UNLINK:
      return "unlink";
    case SYS_CHDIR:
      return "chdir";
    case SYS_GETDENTS:
      return "getdents";
    case SYS_STAT64:
      return "stat64";
    case SYS_FSTAT64:
      return "fstat64";
    case SYS_BRK:
      return "brk";
    case SYS_MMAP2:
      return "mmap2";
    case SYS_MUNMAP:
      return "munmap";
    case SYS_NANOSLEEP:
      return "nanosleep";
    case SYS_KILL:
      return "kill";
    case SYS_POLL:
      return "poll";
    case SYS_PPOLL:
      return "ppoll";
    default:
      return (const char *)0;
  }
}

static const char *h68k_dos_name(uint32_t nr) {
  switch (nr) {
    case 0x00:
      return "_EXIT";
    case 0x01:
      return "_GETCHAR";
    case 0x02:
      return "_PUTCHAR";
    case 0x03:
      return "_COMINP";
    case 0x04:
      return "_COMOUT";
    case 0x05:
      return "_MOVE";
    case 0x08:
      return "_INPOUT";
    case 0x09:
      return "_PRINT";
    case 0x0A:
      return "_GETS";
    case 0x0C:
      return "_KFLUSH";
    case 0x10:
      return "_CONCTRL";
    case 0x18:
      return "_NAMECK";
    case 0x19:
      return "_CURDRV";
    case 0x1A:
      return "_ALLCLOSE";
    case 0x1B:
      return "_FGETC";
    case 0x1C:
      return "_FGETS";
    case 0x1D:
      return "_FPUTC";
    case 0x1E:
      return "_FPUTS";
    case 0x20:
      return "_SUPER";
    case 0x24:
      return "_FFLUSH";
    case 0x2A:
      return "_GETDATE";
    case 0x2B:
      return "_SETDATE";
    case 0x2C:
      return "_GETTIME";
    case 0x2D:
      return "_SETTIME";
    case 0x30:
      return "_VERNUM";
    case 0x31:
      return "_KEEPPR";
    case 0x33:
      return "_BREAKCK";
    case 0x35:
      return "_INTVCG";
    case 0x36:
      return "_DSKFRE";
    case 0x38:
      return "_GETENV";
    case 0x39:
      return "_MKDIR";
    case 0x3A:
      return "_RMDIR";
    case 0x3B:
      return "_CHDIR";
    case 0x3C:
      return "_CREATE";
    case 0x3D:
      return "_OPEN";
    case 0x3E:
      return "_CLOSE";
    case 0x3F:
      return "_READ";
    case 0x40:
      return "_WRITE";
    case 0x41:
      return "_DELETE";
    case 0x42:
      return "_SEEK";
    case 0x43:
      return "_CHMOD";
    case 0x44:
      return "_IOCTRL";
    case 0x45:
      return "_DUP";
    case 0x46:
      return "_DUP2";
    case 0x47:
      return "_CURDIR";
    case 0x48:
      return "_MALLOC";
    case 0x49:
      return "_MFREE";
    case 0x4A:
      return "_SETBLOCK";
    case 0x4B:
      return "_EXEC";
    case 0x4C:
      return "_EXIT2";
    case 0x4D:
      return "_ASSIGN";
    case 0x4E:
      return "_FILES";
    case 0x4F:
      return "_NFILES";
    case 0x51:
      return "_GETPDB";
    case 0x56:
      return "_RENAME";
    case 0x57:
      return "_FILEDATE";
    default:
      return (const char *)0;
  }
}

static const char *h68k_iocs_name(uint32_t nr) {
  switch (nr) {
    case 0x00:
      return "_B_KEYINP";
    case 0x04:
      return "_B_KEYSNS";
    case 0x0E:
      return "_SKEY_MOD";
    case 0x19:
      return "_B_UP";
    case 0x1A:
      return "_B_DOWN";
    case 0x1B:
      return "_B_RIGHT";
    case 0x1C:
      return "_B_LEFT";
    case 0x20:
      return "_B_PUTC";
    case 0x21:
      return "_B_PRINT";
    case 0x22:
      return "_B_COLOR";
    case 0x23:
      return "_B_LOCATE";
    case 0x2A:
      return "_B_CLRST";
    case 0x2B:
      return "_B_ERA_AL";
    case 0x5A:
      return "_DATEGET";
    case 0x5B:
      return "_TIMEGET";
    case 0x7F:
      return "_ONTIME";
    default:
      return (const char *)0;
  }
}

static const char *cpm_bdos_name(uint32_t nr) {
  switch (nr) {
    case 0:
      return "warm_boot";
    case 1:
      return "console_input";
    case 2:
      return "console_output";
    case 3:
      return "reader_input";
    case 4:
      return "punch_output";
    case 5:
      return "list_output";
    case 6:
      return "direct_console_io";
    case 7:
      return "get_iobyte";
    case 8:
      return "set_iobyte";
    case 9:
      return "print_string";
    case 10:
      return "read_console_buffer";
    case 11:
      return "console_status";
    case 12:
      return "version_number";
    case 13:
      return "reset_disk";
    case 14:
      return "select_disk";
    case 15:
      return "open_file";
    case 16:
      return "close_file";
    case 17:
      return "search_first";
    case 18:
      return "search_next";
    case 19:
      return "delete_file";
    case 20:
      return "read_sequential";
    case 21:
      return "write_sequential";
    case 22:
      return "make_file";
    case 23:
      return "rename_file";
    case 24:
      return "return_login_vector";
    case 25:
      return "return_current_disk";
    case 26:
      return "set_dma_address";
    case 27:
      return "get_alloc_vector";
    case 28:
      return "write_protect_disk";
    case 29:
      return "get_readonly_vector";
    case 30:
      return "set_file_attributes";
    case 31:
      return "get_dpb";
    case 32:
      return "set_get_user_code";
    case 33:
      return "read_random";
    case 34:
      return "write_random";
    case 35:
      return "compute_file_size";
    case 36:
      return "set_random_record";
    case 40:
      return "write_random_zero_fill";
    default:
      return (const char *)0;
  }
}

static const char *cpm_bios_name(uint32_t nr) {
  switch (nr) {
    case 0:
      return "boot";
    case 1:
      return "warmboot";
    case 2:
      return "const";
    case 3:
      return "conin";
    case 4:
      return "conout";
    case 5:
      return "list";
    case 6:
      return "punch";
    case 7:
      return "reader";
    case 15:
      return "listst";
    case 16:
      return "sectran";
    default:
      return (const char *)0;
  }
}

static const char *sos_name(uint32_t nr) {
  switch (nr) {
    case 0:
      return "#COLD";
    case 1:
      return "#HOT";
    case 2:
      return "#VER";
    case 3:
      return "#PRINT";
    case 4:
      return "#PRINTS";
    case 5:
      return "#LTNL";
    case 6:
      return "#NL";
    case 7:
      return "#MSG";
    case 8:
      return "#MSX";
    case 9:
      return "#MPRINT";
    case 10:
      return "#TAB";
    case 11:
      return "#LPRINT";
    case 12:
      return "#LPTON";
    case 13:
      return "#LPTOF";
    case 14:
      return "#GETL";
    case 15:
      return "#GETKY";
    case 16:
      return "#BRKEY";
    case 17:
      return "#INKEY";
    case 18:
      return "#PAUSE";
    case 19:
      return "#BELL";
    case 20:
      return "#PRTHX";
    case 21:
      return "#PRTHL";
    case 22:
      return "#ASC";
    case 23:
      return "#HEX";
    case 24:
      return "#2HEX";
    case 25:
      return "#HLHEX";
    case 26:
      return "#WOPEN";
    case 27:
      return "#WRD";
    case 28:
      return "#FCB";
    case 29:
      return "#RDD";
    case 30:
      return "#FILE";
    case 31:
      return "#FSAME";
    case 32:
      return "#FPRNT";
    case 33:
      return "#POKE";
    case 34:
      return "#POKEA";
    case 35:
      return "#PEEK";
    case 36:
      return "#PEEKA";
    case 37:
      return "#MON";
    case 40:
      return "#DRDSB";
    case 41:
      return "#DWTSB";
    case 42:
      return "#DIR";
    case 43:
      return "#ROPEN";
    case 44:
      return "#SET";
    case 45:
      return "#RESET";
    case 46:
      return "#NAME";
    case 47:
      return "#KILL";
    case 48:
      return "#CSR";
    case 49:
      return "#SCRN";
    case 50:
      return "#LOC";
    case 51:
      return "#FLGET";
    case 52:
      return "#RDVSW";
    case 53:
      return "#SDVSW";
    case 54:
      return "#INP";
    case 55:
      return "#OUT";
    case 56:
      return "#WIDCH";
    case 57:
      return "#ERROR";
    case 58:
      return "#BOOT";
    default:
      return (const char *)0;
  }
}

static const char *subsys_name(uint32_t abi, uint32_t nr) {
  switch (abi) {
    case PPAP_TRACE_ABI_H68K_DOS:
      return h68k_dos_name(nr);
    case PPAP_TRACE_ABI_H68K_IOCS:
      return h68k_iocs_name(nr);
    case PPAP_TRACE_ABI_CPM_BDOS:
      return cpm_bdos_name(nr);
    case PPAP_TRACE_ABI_CPM_BIOS:
      return cpm_bios_name(nr);
    case PPAP_TRACE_ABI_SOS:
      return sos_name(nr);
    default:
      return (const char *)0;
  }
}

static void print_subsys_args(const struct ppap_ptrace_event *ev) {
  switch (ev->abi) {
    case PPAP_TRACE_ABI_H68K_DOS:
    case PPAP_TRACE_ABI_H68K_IOCS:
      put_str(" d0=");
      put_hex32(ev->args[0]);
      put_str(" d1=");
      put_hex32(ev->args[1]);
      put_str(" d2=");
      put_hex32(ev->args[2]);
      put_str(" d3=");
      put_hex32(ev->args[3]);
      put_str(" a0=");
      put_hex32(ev->args[4]);
      put_str(" a1=");
      put_hex32(ev->args[5]);
      break;
    case PPAP_TRACE_ABI_CPM_BDOS:
    case PPAP_TRACE_ABI_CPM_BIOS:
    case PPAP_TRACE_ABI_SOS:
      put_str(" af=");
      put_hex16(ev->args[0]);
      put_str(" bc=");
      put_hex16(ev->args[1]);
      put_str(" de=");
      put_hex16(ev->args[2]);
      put_str(" hl=");
      put_hex16(ev->args[3]);
      put_str(" sp=");
      put_hex16(ev->args[4]);
      put_str(" pc=");
      put_hex16(ev->args[5]);
      break;
    default:
      put_str(" a0=");
      put_hex32(ev->args[0]);
      put_str(" a1=");
      put_hex32(ev->args[1]);
      put_str(" a2=");
      put_hex32(ev->args[2]);
      break;
  }
}

static void print_event(const struct ppap_ptrace_event *ev) {
  put_str(abi_name(ev->abi));
  put_chr(' ');
  put_str(event_name(ev->event));
  put_chr(' ');

  if (ev->abi == PPAP_TRACE_ABI_PPAP) {
    const char *name = sys_name(ev->nr);
    if (name)
      put_str(name);
    else
      put_hex32(ev->nr);
  } else {
    const char *name = subsys_name(ev->abi, ev->nr);
    if (name)
      put_str(name);
    else
      put_hex32(ev->nr);
  }

  if (ev->event == PPAP_TRACE_EVENT_SYSCALL_ENTER ||
      ev->event == PPAP_TRACE_EVENT_SUBSYS_ENTER) {
    if (ev->abi == PPAP_TRACE_ABI_PPAP) {
      put_str(" a0=");
      put_hex32(ev->args[0]);
      put_str(" a1=");
      put_hex32(ev->args[1]);
      put_str(" a2=");
      put_hex32(ev->args[2]);
    } else {
      print_subsys_args(ev);
    }
  } else if (ev->event == PPAP_TRACE_EVENT_SYSCALL_EXIT ||
             ev->event == PPAP_TRACE_EVENT_SUBSYS_EXIT) {
    put_str(" = ");
    put_hex32((uint32_t)ev->ret);
  }
  put_nl();
}

static void print_regs(const struct ppap_ptrace_regs *regs) {
  put_str("  ");
  switch (regs->regset) {
    case PPAP_TRACE_REGSET_ARM:
      put_str("arm pc=");
      put_hex32(regs->regs[15]);
      put_str(" sp=");
      put_hex32(regs->regs[13]);
      put_str(" r0=");
      put_hex32(regs->regs[0]);
      put_str(" r1=");
      put_hex32(regs->regs[1]);
      put_str(" r2=");
      put_hex32(regs->regs[2]);
      put_str(" r3=");
      put_hex32(regs->regs[3]);
      break;
    case PPAP_TRACE_REGSET_M68K:
      put_str("m68k pc=");
      put_hex32(regs->regs[16]);
      put_str(" sr=");
      put_hex16(regs->regs[17]);
      put_str(" d0=");
      put_hex32(regs->regs[0]);
      put_str(" d1=");
      put_hex32(regs->regs[1]);
      put_str(" a0=");
      put_hex32(regs->regs[8]);
      put_str(" a7=");
      put_hex32(regs->regs[15]);
      break;
    case PPAP_TRACE_REGSET_Z80:
      put_str("z80 pc=");
      put_hex16(regs->regs[7]);
      put_str(" af=");
      put_hex16(regs->regs[0]);
      put_str(" bc=");
      put_hex16(regs->regs[1]);
      put_str(" de=");
      put_hex16(regs->regs[2]);
      put_str(" hl=");
      put_hex16(regs->regs[3]);
      put_str(" sp=");
      put_hex16(regs->regs[6]);
      break;
    default:
      put_str("regset=");
      put_u32(regs->regset);
      break;
  }
  put_nl();
}

static void usage(void) {
  put_str(
      "Usage: trace [--ppap] [--subsys] [--both] [--regs] <path> [args...]\n");
}

int main(int argc, char *argv[]) {
  volatile int dump_regs = 0;
  int mode_seen = 0;
  uint32_t mode = 0;
  volatile int cmd = 1;

  while (cmd < argc && argv[cmd][0] == '-') {
    if (streq(argv[cmd], "--ppap")) {
      if (!mode_seen) mode = 0;
      mode_seen = 1;
      mode |= PPAP_TRACE_MODE_PPAP_SYSCALL;
    } else if (streq(argv[cmd], "--subsys")) {
      if (!mode_seen) mode = 0;
      mode_seen = 1;
      mode |= PPAP_TRACE_MODE_SUBSYS_CALL;
    } else if (streq(argv[cmd], "--both")) {
      mode_seen = 1;
      mode = PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL;
    } else if (streq(argv[cmd], "--regs")) {
      dump_regs = 1;
    } else {
      usage();
      return 1;
    }
    cmd++;
  }

  if (cmd >= argc) {
    usage();
    return 1;
  }
  if (!mode_seen)
    mode = PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL;

  pid_t pid = vfork();
  if (pid == 0) {
    ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
    execve(argv[cmd], &argv[cmd], (void *)0);
    _exit(127);
  }

  for (;;) {
    int status = 0;
    struct ppap_ptrace_event ev;
    struct ppap_ptrace_regs regs;

    if (waitpid(pid, &status, WSTOPPED) != pid) {
      put_str("trace: waitpid failed\n");
      return 1;
    }
    if (WIFEXITED(status)) {
      put_str("exit ");
      put_u32((uint32_t)WEXITSTATUS(status));
      put_nl();
      return WEXITSTATUS(status);
    }
    if (!WIFSTOPPED(status)) {
      put_str("trace: unexpected child state\n");
      return 1;
    }

    if (ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev) < 0) {
      put_str("trace: GETEVENT failed\n");
      return 1;
    }
    print_event(&ev);

    if (dump_regs) {
      if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
        put_str("trace: GETREGS failed\n");
        return 1;
      }
      print_regs(&regs);
    }

    if (ptrace(PTRACE_SETMODE, pid, (void *)(uintptr_t)mode, (void *)0) < 0) {
      put_str("trace: SETMODE failed\n");
      return 1;
    }
    if (ptrace(PTRACE_CONT, pid, (void *)0, (void *)0) < 0) {
      put_str("trace: CONT failed\n");
      return 1;
    }
  }
}
