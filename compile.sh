#!/usr/bin/env bash

set -x

: ${CXX:=c++}
: ${CC:=cc}
export CXX
export CC

if ! command -v meson; then
	echo "Missing meson" >&2
	exit 1
fi

if command -v nproc; then
	NPROC=$(nproc)
else
	NPROC=2
fi

setup() {
	meson setup __builddir__
}

compile() {
	meson compile -j$NPROC
}

if ! [ -d __builddir__ ]; then
	setup
fi
pushd __builddir__
compile
