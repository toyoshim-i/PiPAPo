# =============================================================================
# boot.S — Xtensa kernel entry (ESP-IDF boot path)
# =============================================================================
#
# On the ESP32-S3, ESP-IDF handles the full boot sequence:
#   ROM bootloader → second-stage bootloader → app_main()
#
# app_main() (in target_xtensa_cc.c) calls kmain() directly.
# Unlike ARM and RISC-V targets, there is no _start / Reset_Handler here
# because ESP-IDF owns the C runtime startup (.data copy, .bss zero, etc.).
#
# This file provides:
#   Default_Handler — infinite loop for unhandled exceptions (debugging aid)
# =============================================================================

    .text
    .align 4
    .global Default_Handler
    .type   Default_Handler, @function
Default_Handler:
    waiti   15
    j       Default_Handler
    .size   Default_Handler, . - Default_Handler
