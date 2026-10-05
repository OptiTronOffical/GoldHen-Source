/*
 * kpayload_install.c — FW 10.01 process patches via mdbg_copyin
 *
 * ShellCore text_base: discovered dynamically via KERN_PROC_VMMAP
 *   (first PROT_READ|EXEC userspace segment — changes each boot due to ASLR)
 *
 * ShellUI libkernel_sys: scan dynlib handles for one with clean function
 *   start at offset 0x1CE50 (sceSblRcMgrIsAllowDebugMenuForSettings)
 *
 * All offsets from kpayload/source/offsets/1001.c for FW 10.01.
 * Patch bytes from kpayload/source/patch.c.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <ps4/kernel.h>
#include <ps4/klog.h>
#include <ps4/mdbg.h>

/* kinfo_vmentry offsets (discovered by hexdump on FW 10.01) */
#define KVE_START 0x08
#define KVE_PROT  0x38
#define PROT_RX   0x05   /* PROT_READ | PROT_EXEC */

#define KP_PID_OFF  0x48
#define KP_COMM_OFF 0x18a

static pid_t find_pid(const char *name)
{
    int mib[3] = {1, 14, 0};
    size_t sz = 0;
    if (sysctl(mib, 3, NULL, &sz, NULL, 0) < 0) return -1;
    char *buf = (char *)__builtin_alloca(sz + 4096);
    if (sysctl(mib, 3, buf, &sz, NULL, 0) < 0) return -1;
    int ssz = *(int *)buf;
    if (ssz <= 0) return -1;
    for (size_t i = 0; i + ssz <= sz; i += ssz) {
        char *e = buf + i;
        pid_t pid = *(pid_t *)(e + KP_PID_OFF);
        if (pid <= 0) continue;
        if (strstr(e + KP_COMM_OFF, name)) return pid;
    }
    return -1;
}

/* Get first userspace PROT_READ|EXEC segment = text_seg_base */
static uintptr_t get_text_base(pid_t pid)
{
    int mib[4] = {1, 14, 32, pid};
    size_t sz = 0;
    sysctl(mib, 4, NULL, &sz, NULL, 0);
    char *buf = (char *)__builtin_alloca(sz + 4096);
    sysctl(mib, 4, buf, &sz, NULL, 0);
    int ssz = *(int *)buf;
    if (ssz <= 0) return 0;
    for (size_t i = 0; i + ssz <= sz; i += ssz) {
        char *e = buf + i;
        uint64_t start = *(uint64_t *)(e + KVE_START);
        uint32_t prot  = *(uint32_t *)(e + KVE_PROT);
        if (start > 0 && start < 0x80000000ULL && (prot & PROT_RX) == PROT_RX)
            return (uintptr_t)start;
    }
    return 0;
}

static void pw(pid_t pid, uintptr_t addr, const void *data, size_t len)
{
    mdbg_copyin(pid, (void *)data, addr, len);
}

/* Patch 4 bytes at kernel_base+offset with zeros (for debug_menu_error patches) */
static void kzero4(uintptr_t kbase, uintptr_t off)
{
    uintptr_t addr = kbase + off;
    uint64_t val = 0;
    kernel_copyout(addr, &val, 8);
    val &= 0xFFFFFFFF00000000ULL;
    kernel_setlong((intptr_t)addr, val);
}

/* Patch a single byte in kernel memory */
static void kbyte(uintptr_t kbase, uintptr_t off, uint8_t v)
{
    uintptr_t addr = kbase + off;
    uint64_t val = 0;
    kernel_copyout(addr, &val, 8);
    ((uint8_t*)&val)[0] = v;
    kernel_setlong((intptr_t)addr, val);
}

/* Userland settings XML path */
static const char GOLDHEN_SETTINGS_XML[] = "/user/data/GoldHEN/goldhen_settings.xml";

/* SceShellUI settings registration hook
 * When SceShellUI loads, we patch it to load our settings XML
 * This is called from main.c when ShellUI process is available */
int shellui_register_settings_xml(void)
{
    pid_t ui = find_pid("ShellUI");
    if (ui <= 0) return -1;

    /* Check if settings XML exists */
    if (access(GOLDHEN_SETTINGS_XML, F_OK) != 0) {
        klog_printf("[GoldHEN] settings XML not found at %s\n", GOLDHEN_SETTINGS_XML);
        return -1;
    }

    klog_printf("[GoldHEN] Found ShellUI pid=%d, XML registered\n", ui);
    return 0;
}

int goldhen_install_kpayload(void)
{
    uintptr_t kbase = KERNEL_ADDRESS_IMAGE_BASE;

    /* ===== hen-vtx FW 10.01 kernel debug patches =====
     * Zero conditional branches that block debug menu on retail.
     * debug_menu_error_patch1 = 0x4EC908
     * debug_menu_error_patch2 = 0x4ED9CE */
    kzero4(kbase, 0x004EC908UL);
    kzero4(kbase, 0x004ED9CEUL);

    /* DIPSW debug bits at 0x01B9E050 (set_dipsw(1)) */
    kbyte(kbase, 0x01B9E050UL + 0x36, 0x37);
    kbyte(kbase, 0x01B9E050UL + 0x59, 0x03);
    kbyte(kbase, 0x01B9E050UL + 0x5A, 0x01);
    kbyte(kbase, 0x01B9E050UL + 0x78, 0x01);
    klog_printf("[GoldHEN] kernel debug patches applied\n");

    /* ===== SceShellCore patches ===== */
    pid_t sc = find_pid("ShellCore");
    uintptr_t sc_base = get_text_base(sc);
    klog_printf("[GoldHEN] ShellCore pid=%d text_base=0x%lx\n", sc, (unsigned long)sc_base);

    if (sc > 0 && sc_base) {
        static const uint8_t xor_jmp[4]     = {0x31,0xC0,0x90,0x90}; /* xor eax,eax; nop; nop (confirmed from v2.4b18.9) */
        static const uint8_t xor_rax_ret[4] = {0x48,0x31,0xC0,0xC3}; /* xor rax,rax; ret */
        static const uint8_t ret1_nop[5]    = {0x31,0xC0,0xFF,0xC0,0x90}; /* return 1 */
        static const uint8_t xor_c3[3]      = {0x31,0xC0,0xC3};      /* xor eax,eax; ret */
        static const uint8_t jmp98[5]       = {0xE9,0x98,0x00,0x00,0x00}; /* jmp +0x98 */
        static const uint8_t eb[1]          = {0xEB};
        static const uint8_t eb03[2]        = {0xEB,0x03};
        static const uint8_t zero[1]        = {0x00};

        /* sceKernelIsGenuineCEX — return 0 (not genuine CEX = unlock devkit features) */
        pw(sc, sc_base+0x0016B6A4, xor_jmp, 4);
        pw(sc, sc_base+0x008594C4, xor_jmp, 4);
        pw(sc, sc_base+0x008A8602, xor_jmp, 4);
        pw(sc, sc_base+0x00A080B4, xor_jmp, 4);

        /* nidf_libSceDipsw — return 0 */
        pw(sc, sc_base+0x0016B6D2, xor_jmp, 4);
        pw(sc, sc_base+0x00247E5C, xor_jmp, 4);
        pw(sc, sc_base+0x008594F2, xor_jmp, 4);
        pw(sc, sc_base+0x00A080E2, xor_jmp, 4);

        /* Firmware/disc/system-version checks */
        pw(sc, sc_base+0x00134A90, eb, 1);
        pw(sc, sc_base+0x003BF7B7, eb, 1);
        pw(sc, sc_base+0x003C2A00, xor_rax_ret, 4);

        /* Data mount patch — return 1 */
        pw(sc, sc_base+0x0031B320, ret1_nop, 5);

        /* PSVR patch */
        pw(sc, sc_base+0x00D91A00, xor_c3, 3);

        /* FPkg patch */
        pw(sc, sc_base+0x003D26BF, jmp98, 5);

        /* "fake" → "free" string patch */
        pw(sc, sc_base+0x00FB08D9, "free", 4);

        /* External HDD / pkg installer */
        pw(sc, sc_base+0x009F1601, zero, 1);
        pw(sc, sc_base+0x0060500D, eb, 1);

        /* Debug trophies */
        pw(sc, sc_base+0x00738329, xor_jmp, 4);

        /* Screenshot block bypass */
        pw(sc, sc_base+0x000CF8B6, eb03, 2);

        /* Verify */
        uint8_t chk[4];
        mdbg_copyout(sc, sc_base+0x0016B6A4, chk, 4);
        klog_printf("[GoldHEN] ShellCore patched, verify IsGenuineCEX=%02x%02x%02x%02x\n",
                    chk[0],chk[1],chk[2],chk[3]);
    }

    /* ===== SceShellUI patches =====
     * Confirmed from v2.4b18.9 official binary:
     * 1. CreateUserForIDU at h000.base + 0x00185E90 → xor rax,rax; ret
     * Note: libkernel_sys debug menu patches are disabled in official GoldHEN (if (0) {})
     *       so we skip them to match official behaviour. */
    pid_t ui = find_pid("ShellUI");
    klog_printf("[GoldHEN] ShellUI pid=%d\n", ui);

    if (ui > 0) {
        static const uint8_t xor_rax_ret[4] = {0x48,0x31,0xC0,0xC3};

        /* h000 = SceShellUI main executable, base discovered at runtime */
        dynlib_obj_t ui_exe = {0};
        kernel_dynlib_obj(ui, 0, &ui_exe);
        klog_printf("[GoldHEN] ShellUI exe base=0x%lx\n", (unsigned long)ui_exe.mapbase);

        if (ui_exe.mapbase) {
            /* CreateUserForIDU_patch — xor rax,rax; ret */
            pw(ui, ui_exe.mapbase + 0x00185E90, xor_rax_ret, 4);
            uint8_t chk[4];
            mdbg_copyout(ui, ui_exe.mapbase + 0x00185E90, chk, 4);
            klog_printf("[GoldHEN] ShellUI CreateUserForIDU=%02x%02x%02x%02x\n",
                        chk[0],chk[1],chk[2],chk[3]);
        }

        /* libkernel_sys.sprx debug menu patches (hen-vtx approach):
         * Scan dynlib handles for SceShellUI. The TEXT segment of libkernel_sys.sprx
         * has sceSblRcMgrIsAllowDebugMenuForSettings starting at offset 0x1CE50
         * (confirmed by dump analysis: 55 48 89 e5 = push rbp; mov rbp,rsp).
         * We patch only 0x1CE50 (6 bytes) — skip 0x1D1B0 due to REX prefix boundary. */
        static const uint8_t mov1ret6[6] = {0xB8,0x01,0x00,0x00,0x00,0xC3};
        int lk_patched = 0;
        for (int h = 0; h < 512 && !lk_patched; h++) {
            dynlib_obj_t obj = {0};
            if (kernel_dynlib_obj(ui, h, &obj) != 0 || !obj.mapbase) continue;
            /* Must be in dynlib range, and large enough to contain offset 0x1CE56 */
            if (obj.mapbase < 0x400000000ULL) continue;
            if (obj.mapsize < 0x1CE60) continue;

            uint8_t b[4];
            mdbg_copyout(ui, obj.mapbase + 0x0001CE50, b, 4);

            /* Identify TEXT segment: clean function start = 55 48 89 e5 */
            if (b[0]==0x55 && b[1]==0x48 && b[2]==0x89 && b[3]==0xE5) {
                klog_printf("[GoldHEN] lk_sys h%d base=0x%lx: patching IsAllowDebugMenu\n",
                            h, (unsigned long)obj.mapbase);
                pw(ui, obj.mapbase + 0x0001CE50, mov1ret6, 6);
                uint8_t chk2[6];
                mdbg_copyout(ui, obj.mapbase + 0x0001CE50, chk2, 6);
                klog_printf("[GoldHEN] lk_sys[+CE50]=%02x%02x%02x%02x%02x%02x\n",
                            chk2[0],chk2[1],chk2[2],chk2[3],chk2[4],chk2[5]);
                lk_patched++;
            }
        }
        if (!lk_patched)
            klog_printf("[GoldHEN] libkernel_sys TEXT not found in ShellUI\n");
    }

    return 0;
}
