# Building the TB132FU Android 17 R1 kernel

## Exact source and toolchain

- release-source tag: `tb132fu-purepixel17-r1-k13`
- exact kernel implementation commit: `5b0f70f39eff047774cf24d61301f92506330113`
- kernel release: `4.19.325-cip135-st19+`
- toolchain: Android Clang `r383902` (clang 11.0.1, Android build 6443078)
- architecture: arm64
- defconfig: `tb132fu_android17_defconfig`
- build number: 13

The release-source tag contains the exact implementation commit plus the
tracked defconfig and public documentation. The defconfig was generated from
the accepted R1 `.config`; these publication additions change no kernel
behavior and make the build configuration reproducible.

## Build

Obtain Android's `clang-r383902` Linux host prebuilt, then set `CLANG_DIR` to
its root directory. The directory must contain `bin/clang`.

```sh
export CLANG_DIR=/path/to/clang-r383902
export OUT_DIR=/path/to/an/empty/output-directory
./build-tb132fu.sh
```

`OUT_DIR` defaults to `out` below the source tree. `JOBS` defaults to the host
CPU count and `KBUILD_BUILD_VERSION` defaults to `13`. The helper produces the
kernel Image, compressed Image, DTBs, and in-tree modules. It verifies the
expanded configuration before compiling.

The accepted expanded `.config` has SHA-256:

```text
6c6920de38ca9d55e1a7b4cbc74fc0cdbb9c5ab4b402c5fbec35201dfa7c5f43
```

The accepted R1 `Module.symvers` has 12,233 entries and SHA-256:

```text
75ba5cd713a4a4d0ec2472bdfd67e5639ee3eb72a814394c2099d427c829269a
```

The module ABI hash is a release-verification value. It is sensitive to the
exact toolchain and complete build environment.

## Boot-image boundary

The generated `arch/arm64/boot/Image.gz` is not a flashable Android boot image.
A working TB132FU boot image also requires the matching ramdisk and DTB, boot
header v2 geometry, command line, 64 MiB partition sizing, and AVB metadata.
Those inputs belong to the matching device/ROM release and are intentionally
not embedded in this kernel source repository. Never flash a standalone Image
or an image assembled for another device.
