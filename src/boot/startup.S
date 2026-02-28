@ startup.S — Reset handler placeholder
@ Phase 0 Step 2: stub to satisfy the linker.
@ Will be replaced with real vector table + BSS zero + data copy in Step 4.

.syntax unified
.cpu cortex-m0plus
.thumb

.section .text
.global _start
_start:
    b _start        @ placeholder: loop until real startup is written
