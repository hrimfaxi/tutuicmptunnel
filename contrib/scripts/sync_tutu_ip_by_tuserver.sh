#!/bin/bash

export TUTU_UID=${TUTU_UID:-102}
export HOST=${HOST:-rack.h}
export COMMENT=${COMMENT:-x360}
export PORT=${PORT:-3322}
export SERVER_PORT=${SERVER_PORT:-3321}
export PSK=${PSK:-testpassword}

validate_ip() {
  local ip=$1
  local stat=1

  # 检查输入是否为空
  if [[ -z "$ip" ]]; then
    return 1
  fi

  # 使用点(.)作为分隔符，将IP地址分割到数组 octets 中
  IFS='.' read -r -a octets <<<"$ip"

  # 检查是否正好分割成4段
  if [[ ${#octets[@]} -ne 4 ]]; then
    stat=1
  else
    # 循环检查每一段
    for octet in "${octets[@]}"; do
      # 1. 检查是否是纯数字
      # 2. 检查数字范围是否在 0-255 之间
      # 3. 检查是否有前导0 (例如 01, 007)
      if ! [[ "$octet" =~ ^[0-9]+$ ]] ||
        ((10#$octet < 0 || 10#$octet > 255)) ||
        ([[ "${octet:0:1}" == "0" ]] && [[ "${#octet}" -gt 1 ]]); then
        stat=1
        break # 有一段不符合，就没必要继续检查了
      else
        stat=0
      fi
    done
  fi
  return $stat
}

if [ -z "$IP" ]; then
  IP=$(curl -4s ip.3322.net)
  if ! validate_ip "$IP"; then
    echo "验证失败：'$IP' 不是一个有效的 IPv4 地址。"
    exit 1
  fi
fi
# TODO: 没有检查ipv6地址的合法性
echo local ip: $IP

V() {
  echo "$@"
  "$@"
}

V tuctl_client psk $PSK server $HOST server-port $SERVER_PORT <<<"server; server-add \
  uid $TUTU_UID \
  address $IP \
  port $PORT \
  comment $COMMENT \
"

# vim: set sw=2 ts=2 expandtab:
