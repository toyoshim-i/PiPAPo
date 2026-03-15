#include "pdb_internal.h"
#include "pdb_trace_util.h"
#include "pdb_util.h"

static int handle_show_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    pid_t pid = ctx->pid;
    int child_stopped = *ctx->child_stopped;
    struct ppap_ptrace_event *last_ev = ctx->last_ev;
    struct ppap_ptrace_caps *caps = ctx->caps;

    if (!streq(tok[0], "show") && !streq(tok[0], "pc") && !streq(tok[0], "sp"))
        return 0;

    {
        const char *show_item = 0;
        if (streq(tok[0], "show")) {
            if (ntok != 2) {
                put_err("pdb: usage: show <abi|event|caps|regset|pc|sp|surface>\n");
                return 1;
            }
            show_item = tok[1];
        } else {
            if (ntok != 1) {
                if (streq(tok[0], "pc"))
                    put_err("pdb: usage: pc\n");
                else
                    put_err("pdb: usage: sp\n");
                return 1;
            }
            show_item = tok[0];
        }
        if (streq(show_item, "abi")) {
            put_str("abi=");
            put_str(abi_name(last_ev->abi));
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "event")) {
            print_event(last_ev);
            return 1;
        }
        if (streq(show_item, "caps")) {
            if (!get_caps_if_stopped(pid, child_stopped, caps))
                return 1;
            print_caps(caps);
            return 1;
        }
        if (streq(show_item, "regset")) {
            struct ppap_ptrace_regs regs;
            if (!get_regs_if_stopped(pid, child_stopped, &regs))
                return 1;
            put_str("regset=");
            put_str(regset_name(regs.regset));
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "pc")) {
            struct ppap_ptrace_regs regs;
            uint32_t pc_idx = 0;
            uint32_t sp_idx = 0;
            if (!get_regs_if_stopped(pid, child_stopped, &regs))
                return 1;
            if (!regset_pc_sp_indices(regs.regset, &pc_idx, &sp_idx)) {
                put_err("pdb: unsupported regset for show pc\n");
                return 1;
            }
            if (pc_idx >= regs.words) {
                put_err("pdb: pc index out of range\n");
                return 1;
            }
            put_str("pc=");
            print_reg_value(regs.regset, regs.regs[pc_idx]);
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "sp")) {
            struct ppap_ptrace_regs regs;
            uint32_t pc_idx = 0;
            uint32_t sp_idx = 0;
            if (!get_regs_if_stopped(pid, child_stopped, &regs))
                return 1;
            if (!regset_pc_sp_indices(regs.regset, &pc_idx, &sp_idx)) {
                put_err("pdb: unsupported regset for show sp\n");
                return 1;
            }
            if (sp_idx >= regs.words) {
                put_err("pdb: sp index out of range\n");
                return 1;
            }
            put_str("sp=");
            print_reg_value(regs.regset, regs.regs[sp_idx]);
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "surface")) {
            uint32_t surface = 0;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                return 1;
            }
            if (ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface) == 0) {
                put_str("surface=");
                put_str(surface_name_for_value(surface));
                put_chr('\n');
                return 1;
            }
            /* Fallback for kernels without PTRACE_GETSURFACE support. */
            if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
                put_str("surface=");
                put_str(surface_name_for_regset(caps->regset));
                put_chr('\n');
                return 1;
            }
            put_err("pdb: GETSURFACE/GETCAPS failed\n");
            return 1;
        }
        if (streq(tok[0], "show"))
            put_err("pdb: usage: show <abi|event|caps|regset|pc|sp|surface>\n");
        else if (streq(tok[0], "pc"))
            put_err("pdb: usage: pc\n");
        else
            put_err("pdb: usage: sp\n");
        return 1;
    }
}

static int handle_memory_view_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    pid_t pid = ctx->pid;
    int child_stopped = *ctx->child_stopped;

    if (streq(tok[0], "mem")) {
        uint32_t addr = 0;
        uint32_t count = 4;
        uint32_t width = 4;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok < 2 || ntok > 4 || !parse_u32(tok[1], &addr)) {
            put_err("pdb: usage: mem <addr> [count] [size]\n");
            put_err("pdb:        size: b|h|w (or 1|2|4)\n");
            return 1;
        }
        if (ntok >= 3 && !parse_u32(tok[2], &count)) {
            put_err("pdb: invalid count\n");
            return 1;
        }
        if (ntok == 4 && !parse_mem_width(tok[3], &width)) {
            put_err("pdb: usage: mem <addr> [count] [size]\n");
            put_err("pdb:        size: b|h|w (or 1|2|4)\n");
            return 1;
        }
        if (count == 0) {
            put_err("pdb: invalid count\n");
            return 1;
        }
        if (width == 1u)
            print_mem_bytes(pid, addr, count);
        else if (width == 2u)
            print_mem_halfwords(pid, addr, count);
        else
            print_mem_words(pid, addr, count);
        return 1;
    }

    if (streq(tok[0], "x") || (tok[0][0] == 'x' && tok[0][1] == '/')) {
        uint32_t addr = 0;
        uint32_t count = 4;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (streq(tok[0], "x")) {
            if (ntok < 2 || ntok > 3 || !parse_u32(tok[1], &addr)) {
                put_err("pdb: usage: x <addr> [count]\n");
                put_err("pdb:    or: x/<n><fmt> <addr> (fmt: x|h|b)\n");
                return 1;
            }
            if (ntok == 3 && !parse_u32(tok[2], &count)) {
                put_err("pdb: invalid count\n");
                return 1;
            }
        } else {
            char fmt = 'x';
            if (!parse_x_spec(tok[0], &count, &fmt) || ntok != 2 ||
                !parse_u32(tok[1], &addr)) {
                put_err("pdb: usage: x <addr> [count]\n");
                put_err("pdb:    or: x/<n><fmt> <addr> (fmt: x|h|b)\n");
                return 1;
            }
            if (count == 0) {
                put_err("pdb: invalid count\n");
                return 1;
            }
            if (fmt == 'b') {
                print_mem_bytes(pid, addr, count);
            } else if (fmt == 'h') {
                print_mem_halfwords(pid, addr, count);
            } else {
                print_mem_words(pid, addr, count);
            }
            return 1;
        }
        print_mem_words(pid, addr, count);
        return 1;
    }

    return 0;
}

static int handle_disassemble_command(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    pid_t pid = ctx->pid;
    int child_stopped = *ctx->child_stopped;

    if (!streq(tok[0], "disas"))
        return 0;

    {
        struct ppap_ptrace_regs regs;
        uint32_t addr = 0;
        uint32_t count = 8;
        uint32_t pc_idx = 0;
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        if (!regset_pc_index(regs.regset, &pc_idx)) {
            put_err("pdb: disas currently supports arm, z80, and m68k tracees only\n");
            return 1;
        }
        addr = regs.regs[pc_idx];
        if (ntok > 3) {
            put_err("pdb: usage: disas [addr] [count]\n");
            return 1;
        }
        if (ntok >= 2 && !parse_u32(tok[1], &addr)) {
            put_err("pdb: usage: disas [addr] [count]\n");
            return 1;
        }
        if (ntok >= 3 && !parse_u32(tok[2], &count)) {
            put_err("pdb: invalid count\n");
            return 1;
        }
        if (regs.regset == PPAP_TRACE_REGSET_Z80)
            disas_z80(pid, addr, count);
        else if (regs.regset == PPAP_TRACE_REGSET_M68K)
            disas_m68k(pid, addr, count);
        else
            disas_thumb(pid, addr, count);
        return 1;
    }
}

int handle_inspect_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    pid_t pid = ctx->pid;
    int child_stopped = *ctx->child_stopped;
    struct ppap_ptrace_event *last_ev = ctx->last_ev;
    struct ppap_ptrace_caps *caps = ctx->caps;

    if (streq(tok[0], "help") || streq(tok[0], "?")) {
        if (ntok != 1) {
            put_err("pdb: usage: help\n");
            return 1;
        }
        print_help();
        return 1;
    }

    if (streq(tok[0], "regs")) {
        struct ppap_ptrace_regs regs;
        if (ntok != 1) {
            put_err("pdb: usage: regs\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        print_regs(&regs);
        return 1;
    }

    if (streq(tok[0], "reg")) {
        struct ppap_ptrace_regs regs;
        uint32_t idx = 0;
        int is16 = 0;
        if (ntok != 2) {
            put_err("pdb: usage: reg <name|index>\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        if (!reg_index_from_token(&regs, tok[1], &idx)) {
            put_err("pdb: unknown register\n");
            return 1;
        }
        is16 = reg_is16(regs.regset);
        {
            const char *name = reg_name(regs.regset, idx);
            if (name) {
                put_str(name);
            } else {
                put_str("r");
                put_u32(idx);
            }
        }
        put_str("=");
        if (is16)
            put_hex16(regs.regs[idx]);
        else
            put_hex32(regs.regs[idx]);
        put_chr('\n');
        return 1;
    }

    if (streq(tok[0], "caps")) {
        if (ntok != 1) {
            put_err("pdb: usage: caps\n");
            return 1;
        }
        if (!get_caps_if_stopped(pid, child_stopped, caps))
            return 1;
        print_caps(caps);
        return 1;
    }

    if (streq(tok[0], "event")) {
        if (ntok != 1) {
            put_err("pdb: usage: event\n");
            return 1;
        }
        print_event(last_ev);
        return 1;
    }

    if (handle_show_commands(ctx, tok, ntok))
        return 1;

    if (streq(tok[0], "surface")) {
        uint32_t surface = 0;
        long rc;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok != 2 || !parse_surface_token(tok[1], &surface)) {
            put_err("pdb: usage: surface <real|ecpu>\n");
            return 1;
        }
        rc = ptrace(PTRACE_SETSURFACE, pid, (void *)(uintptr_t)surface, (void *)0);
        if (rc < 0) {
            put_err("pdb: SETSURFACE failed rc=");
            put_i32((int32_t)rc);
            put_err(" (target may not support requested surface)\n");
            return 1;
        }
        if (ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface) < 0) {
            put_err("pdb: GETSURFACE failed\n");
            return 1;
        }
        put_str("surface=");
        put_str(surface_name_for_value(surface));
        put_chr('\n');
        return 1;
    }

    if (streq(tok[0], "where") || streq(tok[0], "w")) {
        struct ppap_ptrace_regs regs;
        uint32_t pc_idx = 0;
        uint32_t sp_idx = 0;
        if (ntok != 1) {
            put_err("pdb: usage: where\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        if (!regset_pc_sp_indices(regs.regset, &pc_idx, &sp_idx)) {
            put_err("pdb: unsupported regset for where\n");
            return 1;
        }
        if (pc_idx >= regs.words || sp_idx >= regs.words) {
            put_err("pdb: where index out of range\n");
            return 1;
        }
        put_str("pc=");
        print_reg_value(regs.regset, regs.regs[pc_idx]);
        put_str(" sp=");
        print_reg_value(regs.regset, regs.regs[sp_idx]);
        put_chr('\n');
        return 1;
    }

    if (streq(tok[0], "bt")) {
        struct ppap_ptrace_regs regs;
        uint32_t count = 8;
        if (ntok > 2) {
            put_err("pdb: usage: bt [count]\n");
            return 1;
        }
        if (ntok == 2 && !parse_u32(tok[1], &count)) {
            put_err("pdb: usage: bt [count]\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        print_backtrace(pid, &regs, count);
        return 1;
    }

    if (handle_memory_view_commands(ctx, tok, ntok))
        return 1;

    if (handle_disassemble_command(ctx, tok, ntok))
        return 1;

    return 0;
}

int handle_write_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    pid_t pid = ctx->pid;
    int child_stopped = *ctx->child_stopped;

    if (streq(tok[0], "set")) {
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }

        if (ntok == 4 && streq(tok[1], "reg")) {
            struct ppap_ptrace_regs regs;
            int is16 = 0;
            uint32_t idx = 0;
            uint32_t value = 0;

            if (!parse_u32(tok[3], &value)) {
                put_err("pdb: invalid register value\n");
                return 1;
            }
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: GETREGS failed\n");
                return 1;
            }
            if (!reg_index_from_token(&regs, tok[2], &idx)) {
                put_err("pdb: unknown register\n");
                return 1;
            }

            is16 = reg_is16(regs.regset);
            if (is16)
                value &= 0xffffu;
            regs.regs[idx] = value;
            if (ptrace(PTRACE_SETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: SETREGS failed\n");
                return 1;
            }

            put_str("reg ");
            {
                const char *name = reg_name(regs.regset, idx);
                if (name) {
                    put_str(name);
                } else {
                    put_str("r");
                    put_u32(idx);
                }
            }
            put_str("=");
            if (is16)
                put_hex16(value);
            else
                put_hex32(value);
            put_chr('\n');
            return 1;
        }

        if ((ntok == 4 || ntok == 5) && streq(tok[1], "mem")) {
            uint32_t addr = 0;
            uint32_t value = 0;
            uint32_t width = 4u;
            long rc;

            if (!parse_u32(tok[2], &addr) || !parse_u32(tok[3], &value) ||
                (ntok == 5 && !parse_mem_width(tok[4], &width))) {
                put_err("pdb: usage: set mem <addr> <value> [size]\n");
                put_err("pdb:        size: b|h|w (or 1|2|4)\n");
                return 1;
            }
            if (width == 4u) {
                rc = ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)addr, &value);
            } else {
                uint32_t base = addr & ~3u;
                uint32_t old_word = 0;
                uint32_t new_word = 0;
                uint32_t shift = (addr & 3u) * 8u;
                uint32_t mask = (width == 1u) ? 0xffu : 0xffffu;

                if (width == 2u && (addr & 3u) == 3u) {
                    put_err("pdb: set mem halfword must not cross word boundary\n");
                    return 1;
                }
                if (width == 2u && (addr & 1u)) {
                    put_err("pdb: set mem halfword requires even address\n");
                    return 1;
                }
                if ((width == 1u && value > 0xffu) ||
                    (width == 2u && value > 0xffffu)) {
                    put_err("pdb: set mem value out of range for size\n");
                    return 1;
                }

                rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)base, &old_word);
                if (rc < 0) {
                    put_err("pdb: PEEKDATA failed rc=");
                    put_i32((int32_t)rc);
                    put_chr('\n');
                    return 1;
                }
                value &= mask;
                new_word = (old_word & ~(mask << shift)) | (value << shift);
                rc = ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)base, &new_word);
            }
            if (rc < 0) {
                put_err("pdb: POKEDATA failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                return 1;
            }
            put_str("mem ");
            put_hex32(addr);
            put_str("=");
            if (width == 1u)
                put_hex8(value);
            else if (width == 2u)
                put_hex16(value);
            else
                put_hex32(value);
            put_chr('\n');
            return 1;
        }

        put_err("pdb: usage: set reg <name|index> <value>\n");
        put_err("pdb:    or: set mem <addr> <value> [size]\n");
        return 1;
    }

    if (streq(tok[0], "restore")) {
        uint32_t addr = 0;
        uint32_t restored = 0;
        uint8_t bytes[8];
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok < 4 || !streq(tok[1], "mem") || !parse_u32(tok[2], &addr)) {
            put_err("pdb: usage: restore mem <addr> <byte...>\n");
            return 1;
        }
        for (int i = 3; i < ntok; i++) {
            uint32_t v = 0;
            if (!parse_u32(tok[i], &v) || v > 0xffu) {
                put_err("pdb: usage: restore mem <addr> <byte...>\n");
                restored = 0;
                break;
            }
            bytes[restored] = (uint8_t)v;
            restored++;
        }
        if (restored == 0)
            return 1;
        for (uint32_t i = 0; i < restored; i++) {
            int rc = poke_u8(pid, addr + i, bytes[i]);
            if (rc < 0) {
                put_err("pdb: POKEDATA failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                restored = 0;
                break;
            }
        }
        if (restored == 0)
            return 1;
        put_str("mem ");
        put_hex32(addr);
        put_str(" restored ");
        put_u32(restored);
        put_str(" bytes\n");
        return 1;
    }

    return 0;
}
