; Kernel entry point - linked first, at 0x10000.
[bits 32]
section .text.entry
global _start
extern kmain, __bss_start, __bss_end

_start:
    mov esp, 0x90000
    mov edi, __bss_start        ; zero .bss
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb
    call kmain
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
