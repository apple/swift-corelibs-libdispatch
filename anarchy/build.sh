#!/bin/bash
set -e
if [ `uname` == "Darwin" ]; then
    echo "Not building libdispatch on Darwin"
    exit 0
fi

apt-get update
apt-get install make gobjc automake git ca-certificates autoconf libtool pkg-config systemtap-sdt-dev libblocksruntime-dev libkqueue-dev libpthread-workqueue-dev libbsd-dev --no-install-recommends -y
sh autogen.sh
CC=clang-3.5 ./configure --prefix=`pwd`/build
cd src && dtrace -h -s provider.d
make install -j8
