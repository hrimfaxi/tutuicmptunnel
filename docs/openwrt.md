# OpenWrt Client Guide

[English](./openwrt.md) | [简体中文](./openwrt_zh-CN.md)

---

## Overview

`tutuicmptunnel` relies on advanced kernel features such as `eBPF` and `kprobes`, and cannot run directly on standard OpenWrt firmware.
Standard OpenWrt kernels are trimmed for size, defaulting to removal of debugging and tracing features not essential for basic routing.

There are two approaches for deploying the client on an OpenWrt device:

| Approach | Description | Best for |
| :--- | :--- | :--- |
| **Option 1: Automatic build (recommended)** | Use [openwrt-tutuicmptunnel](https://github.com/hrimfaxi/openwrt-tutuicmptunnel) to build packages automatically via the OpenWrt SDK | Most users |
| **Option 2: Manual cross-compilation (this guide)** | Manually configure the toolchain, customize the OpenWrt kernel, and compile `tutuicmptunnel` and `tutu_csum_fixup` | Need to modify source code, debug, or your target platform has no build config yet |

> [!TIP]
> If you just want to get it running on your router, go with Option 1 — the build project already handles dependencies, install paths, and auto-start. This guide covers Option 2 for developers who need deeper control over the build process.

## Option 2: Manual Cross-Compilation

The overall workflow is as follows:

```mermaid
flowchart LR
    A[Download OpenWrt SDK<br/>Matching router model] --> B[Custom kernel build<br/>Enable BPF/kprobes]
    B --> C[Cross-compile<br/>libsodium / tutuicmptunnel<br/>tutu_csum_fixup]
    C --> D[scp to router<br/>tuctl* / tuctl_client<br/>.so / .ko]
    D --> E[Load kernel module<br/>and set auto-start]
```

### Environment Preparation

Download the OpenWrt SDK corresponding to your router model. For example, for Xiaomi AX3000T, go to the [firmware download page](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/) and download from the bottom:

* [openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)

Extract to `$HOME/temp`.

### Kernel Configuration

Since OpenWrt trims certain kernel features, you need to rebuild the OpenWrt kernel with additional options in `menuconfig`:

* `CONFIG_KERNEL_KPROBES=y`
* `CONFIG_KERNEL_KALLSYMS=y`
* `CONFIG_KERNEL_DEBUG_INFO=y`
* `CONFIG_KERNEL_DEBUG_INFO_BTF=y` (requires disabling `CONFIG_KERNEL_DEBUG_INFO_REDUCED`)
* `CONFIG_PACKAGE_kmod-sched-core=m`
* `CONFIG_PACKAGE_kmod-sched-bpf=m`

For convenience, consider building all `kmod` packages used by your OpenWrt device, or add: `CONFIG_ALL_KMOD=y`.

### User-Space Tools

The following packages are needed to inspect `bpf` rules and state; they pull in `libbpf` as a dependency, enabling `tuctl` to compile:

* `CONFIG_PACKAGE_tc-bpf`
* `CONFIG_PACKAGE_bpftool-full`

### 1. Set Up Build Environment

```bash
export STAGING_DIR=${HOME}/temp/openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64/staging_dir
export TOOLCHAIN_GLOB=$STAGING_DIR/toolchain-*
export TARGETROOT_GLOB=$STAGING_DIR/target-*
export TOOLCHAIN=( $TOOLCHAIN_GLOB )
export TARGETROOT=( $TARGETROOT_GLOB )
export KERNEL_DIR_GLOB=$STAGING_DIR/../build_dir/target-*/linux-*/linux-*
export KERNEL_DIR=( $KERNEL_DIR_GLOB )
export ARCH=aarch64
export CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc
export LIBSODIUM_TAG=1.0.20-RELEASE
export KERNEL_ARCH=arm64

echo ================================================================================
echo "TOOLCHAIN:  ${TOOLCHAIN}"
echo "TARGETROOT: ${TARGETROOT}"
echo "KERNEL_DIR:  ${KERNEL_DIR}"
echo "CC:  ${CC}"
echo ================================================================================
```

Verify that the printed `TOOLCHAIN`, `TARGETROOT`, `KERNEL_DIR` paths are valid before proceeding.

### 2. Build Dependencies

```bash
# Build libsodium
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout $LIBSODIUM_TAG # choose a stable release
./autogen.sh
./configure --host=$ARCH-openwrt-linux \
  CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc
make -j $(nproc)

# Build libbpf
cd ..
git clone https://github.com/libbpf/libbpf.git
cd libbpf
git submodule update --init
cd src
make OBJDIR=${HOME}/temp/libbpf-build DESTDIR=${HOME}/temp/libbpf-install \
  INCLUDEDIR=/usr/lib/include LIBDIR=/usr/lib \
  install
```

### 3. Build tutuicmptunnel

```bash
cd ../..
git clone https://github.com/hrimfaxi/tutuicmptunnel.git
cd tutuicmptunnel
rm -f CMakeCache.txt

export STAGING_DIR_HOST="$STAGING_DIR/host"
export PKG_CONFIG_LIBDIR="$TARGETROOT/usr/lib/pkgconfig"
export PKG_CONFIG="$STAGING_DIR_HOST/bin/pkg-config"
export PATH="$STAGING_DIR_HOST/bin:$PATH"

cmake \
  -DCMAKE_TOOLCHAIN_FILE=$(pwd)/toolchains/openwrt-$ARCH.cmake \
  -DBISON_EXECUTABLE=/usr/bin/bison \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_HARDEN_MODE=ON \
  -DUSE_SYSTEM_LIBBPF_BPFTOOL=0 \
  -DSODIUM_INCLUDE_DIR=$(pwd)/../libsodium/src/libsodium/include -DSODIUM_LIBRARY=$(pwd)/../libsodium/src/libsodium/.libs/libsodium.so \
  .
make VERBOSE=1 clean all
make install DESTDIR=stripped
```

## Deploy to Device

Copy build artifacts to the OpenWrt device:

```bash
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/tuctl router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
```

You can now use `tuctl`, `tuctl_client`, etc. on the OpenWrt device.

## Reducing tuctl_client Memory Usage

On memory-constrained devices, you can reduce `tuctl_client` memory usage by lowering the password hashing memory cost.
Set the environment variable `TUTUICMPTUNNEL_PWHASH_MEMLIMIT` (unit: bytes).

**Server** (systemd unit file):

```ini
Environment=TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768
```

**Client**:

```bash
TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768 tuctl_client ...
```

> [!WARNING]
> * Server and client must use the **same value**, otherwise handshake fails and communication cannot be established.
> * Lower values mean less memory usage but also lower security — weigh this based on device capabilities and risk assessment.
