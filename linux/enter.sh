#!/bin/sh
# Run a command inside the chroot with /proc, /sys, /dev mounted. Proxy only via env (never written to disk).
R=/home/claude/emberlinux/chroot
for m in proc sys dev dev/pts run; do mountpoint -q $R/$m || mount --bind /$m $R/$m; done
cp /etc/resolv.conf $R/etc/resolv.conf 2>/dev/null
chroot $R /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive \
  http_proxy="$HTTP_PROXY" https_proxy="$HTTPS_PROXY" /bin/sh -c "$*"
