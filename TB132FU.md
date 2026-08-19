# Lenovo TB132FU kernel

This repository contains the source history for the Lenovo TB132FU kernel
bring-up and Android compatibility work. The current boot-validated source
checkpoint is Linux 4.19.325-cip135-st19 with the TB132FU vendor hardware
support retained.

## Pure Pixel 17 R1 checkpoint

The Android 17 R1 release uses kernel build 13 from immutable implementation
commit `5b0f70f39eff047774cf24d61301f92506330113`. Release-source tag
`tb132fu-purepixel17-r1-k13` adds only the reproducible defconfig and public
documentation. Its kernel release string is `4.19.325-cip135-st19+`.

The exact configuration is published as
`arch/arm64/configs/tb132fu_android17_defconfig`. With Android Clang r383902,
it expands to the accepted `.config` SHA-256
`6c6920de38ca9d55e1a7b4cbc74fc0cdbb9c5ab4b402c5fbec35201dfa7c5f43`.
The accepted module ABI contains 12,233 `Module.symvers` entries and has
SHA-256 `75ba5cd713a4a4d0ec2472bdfd67e5639ee3eb72a814394c2099d427c829269a`.

Pure Pixel 17 R1 physically passed boot, repeated suspend/wake, full battery
depletion and unattended charger recovery, display and touch, Goodix pen,
pogo keyboard/trackpad/lid, Wi-Fi, Bluetooth, front/rear cameras, flash/torch,
four-speaker audio, microphone, haptics, 60/120 Hz operation, adaptive
brightness, AOD/DT2W, and hardware media decode qualification on TB132FU_PRC.
The kernel is shared by the PRC and ROW product contract, but a native ROW
tablet has not completed the same physical qualification matrix.

## Earlier tested checkpoints

| Tag | Kernel | Status |
| --- | --- | --- |
| `tb132fu-purepixel17-r1-k13` | 4.19.325-cip135-st19 | Android 17 R1 kernel build 13 |
| `tb132fu-v84-4.19.325-cip135` | 4.19.325-cip135-st19 | Earlier Android 16 checkpoint |
| `tb132fu-v83-4.19.325` | 4.19.325 | Known-good rollback checkpoint |
| `tb132fu-v82-4.19.300` | 4.19.300 | Known-good rollback checkpoint |
| `tb132fu-v77-4.19.200` | 4.19.200 | Earlier known-good checkpoint |

The Android 17 R1 development branch is
`tb132fu-android17-cip135-k13-audio-slot-controls`. Earlier matching source
branches are `tb132fu-4.19.325-cip135`, `tb132fu-4.19.325`, and
`tb132fu-4.19.300`; their branches and tags are retained for history.

## Earlier Android 16 validation on 4.19.325-cip135-st19

- Android 16 boots successfully.
- The 1536x2560 panel operates at 60 Hz and 120 Hz.
- Touch, Goodix pen, and screen off/on resume work across the full panel.
- Wi-Fi connects, Bluetooth reaches ON state, and the Lenovo pen reconnects.
- Both cameras and the rear flash enumerate through Android's camera service.
- GPS and FM modules load; speaker, haptic, double-tap gesture, and pogo-keyboard
  vendor paths are retained from the preceding functional checkpoint.
- Seven external connectivity modules pass an exact kernel symbol/CRC audit.
- Automated severe-event scans and hands-on use found no kernel panic, oops,
  warning, lockup, module ABI failure, or hardware regression.

Double-tap-to-wake is validated when Android performs a normal screen-off
transition. A generic GSI Always-On Display overlay can instead keep Android in
doze without issuing the framebuffer power-down transition used to arm the
Goodix gesture path. That ROM configuration issue is not a kernel regression.

Pure Pixel 17 uses a device-specific AOD/DT2W integration and does not rely on
that generic GSI behavior.

## Source composition

The tree combines the Motorola/MediaTek 4.19 vendor implementation used as the
newer vendor base, Lenovo TB132FU-specific hardware behavior recovered from the
available Lenovo source and running device, Linux stable updates through
4.19.325, and the CIP v4.19.325-cip135 maintenance checkpoint. Hardware-sensitive
conflicts are resolved conservatively and audited against the working tablet.

This is a downstream vendor kernel, not a byte-identical vanilla stable tree.
The version number does not guarantee that every known or unknown vulnerability
is fixed, especially in out-of-tree MediaTek, Motorola, or Lenovo drivers.

## Important warning

This source is specific to the Lenovo TB132FU. A boot image also depends on the
correct device tree, ramdisk, connectivity modules, boot-header layout, and
partition size. Do not flash a generic Image or use these artifacts on another
device. Keep a known-good boot image and a working fastboot connection before
testing changes.

Boot images, ramdisks, proprietary firmware, signing keys, device logs, and
private build artifacts are not distributed in this source tree. See
`COPYING` and individual file headers for source licensing.
