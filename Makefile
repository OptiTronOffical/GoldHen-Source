#
# GoldHEN v2.4b18.9 - Goldhen Plugin Makefile
# Builds goldhen.elf (PS4 payload ELF) using the ps4-payload-sdk toolchain.
#
# Usage:
#   PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk/ps4-payload-sdk make
#

ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined. Set it to /opt/ps4-payload-sdk/ps4-payload-sdk)
endif

# ---- Output ----
ELF     := goldhen.elf

# ---- Source directories ----
SDIR    := source
LIBDIR  := ../libs

# ---- Source files ----
SRCS := \
    $(SDIR)/main.c             \
    $(SDIR)/config.c           \
    $(SDIR)/klog.c             \
    $(SDIR)/ftp.c              \
    $(SDIR)/binloader.c        \
    $(SDIR)/cheats.c           \
    $(SDIR)/settings_xml.c     \
    $(SDIR)/bd_patch.c         \
    $(SDIR)/update.c           \
    $(SDIR)/kpayload_install.c \
    $(SDIR)/kpayload_stubs.c   \
    $(LIBDIR)/tiny-json/tiny-json.c \
    $(LIBDIR)/tconfig/tconfig.c

# ---- Include paths ----
INCDIRS := \
    -I$(SDIR) \
    -I$(LIBDIR)/tiny-json \
    -I$(LIBDIR)/tconfig

# ---- Compile flags ----
CFLAGS := \
    -O2 \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -std=c11 \
    $(INCDIRS)

# ---- Link flags / libraries ----
# libkernel  : kernel_copyout/copyin, kernel_get_fw_version, etc.
# libpthread : pthread_create, pthread_mutex_*
# libSceNet  : socket, bind, listen, accept, send, recv
# libSceHttp : sceHttpInit, sceHttpSendRequest, etc.
# libc       : standard C library
LDFLAGS := \
    -lkernel \
    -lpthread \
    -lSceNet \
    -lSceHttp \
    -lc

# ---- Build rules ----
.PHONY: all clean

all: $(ELF)

$(ELF): $(SRCS)
	@echo "[GoldHEN] Compiling $@..."
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	@echo "[GoldHEN] Built: $@"

clean:
	@rm -f $(ELF)
	@echo "[GoldHEN] Cleaned."
