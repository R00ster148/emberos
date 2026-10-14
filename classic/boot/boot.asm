; EmberOS boot sector
; Loaded by the BIOS at 0x7C00 (floppy, or CD in El Torito floppy emulation).
; Loads the kernel from sector 2 onward to 0x10000, enters 32-bit
; protected mode and jumps to it.

[org 0x7C00]
[bits 16]

%ifndef KSECTORS
%define KSECTORS 64
%endif

KERNEL_SEG equ 0x1000           ; 0x1000:0000 = 0x10000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    mov ax, 0x0003              ; 80x25 colour text mode
    int 0x10

    mov si, msg_loading
    call print

    ; ---- load kernel, one sector at a time (CHS, 18 spt, 2 heads) ----
    mov ax, KERNEL_SEG
    mov es, ax
    xor bx, bx
    mov word [lba], 1
    mov cx, KSECTORS
.load:
    push cx
    mov ax, [lba]
    xor dx, dx
    mov si, 18
    div si                      ; ax = lba / 18, dx = lba % 18
    inc dl
    mov cl, dl                  ; sector (1-based)
    xor dx, dx
    mov si, 2
    div si                      ; ax = cylinder, dx = head
    mov ch, al
    mov dh, dl
    mov dl, [boot_drive]
    mov di, 4                   ; retries
.retry:
    mov ax, 0x0201
    int 0x13
    jnc .ok
    xor ax, ax                  ; reset drive and try again
    int 0x13
    dec di
    jnz .retry
    jmp disk_error
.ok:
    add bx, 512
    jnz .same_seg
    mov ax, es
    add ax, 0x1000
    mov es, ax
.same_seg:
    inc word [lba]
    mov si, msg_dot
    call print
    pop cx
    loop .load

    ; ---- enable A20 (fast gate) ----
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al

    ; ---- protected mode ----
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pmode

disk_error:
    mov si, msg_err
    call print
.hang:
    hlt
    jmp .hang

print:                          ; ds:si -> zero-terminated string
    pusha
.next:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .next
.done:
    popa
    ret

[bits 32]
pmode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp 0x08:0x10000

; ---- data ----
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; 0x08 code, flat 4 GiB
    dq 0x00CF92000000FFFF       ; 0x10 data, flat 4 GiB
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

boot_drive  db 0
lba         dw 0
msg_loading db "Loading EmberOS", 0
msg_dot     db ".", 0
msg_err     db 13, 10, "Disk read error.", 0

times 510 - ($ - $$) db 0
dw 0xAA55
