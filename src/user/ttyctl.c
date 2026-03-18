/*
 * ttyctl.c — Terminal management utility for PicoCalc
 *
 * Usage:
 *   ttyctl reset          Reset terminal to defaults (40×20, clear)
 *   ttyctl 80             Switch to 80-column mode (4×8 font, 80×40)
 *   ttyctl 40             Switch to 40-column mode (8×16 font, 40×20)
 *   ttyctl sq             Switch to square mode (8×8 font, 40×40)
 *   ttyctl cols           Print current column count
 *   ttyctl backlight N    Set LCD backlight brightness (0–255)
 *   ttyctl battery        Print battery voltage and percentage
 *   ttyctl poweroff       Power off the device
 */

#include "syscall.h"

/* ── Helpers (no libc) ───────────────────────────────────────────────────── */

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void puts_fd(int fd, const char *s)
{
    unsigned len = 0;
    const char *p = s;
    while (*p++) len++;
    write(fd, s, len);
}

static void put_u32(int fd, unsigned v)
{
    char buf[10];
    int i = 0;
    if (v == 0) { write(fd, "0", 1); return; }
    /* Subtract powers of 10 (no libgcc division in freestanding) */
    static const unsigned pw[] = {
        1000000000, 100000000, 10000000, 1000000,
        100000, 10000, 1000, 100, 10, 1
    };
    int started = 0;
    for (int p = 0; p < 10; p++) {
        unsigned d = 0;
        while (v >= pw[p]) { v -= pw[p]; d++; }
        if (d || started) { buf[i++] = (char)('0' + d); started = 1; }
    }
    write(fd, buf, (unsigned)i);
}

/* ── Subcommands ─────────────────────────────────────────────────────────── */

static void cmd_reset(void)
{
    static const char seq[] =
        "\033c"            /* RIS — full reset */
        "\033[?80l"        /* 40-col mode */
        "\033[0m"          /* default colors */
        "\033[2J"          /* clear screen */
        "\033[H";          /* cursor home */
    write(1, seq, sizeof(seq) - 1);
}

static void cmd_cols(void)
{
    struct { uint16_t row, col, xpix, ypix; } ws;
    ws.col = 0;
    if (ioctl(1, 0x5413 /* TIOCGWINSZ */, &ws) == 0) {
        put_u32(1, ws.col);
        write(1, "\n", 1);
    } else {
        puts_fd(2, "ttyctl: ioctl failed\n");
    }
}

static void cmd_backlight(const char *val)
{
    int fd = open("/dev/backlight", 1 /* O_WRONLY */, 0);
    if (fd < 0) {
        puts_fd(2, "ttyctl: /dev/backlight: open failed\n");
        return;
    }
    unsigned len = 0;
    const char *p = val;
    while (*p++) len++;
    ssize_t ret = write(fd, val, len);
    if (ret < 0) {
        puts_fd(2, "ttyctl: backlight write failed (err=");
        put_u32(2, (unsigned)(-(int)ret));
        puts_fd(2, ")\n");
    }
    close(fd);
}

static void cmd_battery(void)
{
    int fd = open("/proc/battery", 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        puts_fd(2, "ttyctl: /proc/battery: open failed\n");
        return;
    }
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n > 0)
        write(1, buf, (unsigned)n);
    else if (n < 0) {
        puts_fd(2, "ttyctl: battery read failed (err=");
        put_u32(2, (unsigned)(-(int)n));
        puts_fd(2, ")\n");
    } else {
        puts_fd(2, "ttyctl: battery: no data\n");
    }
}

static void cmd_poweroff(void)
{
    puts_fd(1, "Powering off...\n");
    int fd = open("/dev/power", 1 /* O_WRONLY */, 0);
    if (fd < 0) {
        puts_fd(2, "ttyctl: /dev/power: open failed\n");
        return;
    }
    ssize_t ret = write(fd, "off", 3);
    close(fd);
    if (ret < 0) {
        puts_fd(2, "ttyctl: power write failed (err=");
        put_u32(2, (unsigned)(-(int)ret));
        puts_fd(2, ")\n");
    }
}

static void usage(void)
{
    static const char msg[] =
        "Usage: ttyctl <command>\n"
        "  reset        Reset terminal\n"
        "  80           80-column mode (80x40)\n"
        "  40           40-column mode (40x20)\n"
        "  sq           Square mode (40x40)\n"
        "  cols         Print column count\n"
        "  backlight N  Set brightness 0-255\n"
        "  battery      Show battery info\n"
        "  poweroff     Power off device\n";
    write(2, msg, sizeof(msg) - 1);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (streq(cmd, "reset"))          cmd_reset();
    else if (streq(cmd, "80"))        write(1, "\033[?80h", 6);
    else if (streq(cmd, "40"))        write(1, "\033[?80l", 6);
    else if (streq(cmd, "sq"))        write(1, "\033[?40h", 6);
    else if (streq(cmd, "cols"))      cmd_cols();
    else if (streq(cmd, "backlight")) {
        if (argc < 3) {
            puts_fd(2, "ttyctl: backlight needs a value\n");
            return 1;
        }
        cmd_backlight(argv[2]);
    }
    else if (streq(cmd, "battery"))   cmd_battery();
    else if (streq(cmd, "poweroff"))  cmd_poweroff();
    else {
        puts_fd(2, "ttyctl: unknown command: ");
        puts_fd(2, cmd);
        puts_fd(2, "\n");
        usage();
        return 1;
    }

    return 0;
}
