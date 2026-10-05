Based on the analysis of the provided source code and the current PS4 jailbreak scene, making the `GoldHen-Source` (a community reconstruction for firmware 10.01) compatible with firmware 13.52 is a non-trivial task. The core challenge is that the kernel memory offsets are different for each firmware version. The project is built using the `ps4-payload-sdk`.

To achieve 13.52 compatibility, you need to update the hardcoded kernel offsets in the source code with the correct values for that firmware. Fortunately, these offsets have been publicly documented by the community.

Here is a step-by-step guide to porting the source.

### 🔍 Step 1: Locate the Offset Definitions in the Source

The `GoldHen-Source` project is a C codebase. The firmware-specific offsets are typically defined in a header file, such as `source/kernel.h` or `source/goldhen.h`. You will need to inspect these files to find the `#define` statements that hold the offset values for the currently supported firmware (10.01).

Commonly, these offsets are used for critical kernel structures like `ucred` (for privilege escalation), `prison0` (for sandbox escape), and `rootvnode` (for mounting filesystems). The names of the variables in the source will likely be similar to the keys used in the public offset databases.

### 📋 Step 2: Obtain the Correct 13.52 Offsets

The community has published the required offsets for firmware 13.52. The most reliable source for these is the `ps4_offsets.js` file from the **raw13g/raw13g.github.io** repository, which is part of the publicly released RAW GAME exploit chain.

Below is a table of the critical kernel offsets for firmware **13.52**, extracted from that file.

| Offset Key | 13.52 Value |
| :--- | :--- |
| `k_idt_rsvd` | `0x1c1e00` |
| `k_oid_kern_file` | `0x1a2f8a0` |
| `k_oid_maxfilesperproc` | `0x1a2f950` |
| `k_oid_maxprocperuid` | `0x1a3ba88` |
| `k_oid_maxfiles` | `0x1a2f9a8` |
| `k_arg1_maxfilesperproc` | `0x22cc47c` |
| `k_arg1_maxprocperuid` | `0x22cc478` |
| `k_arg1_maxfiles` | `0x22cc474` |
| `k_sysctl_handle_int` | `0x3fa8e0` |
| `k_prison0` | `0x1a5c0c0` |
| `k_rootvnode` | `0x2136e90` |
| `k_sysent` | `0x1102b70` |
| `k_sysent_661` | `0x110a760` |
| `k_jmp_rsi` | `0x4d6d0` |
| `k_kl_lock` | `0xe6c60` |
| `k_evf_cv` | `0x785228` |

**Important:** The `kpatch` value should be set to `"1352.bin"` and the `payload` to `"goldhen.bin"`. The `patches/1352.bin` file contains the kernel patch blob for this firmware.

### ✏️ Step 3: Modify the Source Code

1.  Open the header file in the `source` directory where the offsets are defined.
2.  Find the `#define` statements for each of the offsets listed above. For example, you might see `#define K_IDT_RSVD 0x...`.
3.  Replace the existing values with the 13.52 values from the table.
4.  You may also need to update the `kpatch` and `payload` filenames in the code that loads these components.

### 🛠️ Step 4: Rebuild the Payload

After updating the offsets, you must rebuild the `goldhen.elf` file using the `ps4-payload-sdk`. The build command provided in the repository's README is:

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


### ⚠️ Important Considerations

*   **Kernel Panics:** Even with the correct offsets, early betas of GoldHEN for 13.52 have been reported to cause kernel panics on Rest Mode or system shutdown. This indicates that the porting process may require more than just offset updates, potentially involving changes to how the payload handles certain kernel operations.
*   **Alternative: Use a Pre-built Payload:** If your goal is simply to run GoldHEN on a 13.52 console and not to learn about the internals, the community has already released working versions. You can use the `goldhen.bin` file from the **raw13g/raw13g.github.io** repository, which is designed to work with the 13.52 firmware.
*   **Risk of Bricking:** Modifying kernel offsets and running custom payloads carries a risk of rendering your console unstable or inoperable. Proceed with caution and ensure you understand the risks.

In summary, to make this source compatible with 13.52, you need to replace the firmware-specific kernel offsets with the values from the table above and rebuild the payload using the `ps4-payload-sdk`.
