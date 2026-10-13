#!/bin/sh
# Build the Blackfin cross compiler without Docker: the same versions as toolchain/Dockerfile (binutils 2.40,
# gcc 12.3, C only, with libgcc), so `make_firmware.py --native` gives the same bytes as the Docker image.
#   toolchain/build_native.sh [PREFIX]          (default PREFIX: toolchain/bfin, git-ignored)
#   export PATH=$PREFIX/bin:$PATH && cd synth_sdk && python3 make_firmware.py --native
# Needs a host C/C++ compiler, make, curl or wget, xz. About 30 minutes on 4 cores.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "${1:-$HERE/bfin}"
PREFIX=$(cd "${1:-$HERE/bfin}" && pwd)
J=$(nproc 2>/dev/null || echo 2)
SRC=$PREFIX/src
mkdir -p "$SRC" && cd "$SRC"
get() { [ -f "$(basename "$1")" ] || { command -v curl > /dev/null && curl -sSLO "$1" || wget -q "$1"; }; }

get https://ftp.gnu.org/gnu/binutils/binutils-2.40.tar.xz
[ -d binutils-2.40 ] || tar xf binutils-2.40.tar.xz
mkdir -p build-binutils && cd build-binutils
../binutils-2.40/configure --target=bfin-elf --prefix="$PREFIX" --disable-nls --disable-werror --disable-gdb \
    --disable-sim MAKEINFO=true > configure.log
make -j"$J" MAKEINFO=true > make.log              # MAKEINFO=true: no texinfo needed (docs skipped)
make MAKEINFO=true install > install.log
cd "$SRC"

get https://ftp.gnu.org/gnu/gcc/gcc-12.3.0/gcc-12.3.0.tar.xz
[ -d gcc-12.3.0 ] || { tar xf gcc-12.3.0.tar.xz && (cd gcc-12.3.0 && ./contrib/download_prerequisites > /dev/null); }
mkdir -p build-gcc && cd build-gcc
PATH=$PREFIX/bin:$PATH ../gcc-12.3.0/configure --target=bfin-elf --prefix="$PREFIX" --enable-languages=c \
    --without-headers --with-newlib --disable-nls --disable-libssp --disable-libquadmath --disable-libgomp \
    --disable-shared --disable-threads MAKEINFO=true > configure.log
PATH=$PREFIX/bin:$PATH make -j"$J" all-gcc all-target-libgcc MAKEINFO=true > make.log 2>&1
PATH=$PREFIX/bin:$PATH make install-gcc install-target-libgcc MAKEINFO=true > install.log
rm -rf "$SRC"
echo "export PATH=$PREFIX/bin:\$PATH"
