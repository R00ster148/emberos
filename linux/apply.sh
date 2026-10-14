#!/bin/bash
# Applies EmberOS branding and configuration to the chroot. Safe to re-run.
set -e
B=/home/claude/emberlinux; R=$B/chroot; A=$B/art/out
cp -a $B/overlay/. $R/
cp $B/overlay/etc/xdg/xfce4/panel-default.xml $R/etc/xdg/xfce4/panel/default.xml
# Artwork: flat logo icons; wallpaper is the "Clouds" community photo (CC BY-SA, Tibor Mokanszki)
for s in 16 24 32 48 64 96 128 256; do install -D -m644 $A/logo-$s.png $R/usr/share/icons/hicolor/${s}x${s}/apps/emberos.png; done
install -D -m644 $A/logo-128.png $R/usr/share/pixmaps/emberos.png
rm -f $R/usr/share/backgrounds/emberos.png
python3 - "$R" <<'PY'
import sys
from PIL import Image
R = sys.argv[1]
im = Image.open(R + '/usr/share/backgrounds/Clouds_by_Tibor_Mokanszki.jpg').convert('RGB')
im.resize((2560, 1440), Image.LANCZOS).save(R + '/usr/share/backgrounds/emberos-default.jpg', quality=90)
im.resize((1920, 1080), Image.LANCZOS).save('/tmp/emberos-wall-1080.jpg', quality=85)
PY
mkdir -p $R/usr/share/emberos
printf 'Default wallpaper: "Clouds" by Tibor Mokanszki, CC BY-SA 4.0,\nfrom the Ubuntu 24.04 community wallpaper collection.\n' > $R/usr/share/emberos/CREDITS
# Default xfdesktop wallpaper -> ours (SVG wrapper around the JPEG), via dpkg-divert so updates don't undo it
S=/usr/share/backgrounds/xfce/xfce-shapes.svg
chroot $R dpkg-divert --quiet --local --rename --add $S || true
python3 - "$R$S" /tmp/emberos-wall-1080.jpg <<'PY'
import base64, sys
b = base64.b64encode(open(sys.argv[2], 'rb').read()).decode()
open(sys.argv[1], 'w').write('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="1920" height="1080" viewBox="0 0 1920 1080"><image width="1920" height="1080" xlink:href="data:image/jpeg;base64,%s"/></svg>' % b)
PY
# Boot splash: flat logo + small wordmark
P=$R/usr/share/plymouth
install -m644 $A/watermark.png $P/themes/spinner/watermark.png
install -m644 $A/splash-logo.png $P/themes/spinner/bgrt-fallback.png
install -m644 $A/splash-logo.png $P/ubuntu-logo.png
# EmberOS Classic disk image
install -D -m644 /home/claude/emberos/build/emberos.iso $R/usr/share/emberos/emberos-classic.iso
# Name and version
sed -i -e 's/^NAME=.*/NAME="EmberOS"/' -e 's/^PRETTY_NAME=.*/PRETTY_NAME="EmberOS 3.0 (based on Ubuntu 24.04 LTS)"/' $R/usr/lib/os-release
grep -q '^VARIANT=' $R/usr/lib/os-release || echo 'VARIANT="EmberOS 3.0"' >> $R/usr/lib/os-release
sed -i 's/^DISTRIB_DESCRIPTION=.*/DISTRIB_DESCRIPTION="EmberOS 3.0"/' $R/etc/lsb-release
printf 'EmberOS 3.0 \\n \\l\n\n' > $R/etc/issue
# Windows key opens the Start menu
K=$R/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-keyboard-shortcuts.xml
grep -q 'name="Super_L"' $K || sed -i '0,/<property name="default" type="empty">/s//&\n      <property name="Super_L" type="string" value="xfce4-popup-whiskermenu"\/>/' $K
# VirtualBox-friendly WebKit rendering
grep -q WEBKIT_DISABLE_DMABUF_RENDERER $R/etc/environment || echo 'WEBKIT_DISABLE_DMABUF_RENDERER=1' >> $R/etc/environment
# In-chroot steps
$B/enter.sh '
set -e
sed -i "s/^# *\(en_GB.UTF-8\)/\1/; s/^# *\(en_US.UTF-8\)/\1/" /etc/locale.gen
locale-gen >/dev/null
ln -sf /usr/share/zoneinfo/Europe/London /etc/localtime; echo Europe/London > /etc/timezone
glib-compile-schemas /usr/share/glib-2.0/schemas
gtk-update-icon-cache -q -f /usr/share/icons/hicolor
update-desktop-database -q /usr/share/applications || true
update-initramfs -u -k all >/dev/null 2>&1
'
echo "apply ok"
