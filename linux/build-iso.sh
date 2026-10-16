#!/bin/bash
# Packs the chroot into a hybrid BIOS/UEFI live ISO: out/emberos-3.1.iso
set -e
B=/home/claude/emberlinux; R=$B/chroot; I=$B/iso
K=$(ls $R/boot/vmlinuz-* | sort -V | tail -1 | sed 's|.*/vmlinuz-||')
# Clean the system image (no package lists, caches, logs, machine identity, or build-host settings)
$B/enter.sh 'apt-get clean; rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /root/install.log; : > /etc/machine-id; rm -f /var/lib/dbus/machine-id; find /var/log -type f -delete'
for m in run dev/pts dev sys proc; do umount -l $R/$m 2>/dev/null || true; done
rm -f $R/etc/resolv.conf
ln -s ../run/systemd/resolve/stub-resolv.conf $R/etc/resolv.conf   # standard Ubuntu: systemd-resolved, fed by NetworkManager
grep -rqsiE 'proxy|42977' $R/etc/apt $R/etc/environment $R/etc/profile.d && { echo "ERROR: proxy setting found in image"; exit 1; }
cp $R/boot/vmlinuz-$K $I/casper/vmlinuz
cp $R/boot/initrd.img-$K $I/casper/initrd
rm -f $I/casper/filesystem.squashfs
mksquashfs $R $I/casper/filesystem.squashfs -noappend -comp zstd -Xcompression-level 19 -b 1M -processors 2 -no-progress \
  -wildcards -e 'proc/*' 'sys/*' 'dev/*' 'run/*' 'tmp/*' >/dev/null
du -sx --block-size=1 $R | cut -f1 > $I/casper/filesystem.size
mkdir -p $B/out
grub-mkrescue -o $B/out/emberos-3.1.iso $I -- -volid EMBEROS_31 2>&1 | grep -v "^xorriso : UPDATE\|^Drive current\|^Media current\|^Media status\|^Media summary\|^Added to ISO\|^xorriso : NOTE" || true
ls -la $B/out/emberos-3.1.iso
