# EmberOS

Two editions:

- **EmberOS 3.0 (Linux edition)** - a Windows-style desktop built on Ubuntu 24.04: Start menu and
  taskbar, GNOME Web browser with video codecs, one-click real Google Chrome install, "Save My Files"
  persistence, and EmberOS Classic built in. **Download:** Code > Download ZIP, unzip, open the `download` folder and double-click `JOIN-ME.bat` - it builds `emberos-3.0.iso` (761 MB).
- **EmberOS Classic** (`classic/`) - the original OS written from scratch: own bootloader and kernel,
  network drivers, TCP/IP, HTTPS (BearSSL), text web browser, Snake.

## Run EmberOS 3.0 in VirtualBox

1. New VM: Type **Linux**, Version **Ubuntu (64-bit)**. Memory **4096 MB**, 2 CPUs.
2. Create a **20 GB** virtual hard disk (used by *Save My Files* to keep your stuff).
3. Settings > Display: Graphics Controller **VMSVGA**, Video Memory 128 MB.
4. Settings > Storage: attach `emberos-3.0.iso` to the optical drive.
5. Start. On the desktop, open **Save My Files** once, then restart, so Chrome and your files are kept.

## Building

`linux/` holds the build scripts (debootstrap Ubuntu noble, apply overlay, mksquashfs, grub-mkrescue).
`classic/build.sh` builds EmberOS Classic.
