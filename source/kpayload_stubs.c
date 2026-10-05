/*
 * GoldHEN v2.4b18.9 - Kpayload / Runtime Stubs
 *
 * These functions are normally provided at runtime either by:
 *   - The kpayload (kern_proc_load_module, kern_proc_read_mem, etc.)
 *   - An external library (gh_unzip)
 *
 * For the standalone build we provide weak stub implementations that
 * fail gracefully.  When GoldHEN runs on a real PS4, the kpayload will
 * have already installed the real implementations via syscall hooks or
 * by patching the GOT of this ELF.
 *
 * Marked __attribute__((weak)) so the linker prefers a real definition
 * if one is provided.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>

/*
 * kern_proc_load_module
 * Loads a PRX module into the address space of process `pid`.
 * Provided by the kpayload via a patched syscall (syscall 573 / dynlib_load_prx).
 */
__attribute__((weak))
int kern_proc_load_module(pid_t pid, const char *path)
{
    (void)pid; (void)path;
    fprintf(stderr, "[GoldHEN] kern_proc_load_module: kpayload not installed\n");
    return -1;
}

/*
 * gh_unzip
 * Extracts a ZIP archive to a directory.
 * A real implementation using miniz or libarchive is linked in by
 * the goldhen plugin build when the extra library is available.
 */
__attribute__((weak))
int gh_unzip(const char *zip_path, const char *dest_dir)
{
    (void)zip_path; (void)dest_dir;
    fprintf(stderr, "[GoldHEN] gh_unzip: not implemented\n");
    return -1;
}
