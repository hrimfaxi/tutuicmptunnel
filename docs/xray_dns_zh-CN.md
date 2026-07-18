# 使用 tutuicmptunnel 保护 DNS 查询（xray + OpenWrt）

[English](./xray_dns.md) | [简体中文](./xray_dns_zh-CN.md)

---

## 概述

将 VPS 上的 DNS 查询通过 xray-core 的 dokodemo-door 转发到 8.8.8.8，并在 OpenWrt 侧用 tutuicmptunnel 将 UDP 查询封装为 ICMP，降低 DPI 干扰与污染概率。通过 iptables 将对外的 53/UDP 重定向到本机 5353/UDP，避开 systemd-resolved 端口占用。

## 拓扑与思路

- 选一台香港 VPS（EDNS 选路更接近国内，延迟较低）。
- VPS 上用 xray-core 开 5353/UDP dokodemo-door，转发到 8.8.8.8:53。
- VPS 上用 iptables 将 eth0 的 53/UDP 重定向到 5353/UDP。
- OpenWrt 上使用 tutuicmptunnel，将外发 DNS 的 UDP 封装为 ICMP 转发，绕开GFW DPI。
- OpenWrt 的 WAN 接口 up/down 时通过 hotplug 脚本自动建立/拆除隧道。
- 最终在 OpenWrt 接口里把上游 DNS 配为你的 VPS IP。

## VPS 端配置

### xray-core 配置
在服务端添加 Inbound（dokodemo-door）监听 5353/UDP，并转发到 8.8.8.8:53：

```json
        {
            "port": 5353,
            "protocol": "dokodemo-door",
            "settings": {
                "address": "8.8.8.8",
                "port": 53,
                "network": "udp"
            },
            "tag": "dns-in"
        },
```

> 说明
>
> - 使用 5353 是为了避免与 systemd-resolved 占用的 53 端口冲突。
> - 该 Inbound 仅处理 UDP。

### 重启服务：

```sh
sudo systemctl restart xray
```

## 端口重定向（iptables）

将来自 eth0 的 UDP 53 重定向到本机 UDP 5353：
插入到最前，避免被其他范围规则抢先匹配

```sh
sudo iptables -t nat -I PREROUTING 1 -i eth0 -p udp --dport 53 -j REDIRECT --to-ports 5353
```

持久化（Debian/Ubuntu）：

```sh
sudo apt-get install -y iptables-persistent
sudo netfilter-persistent save
```
验证：

```sh
sudo iptables -t nat -L -v -n
```

测试解析（从外部机器）：

```sh
dig reddit.com @your_vps_ip
;; ->>HEADER<<- opcode: QUERY, rcode: NOERROR, id: 44623
;; flags: qr rd ra ; QUERY: 1, ANSWER: 4, AUTHORITY: 0, ADDITIONAL: 0
;; QUESTION SECTION:
;; reddit.com.    IN    A

;; ANSWER SECTION:
reddit.com.    192    IN    A    151.101.193.140
reddit.com.    192    IN    A    151.101.1.140
reddit.com.    192    IN    A    151.101.65.140
reddit.com.    192    IN    A    151.101.129.140

;; AUTHORITY SECTION:

;; ADDITIONAL SECTION:

;; Query time: 80 msec
;; SERVER: x.x.x.x
;; WHEN: Fri Oct 10 10:21:19 2025
;; MSG SIZE  rcvd: 92
```

但不要高兴太早，此时GFW DPI仍然能污染你：

```sh
dig www.google.com @your_vps_ip

;; ->>HEADER<<- opcode: QUERY, rcode: NOERROR, id: 62391
;; flags: qr rd ra ; QUERY: 1, ANSWER: 1, AUTHORITY: 0, ADDITIONAL: 0
;; QUESTION SECTION:
;; www.google.com.    IN    A

;; ANSWER SECTION:
www.google.com.    141    IN    A    31.13.88.26

;; AUTHORITY SECTION:

;; ADDITIONAL SECTION:

;; Query time: 51 msec
;; SERVER: x.x.x.x
;; WHEN: Fri Oct 10 10:14:51 2025
;; MSG SIZE  rcvd: 48
```

# OpenWrt 端配置

## tutuicmptunnel 与热插拔脚本

使用 hotplug 在 WAN up 时创建通道，在 WAN down 时清理。以下以 53 端口为例，请替换变量中的真实值。

    /etc/hotplug.d/iface/95-wan-up 
```sh
#!/bin/sh

[ "$ACTION" = "ifup" ] || exit 0
[ "$INTERFACE" = "wan" ] || exit 0

logger "启动wan自定义脚本"

UID_=yourname-dns
HOST=x.x.x.x
PSK=yourpsk
PORT=53
#export TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768 根据你的tuserver设置

V() {
  echo "$@"
  "$@"
}

tuctl client-del $UID_ address $HOST
tuctl client-add uid $UID_ address $HOST port $PORT comment your-vps-name-dns
echo "server-add uid $UID_ address @client_ip@ port $PORT comment yourname-dns" | V tuctl_client server $HOST server-port 14801 psk $PSK
```

    /etc/hotplug.d/iface/95-wan-down
```sh
#!/bin/sh

[ "$ACTION" = "ifdown" ] || exit 0
[ "$INTERFACE" = "wan" ] || exit 0

logger "关闭wan自定义脚本"

UID_=yourname-dns
HOST=x.x.x.x
PSK=yourpsk
PORT=53
COMMENT=yourname-dns
#export TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768 根据你的tuserver设置

V() {
  echo "$@"
  "$@"
}

tuctl client-del uid $UID_ address $HOST
echo "server-del uid $UID_" | V tuctl_client server $HOST server-port 14801 psk $PSK
```

在服务器与客户端两侧的 /etc/tutuicmptunnel/uids 文件中添加对应 UID，例如：

```
116 yourname-dns
```



说明：

- 变量 UID_ 用于标识本次隧道实例，需两端一致。
- HOST 为你的 VPS 公网 IP。
- PSK 为预共享密钥。
- tuctl_client/tuctl 的命令与端口 14801 按你的部署保持一致。
- tutuicmptunnel 在 netfilter 之前进行 UDP→ICMP 转换，不会与 iptables 的 REDIRECT 产生冲突。
- 确保 OpenWrt 与 VPS 的时间同步，避免基于密钥/会话的机制失效。

## 测试和启用dns

此时就可以使用ifdown wan; ifup wan来启用tutuicmptunnel来保护udp(转为icmp)。

检查：
```sh
dig reddit.com @your_vps_ip
;; ->>HEADER<<- opcode: QUERY, rcode: NOERROR, id: 44623
;; flags: qr rd ra ; QUERY: 1, ANSWER: 4, AUTHORITY: 0, ADDITIONAL: 0
;; QUESTION SECTION:
;; reddit.com.    IN    A

;; ANSWER SECTION:
reddit.com.    192    IN    A    151.101.193.140
reddit.com.    192    IN    A    151.101.1.140
reddit.com.    192    IN    A    151.101.65.140
reddit.com.    192    IN    A    151.101.129.140

;; AUTHORITY SECTION:

;; ADDITIONAL SECTION:

;; Query time: 80 msec
;; SERVER: x.x.x.x
;; WHEN: Fri Oct 10 10:21:19 2025
;; MSG SIZE  rcvd: 92
```

若返回 4 个一致的 IP，且与全球公共解析一致，说明污染显著缓解。
在网络接口或 DHCP/DNS 配置中，将主 DNS 设置为你的 VPS IP（即 HOST）。

# 总结

- xray-core 负责把进入 VPS 的 53/UDP 查询转发到稳健的公共 DNS。
- iptables 将外部对 53 的请求重定向到 xray 的 5353，规避端口冲突。
- tutuicmptunnel 在 OpenWrt 将 UDP 查询转换为 ICMP，有效减轻 DPI 污染。
- 通过 hotplug 脚本自动管理通道，整体流程稳定，可在家庭宽带与移动网络下取得较好的解析准确性。
