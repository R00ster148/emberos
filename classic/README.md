# EmberOS

A tiny 32-bit x86 hobby OS: custom bootloader, protected-mode C kernel, interrupt-driven
keyboard and timer, RTC clock, RAM filesystem, a shell, Snake, and a text web browser that
speaks HTTP and HTTPS by itself.

## Run in VirtualBox

1. **New VM** → Type: `Other`, Version: `Other/Unknown` (32-bit). Give it **64 MB RAM** (the web needs at least 16 MB). No hard disk needed.
2. **Settings → Storage** → click the empty optical drive → choose `emberos.iso`.
3. **Settings → Network** → Adapter 1 enabled, attached to **NAT** (the default). Adapter type can be
   `PCnet-FAST III` (the default for this VM type) or `Intel PRO/1000 MT Desktop`.
4. Make sure **Settings → System → Enable EFI** is *off*.
5. Start the VM.

## Commands

| command | what it does |
|---|---|
| `search <words>` | search the web (DuckDuckGo, falling back to Mojeek, then Wiby) |
| `open <address>` | open a web page, e.g. `open en.wikipedia.org` (https:// is assumed) |
| `engine [ddg\|mojeek\|wiby]` | choose the search engine |
| `net` | show network info; `net dns <ip>` sets a DNS server by hand |
| `nettest` | check which sites your connection can reach, over HTTP and HTTPS |
| `help` `info` `clear` `echo` `time` `uptime` `calc` `color` | the basics |
| `ls` `cat` `write` `append` `rm` | files (kept in RAM) |
| `snake` `reboot` `shutdown` | |

In the browser: arrows / PgUp / PgDn scroll, type a link number + Enter to follow it,
**B** goes back, **S** starts a new search, **Q** quits. Esc cancels a download.

## How the web works

EmberOS has its own network stack: Intel e1000 and AMD PCnet drivers, ARP, IPv4, UDP, DHCP,
DNS, TCP and an HTTP/1.0 client. HTTPS uses **BearSSL 0.6** (TLS 1.0-1.2) on top of that TCP
stack. Server certificates are checked against the **Mozilla root certificate list** (121 roots,
from `certifi`), including the site name and expiry date, using the VM's clock. Random numbers
for the encryption come from the CPU's RDRAND instruction when available, mixed with timing
jitter.

Limits: no TLS 1.3 (the few sites that require it will refuse), no JavaScript, no images,
no cookies or forms. Pages are shown as text with numbered links.

## Build

Needs `nasm`, `gcc` (with `-m32`), `ld`, `ar`, and Python 3 with `pycdlib` and `cryptography`
(`pip install pycdlib cryptography`).

    ./build.sh

Outputs `build/emberos.iso` and `build/emberos.img` (a floppy image you can attach to a floppy
controller instead). Set `EXTRA_CA=file.pem` to trust an extra root certificate, e.g. for testing
against your own local HTTPS server.

## Layout

    boot/boot.asm          boot sector: loads kernel to 0x10000, enables A20, enters protected mode
    kernel/entry.asm       kernel entry: stack, zero .bss, call kmain
    kernel/isr.asm         IRQ and CPU exception stubs
    kernel/kernel.h        shared declarations and the memory map
    kernel/kernel.c        console, IDT/PIC/PIT, keyboard, RTC, RAM fs, shell, snake
    kernel/net.c           PCI, e1000 + PCnet drivers, ARP/IP/UDP/DHCP/DNS/TCP, HTTP client
    kernel/tls.c           HTTPS: BearSSL glue (entropy, clock, certificate checks)
    kernel/web.c           HTML-to-text renderer, page viewer, search
    kernel/libc/           the two C headers BearSSL needs
    kernel/linker.ld       links the kernel at 0x10000 as a flat binary
    third_party/bearssl/   BearSSL 0.6 (MIT licence) - see BEARSSL_LOCAL_CHANGES.txt
    tools/mkta.py          turns a PEM certificate bundle into kernel/trust_anchors.h
    tools/mozilla-cacert.pem   Mozilla's root certificates (from certifi 2026.7.22)
