# openwrt客户端指南

## 概述

`tutuicmptunnel` 可以在标准的 `OpenWrt` 固件上运行。
本指南将详细介绍交叉编译 `tutuicmptunnel` 项目，使其在 `OpenWrt` 设备上成功运行客户端模式。

## 环境准备

需要下载路由器品牌对应的`openwrt-sdk`包，比如你是小米路由器ax3000t，就到
[小米ax3000t固件下载页面](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/)
页面下方下载：
- [openwrt-sdk](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)

然后解压到`$HOME/temp`目录。

## 自动编译

参见[openwrt-tutuicmptunnel](https://github.com/hrimfaxi/openwrt-tutuicmptunnel)

## 手工交叉编译

以下是在`aarch64`架构的`openwrt`下的交叉编译方法。

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

# 编译`libsodium`
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout $LIBSODIUM_TAG # 选择一个最新的稳定版发布
./autogen.sh
./configure --host=$ARCH-openwrt-linux \
  CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc
make -j $(nproc)

# 编译`libbpf`
cd ..
git clone https://github.com/libbpf/libbpf.git
cd libbpf
git submodule update --init
cd src
make OBJDIR=${HOME}/temp/libbpf-build DESTDIR=${HOME}/temp/libbpf-install \
  INCLUDEDIR=/usr/lib/include LIBDIR=/usr/lib \
  install

# 编译`tutuicmptunnel`
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

最后复制库到`openwrt`设备：

```bash
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/tuctl router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
```

`ssh`到`openwrt`设备，设置开机启动模块:

此时就可以在`openwrt`设备上使用`tuctl`，`tuctl_client`等程序。

## tuctl_client内存使用量问题

在内存受限的设备上，可通过降低密码哈希的内存占用来减少`tuctl_client`的内存使用。
设置环境变量`TUTUICMPTUNNEL_PWHASH_MEMLIMIT`（单位：字节）即可实现。

示例

    服务器（单元文件）：
    Environment=TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768

    客户端：
    TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768 tuctl_client ...

要点

> 服务器与客户端必须使用相同的数值，否则将无法建立通信。 \
> 该参数数值越小，内存占用越低，但安全性也会相应下降，请根据设备能力和风险评估进行权衡。 \
> 服务器与客户端的TUTUICMPTUNNEL_PWHASH_MEMLIMIT必须一致；不一致将导致握手失败、无法通信。
