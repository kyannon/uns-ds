/* x86-64 twin of crt_aarch64.S, used to test the stub-linking method on a PC */
    .text
    .globl _start
_start:
    xor %ebp, %ebp
    mov %rdx, %r9
    pop %rsi
    mov %rsp, %rdx
    and $-16, %rsp
    push %rax
    push %rsp
    xor %r8d, %r8d
    xor %ecx, %ecx
    lea main(%rip), %rdi
    call *__libc_start_main@GOTPCREL(%rip)
    hlt
    .section .note.GNU-stack,"",%progbits
