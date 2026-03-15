#include "pdb_internal.h"
#include "pdb_util.h"

void usage(void)
{
    put_str("Usage: pdb [-q] [--batch] [-c <cmd> ...] [-f <script> ...] <program> [args...]\n");
    put_str("       pdb [-q] [--batch] [-c <cmd> ...] [-f <script> ...] --attach <pid>\n");
}

void print_help(void)
{
    put_str("options:\n");
    put_str("  -q                suppress prompt/command echo output\n");
    put_str("  --batch           suppress automatic stop/target output\n");
    put_str("  -c <cmd>          queue startup command (repeatable)\n");
    put_str("  -f <script>       load startup commands from file (repeatable)\n");
    put_str("  --attach <pid>    attach to a running process instead of exec\n");
    put_str("                    script format: one command per line, '#' comment\n");
    put_str("\n");
    put_str("commands:\n");
    put_str("  help              show this help\n");
    put_str("  regs              show registers\n");
    put_str("  reg <name|idx>    show one register\n");
    put_str("  caps              show trace capabilities\n");
    put_str("  event             show last stop event\n");
    put_str("  show abi          show current stop ABI\n");
    put_str("  show event        show last stop event\n");
    put_str("  show caps         show trace capabilities\n");
    put_str("  show regset       show current register set\n");
    put_str("  show pc           show current program counter\n");
    put_str("  show sp           show current stack pointer\n");
    put_str("  pc                show current program counter\n");
    put_str("  sp                show current stack pointer\n");
    put_str("  show surface      show current debug surface\n");
    put_str("  surface <s>       set debug surface (real|ecpu)\n");
    put_str("  where | w         show pc and sp\n");
    put_str("  bt [count]        show a simple frame-pointer backtrace\n");
    put_str("  x <addr> [count]  read memory words\n");
    put_str("  x/<n><fmt> <addr> read memory (<fmt>: x=word, h=half, b=byte)\n");
    put_str("  mem <a> [n] [sz]  read memory with size b|h|w|1|2|4\n");
    put_str("  disas [a] [n]     disassemble n instructions from addr/pc\n");
    put_str("  step | s          single-step\n");
    put_str("  next | n          step over call (z80), else single-step\n");
    put_str("  run | cont | continue | c    continue\n");
    put_str("  set reg <r> <v>   write register by name or index\n");
    put_str("  set mem <a> <v> [size]   write memory (size: b|h|w|1|2|4)\n");
    put_str("  restore mem <a> <bytes...>  write byte sequence\n");
    put_str("  break | b <addr>  set breakpoint (auto sw/hw from caps)\n");
    put_str("  break | b <sw|hw> <addr>  force breakpoint type\n");
    put_str("  disable <id>      disable breakpoint by id\n");
    put_str("  enable <id>       enable breakpoint by id\n");
    put_str("  delete | d <id>   clear breakpoint by id\n");
    put_str("  info break|b      show local breakpoint table\n");
    put_str("  detach            detach and quit\n");
    put_str("  quit | q          detach and quit\n");
}

int parse_startup_options(int argc, char *argv[],
                          int *argi, int *show_prompt, int *batch_mode,
                          int *scripted_mode, int *attach_mode,
                          pid_t *attach_pid, char **script_cmds,
                          int *script_count, char *script_storage,
                          int *script_storage_used, int *exit_code)
{
    while (*argi < argc) {
        if (streq(argv[*argi], "-h") || streq(argv[*argi], "--help")) {
            print_help();
            *exit_code = 0;
            return 0;
        }
        if (streq(argv[*argi], "-q")) {
            *show_prompt = 0;
            (*argi)++;
            continue;
        }
        if (streq(argv[*argi], "--batch")) {
            *batch_mode = 1;
            *show_prompt = 0;
            (*argi)++;
            continue;
        }
        if (streq(argv[*argi], "-c")) {
            int non_space = 0;
            int cmd_len = 0;
            const char *cmd;
            *scripted_mode = 1;
            if (*argi + 1 >= argc) {
                put_err("pdb: -c requires a command string\n");
                *exit_code = 1;
                return 0;
            }
            cmd = argv[*argi + 1];
            for (cmd_len = 0; cmd[cmd_len]; cmd_len++) {
                if (!is_script_space(cmd[cmd_len])) {
                    non_space = 1;
                    break;
                }
            }
            for (; cmd[cmd_len]; cmd_len++)
                ;
            if (!non_space) {
                *argi += 2;
                continue;
            }
            if (cmd_len >= PDB_SCRIPT_LINE_MAX) {
                put_err("pdb: -c command too long\n");
                *exit_code = 1;
                return 0;
            }
            if (*script_count >= PDB_SCRIPT_CMD_MAX) {
                put_err("pdb: too many script commands (max ");
                put_u32(PDB_SCRIPT_CMD_MAX);
                put_str(")\n");
                *exit_code = 1;
                return 0;
            }
            script_cmds[(*script_count)++] = argv[*argi + 1];
            *argi += 2;
            continue;
        }
        if (streq(argv[*argi], "-f")) {
            *scripted_mode = 1;
            if (*argi + 1 >= argc) {
                put_err("pdb: -f requires a script path\n");
                *exit_code = 1;
                return 0;
            }
            if (load_script_file(argv[*argi + 1], script_cmds, script_count,
                                 script_storage, script_storage_used) < 0) {
                *exit_code = 1;
                return 0;
            }
            *argi += 2;
            continue;
        }
        if (streq(argv[*argi], "--attach")) {
            uint32_t parsed_pid = 0;
            if (*argi + 1 >= argc) {
                put_err("pdb: --attach requires a pid\n");
                *exit_code = 1;
                return 0;
            }
            if (!parse_u32(argv[*argi + 1], &parsed_pid) ||
                parsed_pid == 0 ||
                parsed_pid > 0x7fffffffu) {
                put_err("pdb: --attach requires a valid positive pid\n");
                *exit_code = 1;
                return 0;
            }
            *attach_mode = 1;
            *attach_pid = (pid_t)parsed_pid;
            *argi += 2;
            continue;
        }
        break;
    }
    return 1;
}

int validate_startup_options(int argc, int argi,
                             int attach_mode, int scripted_mode,
                             int script_count)
{
    if (!attach_mode && argi >= argc) {
        usage();
        return 0;
    }
    if (attach_mode && argi < argc) {
        put_err("pdb: --attach does not take a program path\n");
        return 0;
    }
    if (scripted_mode && script_count == 0) {
        put_err("pdb: no scripted commands\n");
        return 0;
    }
    return 1;
}

int read_next_command_line(char *line, int line_size,
                           char **script_cmds, int script_count,
                           int *script_index, int show_prompt)
{
    if (*script_index < script_count) {
        int i = 0;
        const char *src = script_cmds[(*script_index)++];
        while (src[i] && i < line_size - 1) {
            line[i] = src[i];
            i++;
        }
        line[i] = '\0';
        if (show_prompt) {
            put_str("pdb> ");
            put_str(line);
            put_chr('\n');
        }
        return 1;
    }

    if (script_count > 0)
        return 0;
    if (show_prompt)
        put_str("pdb> ");
    if (readline(line, line_size) < 0) {
        put_chr('\n');
        return 0;
    }
    return 1;
}
