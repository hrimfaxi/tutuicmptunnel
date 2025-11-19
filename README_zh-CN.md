# tutuicmptunnel

基于 `bpf` 的 `UDP` 转 `ICMP` 隧道工具，可作为 `udp2raw` `ICMP` 模式的替代方案。
推荐与 `kcptun`、`hysteria`、`wireguard` 等工具配合使用，共同应对 `GFW` 或 `ISP` 越来越严厉的 `UDP` `QOS` 和丢包策略，有效提升穿透能力和连接稳定性。

## 优点与特性

1. 同等`cpu`下最大流量比`udp2raw`快几倍，同时`cpu`占用资源用少的多。参见[性能测试](docs/benchmark.md)
2. 安全的设计与实现
3. 支持`openwrt`上运行
4. 支持`ipv4/ipv6`下的`icmp`/`icmp6`
5. 可以使用`tuctl_server`安全而迅速的同步服务器/客户端配置

## 概述

`tutuicmptunnel`分为服务器端和客户端两部分，双方都需要分别运行对应的服务器或客户端程序。
每台主机只能扮演服务器或客户端中的一种角色，不能同时兼任。
为了区分不同客户端发送的报文，每个连接到服务器的客户端都会被分配一个唯一的`UID`取值范围为0~255，
该`UID`对应`ICMP`协议的`code`字段。因此，每台服务器最多支持256个客户端用户。

客户端也可以与不同的服务器进行映射。例如，可以将主机`1.2.3.4`的端口`3322`映射为`uid` 100，
而将主机`2.3.4.5`的端口`2233`映射为`uid` 101。
这样，客户端可以根据不同的服务器和端口，分配和使用不同的`UID`，以实现对多台服务器的灵活接入和管理。

允许客户端在不同的服务器上使用相同的`UID`，因为`UID`实际上只是标识该客户端在特定服务器上的身份。
也就是说，`UID`仅在每台服务器范围内唯一，不同服务器之间可以重复分配相同的`UID`。

* 客户端使用三元组 [`UID`, 服务器`IP`, 目标端口（`port`）] 来标识哪些`UDP`包需要被转换为ICMP包。
* 服务器端则使用三元组 [`UID`, 客户端`IP`, 目标端口（`port`）] 来标识哪些`ICMP`包需要被还原并转发为`UDP`包。
* `IP`地址可以是`IPv4`或`IPv6`，`tutuicmptunnel`会根据`IP`类型自动选择使用`ICMP`或`ICMPv6`进行封装和转发。

`tutuicmptunnel`可以与`WireGuard`、`xray-core`+`kcptun`、`hysteria`等工具搭配使用。
由于这些软件本身已经具备加密和完整性校验功能，因此`tutuicmptunnel`不负责数据的加密、混淆和校验，仅负责数据的封装与转发。

`tutuicmptunnel`不会修改数据包的负载内容，也不会在报文中添加额外的`IP`头部。
服务器端的转发规则完全依赖用户通过命令手动添加上述三元组进行配置。

客户端可以通过`SSH`命令调用服务器命令：使用[tuctl](tuctl/README.md)命令手动修改上述三元组（包括更新客户端自身的`IP`地址）。
为了方便客户端动态通知服务器更新自己的`IP`和端口信息，也可以使用`tuctl_client`工具通过`UDP`协议实现配置同步。

`tuctl_server`和`tuctl_client`使用UDP协议进行通信，采用基于时间戳的机制，并结合`XChaCha20-Poly1305`加密算法、`Argon2id`密钥派生算法以及预共享密钥（`PSK`）进行安全认证。这种方案可以安全且高效地让客户端将新的配置信息实时通知到服务器。

## 操作系统要求与依赖

### `Ubuntu`

版本：至少20.04，建议使用24.04以上`lts`版本。

依赖准备:

```sh
sudo apt install -y git libbpf-dev clang llvm cmake libsodium-dev dkms linux-tools libsodium-dev libelf-dev flex bison
```

注意：如果你安装的不是标准`ubuntu`内核，请安装对应内核的`linux-tools`。或者放弃使用官方`bpftool`，在`cmake`配置时使用参数`-DUSE_SYSTEM_LIBBPF_BPFTOOL=0`。

### `Arch Linux`

版本：最新即可

依赖准备:

```sh
sudo pacman -S git base-devel libbpf clang cmake libsodium dkms libsodium flex bison
```

### `Openwrt`

版本: 至少24.10.1，请看[openwrt指南](docs/openwrt.md)

## 安装方法

1. 检出代码并安装

```sh
git clone https://github.com/hrimfaxi/tutuicmptunnel
cd tutuicmptunnel
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_HARDEN_MODE=1 -DUSE_SYSTEM_LIBBPF_BPFTOOL=1 .
```

注意：如果是`Ubuntu` 20.04，需要使用`git`版本的`libbpf`/`bpftool`，并且关闭`bpf timer`支持

```sh
cmake -DCMAKE_BUILD_TYPE=Release -DUSE_SYSTEM_LIBBPF_BPFTOOL=0 -DDISABLE_BPF_TIMER=1 -DBPF_CPU_VERSION="" .
````

```sh
make
sudo make install
```

2. 内核模块

服务器和客户端上都需要安装[tutu_csum_fixup](tutu_csum_fixup/README.md)内核模块，用于修复`bpf`不能修改的`icmp`包检验和。

```sh
cd tutu_csum_fixup
sudo dkms remove tutu_csum_fixup/x.x --all # 如果之前安装过旧版本的话，使用dkms status查看过去版本
sudo make dkms
sudo tee -a /etc/modules-load.d/modules.conf <<< tutu_csum_fixup
sudo modprobe tutu_csum_fixup
```

有些系统需要设置`force_sw_checksum`参数，详情参见[tutu_csum_fixup](tutu_csum_fixup/README.md#force_sw_checksum)。

3. 服务器：设置系统服务并启用可选的`tuctl_server`

`tuctl_server`可以帮助客户端控制服务器端的`tutuicmptunnel`配置。

请完成以下要求：
* 为防止暴力破解，使用前请记得选一个足够强大的`PSK`口令。最好是使用`uuidgen -r`命令生成。
* 由于使用了时间戳作为验证，服务器/客户端都需要准确的系统时间。

```sh
# 开机自动加载tutuicmptunnel服务并恢复配置
sudo cp contrib/etc/systemd/system/tutuicmptunnel-server@.service /etc/systemd/system/
sudo systemctl enable --now tutuicmptunnel-server@eth0.service # 其中eth0是服务器的网络接口

# 可选的tuctl_server
sudo cp contrib/etc/systemd/system/tutuicmptunnel-tuctl-server.service /etc/systemd/system/
# 修改psk，端口等
sudo vim /etc/systemd/system/tutuicmptunnel-tuctl-server.service
timedatectl | grep "System clock synchronized:" # 检查系统时间是否已经ntp同步
# 重载配置
sudo systemctl daemon-reload
# 开启tuctl_server服务
sudo systemctl enable --now tutuicmptunnel-tuctl-server
```

此时可以使用`tuctl`命令检查服务器状态
```sh
sudo tuctl
tutuicmptunnel: Role: Server, BPF build type: Release, no-fixup: off

Peers:
....
```

也可以不使用`systemd`系统服务手工启动`tutuicmptunnel`为服务器模式：

```sh
sudo tuctl unload iface eth0 # 先清理
sudo tuctl load iface eth0 # 加载bpf到eth0接口
sudo tuctl server # 设置为服务器模式
sudo tuctl server-add uid 123 address 1.2.3.4 port 1234 # 添加客户端(id 123)，ip为1.2.3.4，目的udp端口为1234
sudo tuctl server-del uid 123 # 删除之前的客户端
```

4. 可选设置`UID`和主机名映射表

为了便于管理`UID`，`tutuicmptunnel`支持通过映射表`/etc/tutuicmptunnel/uids`将客户端主机名与`UID`进行对应。
可以按如下方式创建和编辑该文件：

```sh
sudo mkdir -p /etc/tutuicmptunnel
sudo vim /etc/tutuicmptunnel/uids
```

格式如下：

```
#
# 格式： UID 主机名 # 可选的注释
#

0 alice # alice's laptop
1 bob   # bob's laptop
```

设置完成后，在`tuctl`命令中，所有需要指定`UID`(如`uid 0`）的地方，都可以直接使用主机名（如`user alice`）进行替代，使管理更加直观和便捷。

5. 客户端：设置系统服务并启用`tutuicmptunnel`

```sh
# 设置tutuicmptunnel开机启动
sudo cp contrib/etc/systemd/system/tutuicmptunnel-client@.service /etc/systemd/system/
sudo systemctl enable --now tutuicmptunnel-client@enp4s0 # 假设你上网接口是enp4s0
```

现在可以试用下`tutuicmptunnel`：

```sh
export ADDRESS=yourserver.com # 服务器域名或者ip
export PORT=3322 # 需要转换为icmp的udp端口
export TUTU_UID=123 # tutuicmptunnel用户的id
export PSK=yourlongpsk # tuctl_server的psk
export SERVER_PORT=14801 # tuctl_server的端口
export COMMENT=yourname # 对你的客户端的身份的描述，命令成功之后可以在服务器上tuctl命令输出中查看注释

# 设置为客户端模式
sudo tuctl client
# 设置服务器的终端配置
sudo tuctl client-add uid $TUTU_UID address $ADDRESS port $PORT
# 验证是否正确
sudo tuctl status
# 使用3322.net获取客户端的公网IP
IP=$(curl -s ip.3322.net)
# 使用tuctl_client通知服务器新的客户端设置
tuctl_client psk $PSK server $ADDRESS server-port $SERVER_PORT <<< "server-add uid $TUTU_UID address $IP port $PORT comment $COMMENT"
```

此时可以到服务器上去使用`sudo tuctl`命令查看客户端添加的规则：

```sh
tutuicmptunnel: Role: Server, BPF build type: Release, no-fixup: off

Peers:
  User: xxxx, Address: xxx.xxx.xxx.xxx, Sport: 37926, Dport: 3322, ICMP: 11403, Comment: yourname
```

如果一切配置正确，此时客户端与服务器之间，原本源地址为客户端、目标端口为3322的`UDP`通信，会被`tutuicmptunnel`在客户端自动转换为`ICMP`包发送。
服务器接收到`ICMP`包后，会将其还原为`UDP`包并继续转发。 在整个传输过程中，网络中间节点只能看到`ICMP Echo`/`Reply`报文。

## 主要应用场景

| 名称 | 简介 |
| :--- | :--- |
| [iperf3](docs/iperf3.md) | 一款强大的网络性能测试工具，用于测量带宽、抖动和丢包。 |
| [hysteria](docs/hysteria.md) | 基于 QUIC 协议的代理工具，专为不稳定和高丢包网络优化。 |
| [xray+kcptun](docs/xray_kcptun.md) | Xray 核心与 KCPtun 协议的组合，用于加速和稳定网络连接。 |
| [wireguard](docs/wireguard.md) | 一个现代化、高性能且配置简单的安全 VPN 隧道。 |
| [openwrt](docs/openwrt.md) | 针对嵌入式设备（尤其是路由器）的高度可定制化 Linux 操作系统。 |

## 致谢

`tutuicmptunnel`在设计、实现和性能调优过程中，参考并受益于大量优秀的开源项目和技术文章。谨向它们的作者及社区贡献者致以诚挚感谢！

* [hysteria](https://github.com/apernet/hysteria)
* [kcptun](https://github.com/xtaci/kcptun)
* [xray-core](https://github.com/XTLS/Xray-core)
* [udp2raw](https://github.com/wangyu-/udp2raw)
* [mimic](https://github.com/hack3ric/mimic)
* [libbpf-bootstrap](https://github.com/libbpf/libbpf-bootstrap)

特别鸣谢 [@hack3ric](https://github.com/hack3ric)
及所有贡献者的持续维护，让`eBPF`上`UDP`→`fakeTCP` 混淆成为可能。

## 许可证

本项目整体遵循 **GNU General Public License v2.0**。
其中 `libbpf`、`bpftool` 子模块保留其各自原始许可证（`LGPL-2.1` / `BSD-2-Clause`）。
