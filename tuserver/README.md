# tuserver

用于安全并抗重放的通过udp下发tuctl命令到服务器。比ssh连接要快。

## 特点

1. 服务器客户端使用psk模式生成密码
2. 为了抗重放，使用了时间戳来检查重放窗口。要求服务器客户端均使用`NTP``协议同步时间，否则无法使用。
3. 会插入随机负载来随机化udp包大小

## 服务器

```sh
tuctl_server -k yoursafepsk
```

如果端口需要改变默认的`14801`，添加`--port PORT`。

服务器支持以下环境变量:

- `TUTUICMPTUNNEL_PWHASH_MEMLIMIT`: 控制`argon2id13`算法的内存用量， 低内存设备可以适量减少
- `TUTUICMPTUNNEL_USE_KTUCTL``: 调用`ktuctl`而不是`tuctl`修改服务器的配置。

## 客户端 

```sh
tuctl_client psk yoursafepsk server servername script - <<< dump
```

如果端口需要改变默认的`14801`，添加`server-port PORT`。

## systemd

可以使用`contrib/systemd/system/tutuicmptunnel-tuctl-server.service`来自动启用服务：

```sh
sudo cp contrib/systemd/system/tutuicmptunnel-tuctl-server.service /etc/systemd/system/
sudo systemctl edit --full tutuicmptunnel-tuctl-server
# 编辑psk或者端口
sudo systemctl enable --now tutuicmptunnel-tuctl-server
```

而在客户端可以考虑使用cron服务器每5分钟更新IP:

```sh
*/5 * * * * PSK=yousafepsk SERVER=myhost SERVER_PORT=14801 TUTU_UID=123 COMMENT=laptop /usr/local/bin/sync_tuctl_server.sh
```

/usr/local/bin/sync_tuctl_server.sh:
```sh
#!/bin/bash

tuctl_client psk $PSK server $SERVER server-port $SERVER_PORT script - <<< "add $TUTU_UID comment $COMMENT address @client_ip@ PORT=3322"
```

`@client_ip@` 是一个占位符，`tuctl_server` 在执行脚本时会自动将其替换为客户端的 UDP 源地址。
