/*
 * main.c — Kernel early init entry point (Phase 0 stub)
 *
 * Phase 0 Step 2: minimal stub to verify the build pipeline.
 * Steps 5–7 will replace this with real clock init, UART init,
 * and XIP verification output.
 */

int main(void) {
    /* Placeholder: spin forever until real init code is written.
     * Step 4 will replace pico_crt0 with our custom startup.S,
     * at which point this entry point becomes kmain(). */
    for (;;) {
    }
}
