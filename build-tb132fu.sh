#!/usr/bin/env bash

set -Eeuo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out_dir=${OUT_DIR:-$source_dir/out}
clang_dir=${CLANG_DIR:-}
jobs=${JOBS:-$(nproc)}
build_version=${KBUILD_BUILD_VERSION:-13}
expected_config_sha=6c6920de38ca9d55e1a7b4cbc74fc0cdbb9c5ab4b402c5fbec35201dfa7c5f43

if [[ -z "$clang_dir" || ! -x "$clang_dir/bin/clang" ]]; then
    echo 'Set CLANG_DIR to the Android clang-r383902 prebuilt root.' >&2
    exit 1
fi

for tool in make sha256sum nproc; do
    command -v "$tool" >/dev/null || {
        echo "Missing required host tool: $tool" >&2
        exit 1
    }
done

mkdir -p -- "$out_dir"

make_args=(
    -C "$source_dir"
    O="$out_dir"
    ARCH=arm64
    LLVM=1
    LLVM_IAS=1
    CROSS_COMPILE=aarch64-linux-gnu-
    CLANG_TRIPLE=aarch64-linux-gnu-
    MALI_CUSTOMER_RELEASE=1
    MALI_UNIT_TEST=0
    KBUILD_BUILD_VERSION="$build_version"
)

export PATH="$clang_dir/bin:/usr/bin:/bin"

make "${make_args[@]}" tb132fu_android17_defconfig

observed_config_sha=$(sha256sum "$out_dir/.config" | awk '{print $1}')
if [[ "$observed_config_sha" != "$expected_config_sha" ]]; then
    echo 'Expanded kernel configuration does not match Pure Pixel 17 R1.' >&2
    echo "Expected: $expected_config_sha" >&2
    echo "Observed: $observed_config_sha" >&2
    exit 1
fi

make "${make_args[@]}" -j"$jobs" Image Image.gz dtbs modules

kernel_release=$(make "${make_args[@]}" -s kernelrelease)
printf 'Kernel release: %s\n' "$kernel_release"
sha256sum \
    "$out_dir/arch/arm64/boot/Image" \
    "$out_dir/arch/arm64/boot/Image.gz" \
    "$out_dir/.config" \
    "$out_dir/Module.symvers"

echo 'Build complete. The generated Image/Image.gz is not a flashable boot.img.'
