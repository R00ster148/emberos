#!/bin/sh
# Builds EmberOS into build/emberos.iso (bootable CD) and build/emberos.img (1.44 MB floppy).
# Needs: nasm, gcc (with -m32 support), ld, ar, python3 + pycdlib (pip install pycdlib)
# Optional: EXTRA_CA=file.pem adds extra trusted root certificates (used for local testing).
set -e
cd "$(dirname "$0")"
mkdir -p build/bearssl

GI=$(gcc -m32 -print-file-name=include)
BASE="-m32 -march=i686 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
 -fno-asynchronous-unwind-tables -mgeneral-regs-only -nostdinc -Ikernel/libc -isystem $GI -O2"
BRDEF="-DBR_NO_INTRINSICS -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 \
 -DBR_USE_WIN32_TIME=0 -DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -DBR_INT128=0 -DBR_UMUL128=0"

# BearSSL (TLS library) -> build/libbearssl.a, rebuilt only if missing
if [ ! -f build/libbearssl.a ]; then
    echo "building BearSSL..."
    for f in third_party/bearssl/*.c; do
        gcc $BASE $BRDEF -Ithird_party/bearssl -c "$f" -o "build/bearssl/$(basename "${f%.c}").o"
    done
    ar rcs build/libbearssl.a build/bearssl/*.o
fi

# Trusted root certificates (Mozilla list, plus EXTRA_CA if given)
python3 tools/mkta.py tools/mozilla-cacert.pem $EXTRA_CA > kernel/trust_anchors.h 2>/dev/null

nasm -f elf32 kernel/entry.asm -o build/entry.o
nasm -f elf32 kernel/isr.asm   -o build/isr.o
for f in kernel net web; do
    gcc $BASE -Wall -Wextra -Wno-unused-parameter -c kernel/$f.c -o build/$f.o
done
gcc $BASE $BRDEF -Ithird_party/bearssl -Wall -c kernel/tls.c -o build/tls.o
ld -m elf_i386 -T kernel/linker.ld --oformat binary -o build/kernel.bin \
   build/entry.o build/isr.o build/kernel.o build/net.o build/web.o build/tls.o build/libbearssl.a
ld -m elf_i386 --no-warn-rwx-segments -T kernel/linker.ld -o build/kernel.elf \
   build/entry.o build/isr.o build/kernel.o build/net.o build/web.o build/tls.o build/libbearssl.a

KSIZE=$(stat -c %s build/kernel.bin)
KSECTORS=$(( (KSIZE + 511) / 512 ))
BSS_END=$(nm build/kernel.elf | awk '/ __bss_end$/ {print $1}')
echo "kernel: $KSIZE bytes ($KSECTORS sectors), ends at 0x$BSS_END"
if [ $((0x$BSS_END)) -ge $((0x80000)) ]; then echo "ERROR: kernel too big for low memory"; exit 1; fi

nasm -f bin -DKSECTORS=$KSECTORS boot/boot.asm -o build/boot.bin

cat build/boot.bin build/kernel.bin > build/emberos.img
truncate -s 1474560 build/emberos.img

python3 - <<'PYEOF'
import pycdlib
iso = pycdlib.PyCdlib()
iso.new(interchange_level=1, vol_ident='EMBEROS')
iso.add_file('build/emberos.img', '/EMBEROS.IMG;1')
iso.add_eltorito('/EMBEROS.IMG;1', bootcatfile='/BOOT.CAT;1', media_name='floppy')
iso.write('build/emberos.iso')
iso.close()
PYEOF
echo "built build/emberos.iso and build/emberos.img"
