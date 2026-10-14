; Interrupt stubs
[bits 32]
section .text
extern irq0_handler, irq1_handler, exception_handler
global irq0_stub, irq1_stub, isr_ignore, exc_table

irq0_stub:                      ; PIT timer
    pushad
    cld
    call irq0_handler
    popad
    iretd

irq1_stub:                      ; PS/2 keyboard
    pushad
    cld
    call irq1_handler
    popad
    iretd

isr_ignore:                     ; spurious / unused vectors
    iretd

; CPU exceptions 0-31: push the vector number and panic.
%assign i 0
%rep 32
exc %+ i:
    cli
    push dword i
    call exception_handler
    jmp $
%assign i i+1
%endrep

section .data
exc_table:
%assign i 0
%rep 32
    dd exc %+ i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
