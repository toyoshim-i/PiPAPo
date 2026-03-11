#include "syscall.h"

static volatile uint32_t trace_word = 0x11223344u;

int main(void)
{
    write(1, (const void *)&trace_word, sizeof(trace_word));
    return trace_word == 0x55667788u ? 0 : 3;
}
