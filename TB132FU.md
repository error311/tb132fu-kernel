# Lenovo TB132FU kernel

This repository contains the source history for the Lenovo TB132FU kernel
bring-up and Android compatibility work. The current boot-validated source
checkpoint is Linux 4.19.325-cip135-st19 with the TB132FU vendor hardware
support retained.

## Tested checkpoints

| Tag | Kernel | Status |
| --- | --- | --- |
| `tb132fu-v84-4.19.325-cip135` | 4.19.325-cip135-st19 | Current boot-validated checkpoint |
| `tb132fu-v83-4.19.325` | 4.19.325 | Known-good rollback checkpoint |
| `tb132fu-v82-4.19.300` | 4.19.300 | Known-good rollback checkpoint |
| `tb132fu-v77-4.19.200` | 4.19.200 | Earlier known-good checkpoint |

The matching source branches are `tb132fu-4.19.325-cip135`,
`tb132fu-4.19.325`, and `tb132fu-4.19.300`. Older branches and tags are
retained for history.

## Validated on 4.19.325-cip135-st19

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

Boot images and proprietary firmware are not distributed in this source tree.
See `COPYING` and individual file headers for source licensing.
