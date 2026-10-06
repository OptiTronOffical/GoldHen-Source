2ND SOURCE LEAK - OPENHEN https://github.com/OptiTronOffical/OpenGoldHEN

COLLECTED FROM: https://x.com/Sonic_Iso/status/2107240372782928089?s=20

NOT MINE


# GoldHEN v2.4b18.9 — Community Source Reconstruction

## About

This is a community-reconstructed source code for **GoldHEN v2.4b18.9** for PS4 firmware 10.01, built using the [ps4-payload-sdk](https://github.com/ps4sdk/ps4-payload-sdk) (orbis-clang).

GoldHEN was originally developed by **SiSTRo** as closed-source software. This reconstruction was produced for **research and educational purposes only**, to give the PS4 homebrew community insight into how GoldHEN works and to preserve knowledge of the implementation for FW 10.01.

**There are no plans to continue this to newer versions of GoldHEN.**

---

## What This Is

A reverse-engineered source reconstruction derived from:
- Live memory analysis of GoldHEN v2.4b18.9 running on PS4 FW 10.01
- Static analysis of the embedded kernel module (inner ELF)
- String and symbol extraction from the compiled binary

The official `goldhen.bin` and `goldhen_private.prx` binaries are embedded as C header arrays (`goldhen_bin.h` / `goldhen_private_prx.h`) and executed at runtime. All kernel patches and hooks are from the official binary.

---

## What Is Implemented

- Jailbreak (ucred, prison, rootvnode patching)
- FakeSelf / FakePKG hooks (via goldhen.bin)
- Kernel debug patches (debug menu enable, DIPSW bits)
- ShellCore / ShellUI / libkernel_sys patches
- SceRemotePlay patches
- FTP server (port 2122)
- BinLoader / payload loader (port 9022)
- Kernel log server with ring buffer (port 3233)
- Settings XML registration in SceShellUI
- goldhen_private.prx deployment and exec hook installation
- XMB icon with full GoldHEN settings menu
- Game overlay (FPS/CPU/RAM) via goldhen_private.prx
- Config.ini support (/data/GoldHEN/config.ini)

---

## Building

Requirements: [ps4-payload-sdk](https://github.com/ps4sdk/ps4-payload-sdk)

```bash
orbis-clang -O1 -std=c11 -Isource -Ilibs/tiny-json -Ilibs/tconfig \
  source/main.c source/config.c source/klog.c source/ftp.c \
  source/binloader.c source/cheats.c source/settings_xml.c \
  source/bd_patch.c source/update.c source/kpayload_install.c \
  source/kpayload_stubs.c source/plugin_installer.c \
  libs/tiny-json/tiny-json.c libs/tconfig/tconfig.c \
  -lkernel -lpthread -lSceNet -lSceHttp -lc \
  -o goldhen.elf
```

---

## Disclaimer

This project is for **research and educational purposes only**. All credit for GoldHEN goes to **SiSTRo** and the original GoldHEN development team. This reconstruction does not claim to be an original work — it is a community effort to document and understand the existing implementation.

Use at your own risk.
