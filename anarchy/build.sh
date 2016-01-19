#!/bin/bash
set -e
if [ `uname` == "Darwin" ]; then
    echo "Not building libdispatch on Darwin; using system libdispatch"
    exit 0
fi

#install dependencies only if we don't have them
deps=("make" "gobjc" "automake" "autoconf" "libtool" "pkg-config" "systemtap-sdt-dev" "libblocksruntime-dev" "libkqueue-dev" "libpthread-workqueue-dev" "libbsd-dev")
install_deps() {
    apt-get update
    apt-get install --no-install-recommends -y ${deps[@]}
}
dpkg -s "${deps[@]}" >/dev/null 2>&1 || install_deps

if [ ! -f configure ]; then
    sh autogen.sh
fi
builddir=`pwd`/build
if [ ! -f Makefile ]; then
    CC=clang-3.5 ./configure --prefix=$builddir
fi
if [ ! -f src/privder.h ]; then
    cd src && dtrace -h -s provider.d
fi
if [ ! -f $builddir/lib/libdispatch.so ]; then
    make install -j8
fi