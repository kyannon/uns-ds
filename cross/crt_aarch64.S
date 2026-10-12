/* Minimal program entry for aarch64 glibc: hands off to __libc_start_main.
 * Equivalent to glibc's sysdeps/aarch64/start.S without init/fini (unused). */
    .text
    .globl _start
    .type _start, %function
_start:
    mov x29, #0
    mov x30, #0
    mov x5, x0              /* rtld_fini from the dynamic linker */
    ldr x1, [sp]            /* argc */
    add x2, sp, #8          /* argv */
    mov x6, sp              /* stack_end */
    mov x3, #0              /* init */
    mov x4, #0              /* fini */
    adrp x0, main
    add x0, x0, :lo12:main
    bl __libc_start_main
    brk #1000
    .size _start, .-_start
    .section .note.GNU-stack,"",%progbits
