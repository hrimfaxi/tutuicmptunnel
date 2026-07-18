# WireGuard

[English](./wireguard.md) | [简体中文](./wireguard_zh-CN.md)

---

`WireGuard`是一个高效且强大的基于`UDP`的`VPN`协议。为了防止流量被`ISP`针对`UDP`进行`QoS`限速或干扰，
可以使用`tutuicmptunnel`对`WireGuard`流量进行封装和保护。

本教程假设你已经拥有一个配置好的 `/etc/wireguard/myserver.conf`，现在将对其进行调整，以启用`tutuicmptunnel`封装。

已知参数如下：

- **WireGuard 服务端端口**：`9000`
- **服务器域名**：`myserver.ip`
- **tuctl_server 端口**：`9010`
- **tuctl_server PSK 口令**：`verylongpsk`

## 部署

首先为你的客户端设备选择一个`uid`和主机名，在我们的例子里是`uid`=`100`，主机名为`a320`。

对服务器和客户端的`/etc/tutuicmptunnel/uids`添加:

```
100 a320
```

我们推荐在客户端使用 `tuctl_client` 来远程控制服务器上的 `tutuicmptunnel` 规则。因此，请确保服务器已安装并运行 `tutuicmptunnel-tuctl-server` 系统服务。

> 以下操作都在客户端上运行

首先，创建环境变量文件: `/etc/wireguard/tutuicmptunnel.myserver`

```sh
#!/bin/sh

TUTU_UID=a320
ADDR=myserver.ip
PORT=9000
SERVER_PORT=9010
PSK=verylongpsk
COMMENT=myserver-wgserver
```

在`WireGuard`配置中，添加如下内容以实现接口启动和关闭时的自动规则管理：

```ini
[Interface]
# 在接口启动前，先添加本地规则
PreUp = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && tuctl client-add user $TUTU_UID address $ADDR port $PORT comment $COMMENT || true
# 启动后，通过 tuctl_client 远程添加服务器规则
PreUp = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && tuctl_client server $ADDR server-port $SERVER_PORT psk $PSK <<< "server-add user $TUTU_UID comment $COMMENT address @client_ip@ port $PORT" || true
# 在接口关闭时，删除本地规则
PostDown = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && tuctl client-del user $TUTU_UID address $ADDR || true
# 接口关闭后，远程删除服务器规则
PostDown = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && tuctl_client server $ADDR server-port $SERVER_PORT psk $PSK <<< "server-del user $TUTU_UID" || true
```

这样配置后，每次启动`WireGuard`的`myserver`接口时，会自动添加客户端和服务器端的规则；关闭接口时，则会自动删除相关规则。

使用`wg-quick`重启接口即可：

```bash
sudo wg-quick down myserver
sudo wg-quick up myserver
```

如需排查`ICMP`流量，可用以下命令：

```bash
sudo tcpdump -i any -n icmp -v
```

> 小提示：启用`tcp_bbr`拥塞控制算法能显著提升`WireGuard`的性能。
