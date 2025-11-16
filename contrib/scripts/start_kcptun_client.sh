#!/bin/sh

SMUXVER=${SMUXVER:-1}
HOST=${HOST:-107.174.121.111}
PORT=${PORT:-3322}
LOCAL_PORT=${LOCAL_PORT:-8964}
MODE=${MODE:-fast}
NOCOMP=${NOCOMP:-"-nocomp"}
AUTOEXPIRE=${AUTOEXPIRE:-900}
DATASHARD=${DATASHARD:-0}
PARITYSHARD=${PARITYSHARD:-0}
CRYPT=${CRYPT:-xor}
KEY=${KEY:-LC2N0lx5_Kq6l.6l}
RCVWND=${RCVWND:-1024}
SNDWND=${SNDWND:-256}
SOCKBUF=${SOCKBUF:-16777217}
MTU=${MTU:-1400}

trap "should_exit=1" INT

should_exit=0
while [ $should_exit -eq 0 ]; do
  kcptun-client \
    --smuxver $SMUXVER \
    -r "${HOST}:${PORT}" \
    -l ":${LOCAL_PORT}" \
    -mode "${MODE}" \
    ${NOCOMP} \
    -autoexpire "${AUTOEXPIRE}" \
    --datashard "${DATASHARD}" \
    --parityshard "${PARITYSHARD}" \
    --crypt "${CRYPT}" \
    --key "${KEY}" \
    --rcvwnd "${RCVWND}" \
    --sndwnd "${SNDWND}" \
    -sockbuf "${SOCKBUF}" \
    --mtu "${MTU}"
done

# vim: set sw=2 ts=2 expandtab :
