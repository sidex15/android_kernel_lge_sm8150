# LG V50 (flash) kernel — with V50s/G8x Dual Screen (DS2) support

This tree adds support for the **2nd-generation LG Dual Screen (DS2)** — the
USB-C accessory that ships with the LG V50s / G8X — on the **LG V50 ("flash",
LM-V500N / `flash_lao_kr`)**, which natively ships only the 1st-generation
cover display.

## What Fully works

- **Display** — the DS2 comes up as a DP-1 external display (1080×2340).
- **Touch** — the DS2's USB-HID multitouch (`VID 0x1004 PID 0x637a`, "LMV515N").
- **Folding** — folding the case over/behind toggles the DS2 on/off correctly,
  with no kernel panic.

## Known limitations

- **Cover-display AOD (folded clock/notification window):** *not supported.*
  On the V50s the DS2 panel (Renesas **r66456**) is driven as a secondary **DSI**
  display, which is what gives it the low-power AOD window. On the V50 the DS2 is
  only reachable as an **external DP display** — AOD/LP2 are DSI-panel-only
  features with no channel over DP. This is a board/architecture limitation, not
  a software toggle.
- **Quick detach → re-attach:** re-attaching within a few seconds of a detach can
  fail to re-enumerate, because the DS2's own MCU does not cold-boot fast enough
  and goes unresponsive to both USB and PD. Wait a bit longer between
  detach/attach, or reboot. This is a DS2 firmware/hardware limit.
- **Charging** — a charger attached while the DS2 kills
  the accessory; charging works when the DS2 is detached or not connected (standard USB-C/QC).
- **Replug** — unplugging and replugging does not work... attempts are done in this commit (https://github.com/sidex15/android_kernel_lge_sm8150/commit/137336b94f273477cd0035818ca40d120ca87fec)

## How it works (architecture)

The DS2 is a USB-C accessory: **DP alt-mode** video (through the DS2's internal
Analogix **ANX7530** DP→DSI bridge driving its r66456 panel) plus a **USB-HID**
touch/MCU device. Power comes from **VCONN (6W)**. The DS2 speaks no real USB-PD,
so `drivers/usb/misc/lge_ds2.c` registers a PD **emulator** that fakes the DP
alt-mode negotiation with the policy engine.

On the V50, DP AUX and lanes are routed through an **on-board iCE40 FPGA
crossbar** (selected by GPIO 67); the DS2's captive plug has its SBU pair
cross-wired, so the AUX/orientation is inverted relative to a normal plug.

### On the Replugging attempt
The single biggest quirk on the V50: the DS2's captive plug is read by the PMIC
as an **unoriented debug accessory (`SINK_DEBUG_ACCESSORY`, Rd/Rd)** on replug,
where a native V50s reads it as `SINK_POWERED_CABLE` (Rd/Ra). That single
misclassification originally cascaded into moisture-detection lockout, UART
debug-cable takeover of the SBU, USB-ID contention, no source/host mode, and no
VCONN. All of those paths are now gated on the DS2 being physically present
(hall / VPD active) so real debug cables and moisture detection stay untouched.

## Building

```sh
# toolchain: LLVM/clang (weebx-clang15 used during bring-up)
export PATH=~/toolchains/weebx-clang15/bin:$PATH

# drivers/kernelsu is a symlink to your KernelSU checkout; recreate if needed:
ln -sf ../KernelSU/kernel drivers/kernelsu

make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump \
     STRIP=llvm-strip O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     CLANG_TRIPLE=aarch64-linux-gnu- vendor/dragon_flash_defconfig
make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump \
     STRIP=llvm-strip O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     CLANG_TRIPLE=aarch64-linux-gnu- -j$(nproc)
```

Key config: `CONFIG_MACH_SM8150_FLASH=y`, `CONFIG_LGE_DUAL_SCREEN=y`,
`CONFIG_LATTICE_ICE40=y`, `CONFIG_LGE_COVER_DISPLAY=n`. DT changes (the
`lge_usb_ds2` node) live in the DTBO overlays, so flash the rebuilt `dtbo.img`
as well as the kernel `Image` when the device tree changes.

## Runtime tunables

Module parameters (under `/sys/module/.../parameters/`) for the DS2 path:

| Parameter | Module | Default | Purpose |
|---|---|---|---|
| `ds2_auto_hpd` | `lge_ds2` | on (flash) | fake HAL-ready where the ROM lacks the dualscreen HAL sysfs |
| `ds2_hallic_keep_alive` | `lge_ds2` | on | don't tear down a DS2 that's still alive on a transient CC glitch |
| `ds2_vconn_recovery_time_ms` | `lge_ds2` | 1500 | VCONN off-dwell during a recovery cold-reset |
| `ds2_hardcoded_edid` | `msm_drm` | on | serve the known-good DS2 EDID instead of the flaky AUX read |
| `ds2_en_unoriented_dbg_src` | `qpnp-smb5` | on | accept the DS2's unoriented Rd/Rd as a source attach |

## AI Disclosure

This port was assisted by Claude Fable 5 and Claude Opus 4.8, The author role in this port is a QA and code review, and providing logs to provide context to the AI.

---

Linux kernel
============

This file was moved to Documentation/admin-guide/README.rst

Please notice that there are several guides for kernel developers and users.
These guides can be rendered in a number of formats, like HTML and PDF.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.

There are various text files in the Documentation/ subdirectory,
several of them using the Restructured Text markup notation.
See Documentation/00-INDEX for a list of what is contained in each file.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.
