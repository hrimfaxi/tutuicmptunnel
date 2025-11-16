# hysteria

## 示例展示

```
本地 ‑(SOCKS5)→ Hysteria-UDP ‑(ICMP)→ 服务器
全部参数均已抽象为占位符，请按实际情况替换，尤其是域名 / 密钥。
⚠️ 文档刻意省略了真实 SNI、密码、IP 等敏感信息，请勿在公开场合留下明文。
```

我们假设你现在有一个可以用的`hysteria`配置，`UDP`端口为`3322`。
现在你需要使用`tutuicmptunnel`将流量转变为`ICMP`协议。

## 依赖

`Ubuntu`:

```bash
sudo apt-get install curl
```

`Archlinux`:

```bash
sudo pacman -S curl
```

## `tutuicmptunnel`设置

首先要在服务器上为你的客户端设备选好一个`UID`，比如说主机名为`a320`，`UID`为100。

`/etc/tutuicmptunnel/uids`:

```
100 a320 # a320
```

客户端上也同样添加以上记录到`/etc/tutuicmptunnel/uids`。

## 修改systemd 单元文件

注意`tutuicmptunnel`无法处理`GSO`包，需要关闭`hysteria`的相关功能。
通过设置环境变量`QUIC_GO_DISABLE_GSO=1`，如下所示：

 `/etc/systemd/system/hysteria-server@.service`:

```ini
[Unit]
Description=Hysteria Server (%i.yaml)
After=network.target tutuicmptunnel.service

[Service]
Type=simple
ExecStart=/usr/local/bin/hysteria server --config /etc/hysteria/%i.yaml
DynamicUser=yes
Environment=HYSTERIA_LOG_LEVEL=info QUIC_GO_DISABLE_GSO=1
CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_BIND_SERVICE CAP_NET_RAW
AmbientCapabilities=CAP_NET_ADMIN CAP_NET_BIND_SERVICE CAP_NET_RAW
NoNewPrivileges=true
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## 清理过期会话

如果架构不支持`bpf_timer`才需要需要清理过期会话（`mips`或者较低版本的`ubuntu`)

```bash
sudo cp contrib/etc/systemd/system/tutuicmptunnel-server-reaper.timer /etc/systemd/system/
sudo systemctl enable --now tutuicmptunnel-server-reaper.timer
```

### 同步客户端`ip`脚本

可以在客户端上使用`tuctl_client`工具来远程修改服务器的`tuctl`配置，这样客户端公网`IP`切换了也可以通知服务器。

`/usr/local/bin/tutuicmptunnel_sync.sh`:

```bash
#!/bin/bash

# 本脚本在客户端上运行，通过tuctl_client工具通告服务器客户端配置的更新。

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=eth0 # 你的客户端的上网接口名
sudo tuctl dump > $TMP
sudo tuctl unload iface $DEV
sudo tuctl load iface $DEV #ethhdr #debug

export TUTU_UID=100 # 替换为你的服务器上选好的uid或用户名
export ADDRESS=yourdomain.com # 替换为你的hysteria服务器域名或IP
export PORT=3322 # 替换为你的hysteria服务器udp端口

sudo tuctl script - < $TMP
sudo tuctl client
sudo tuctl client-add address $ADDRESS port $PORT user $TUTU_UID

export COMMENT=yourdevice # 替换为你的客户端的注释，此注释会在服务器的tuctl命令上显示
export HOST=$ADDRESS
export PSK=yourlongpsk # 替换为你的tuctl_server的PSK口令
export SERVER_PORT=your_tuserver_port # 替换为你的tuctl_server的端口

# 使用3322.net提供的服务获取本机公网IP，你也可以换成其他服务
IP=$(curl -s ip.3322.net)
echo local ip: $IP

V tuctl_client psk $PSK server $HOST server-port $SERVER_PORT <<<"server-add uid $TUTU_UID address $IP port $PORT comment $COMMENT"

# vim: set sw=2 ts=2 expandtab:
```

### 启动`tutuicmptunnel`

```bash

# 先同步tutuicmptunnel的客户端IP
/usr/local/bin/tutuicmptunnel_sync.sh

# 再跑 hysteria 客户端
QUIC_GO_DISABLE_GSO=1 hysteria client -c client.yaml
```

如果客户端公网`IP`频繁切换，此时需要更新客户端`IP`到服务器。可以添加以上脚本到`crontab`定期运行（每5分钟）：

```
PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin
*/5 * * * * /usr/local/bin/tutuicmptunnel_sync.sh
```

或者使用一个`systemd-timer`达到同样目的：

`/etc/systemd/system/tutuicmptunnel_sync.service`:

```ini
[Unit]
Description=Sync tutuicmptunnel config

[Service]
Type=oneshot
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin
ExecStart=/usr/local/bin/tutuicmptunnel_sync.sh
```

`/etc/systemd/system/tutuicmptunnel_sync.timer`:

```
[Unit]
Description=Run tutuicmptunnel_sync every 5 minutes

[Timer]
OnBootSec=5min
OnUnitActiveSec=5min
Persistent=true

[Install]
WantedBy=timers.target
```

## 测试

测速：

```bash
hysteria speedtest -c client.yaml 
```

查看 ICMP 隧道计数：

```bash
sudo tuctl -d
```

`BPF`日志：

```bash
sudo bpftool prog tracelog
```

查看是否有icmp包：

```bash
sudo tcpdump -i any icmp -n -v
```
