#!/usr/bin/env bash

source common.sh

# clone the latest gtest
[ -d googletest ] || git clone https://github.com/google/googletest.git

cd "$SCRIPT_DIR/../.." || die "cd fail"

# copy gtest source into lib/gtest folder
mkdir -p ./lib/gtest
cp -r -f -t ./lib/gtest "$PKG_DIR/googletest/googletest"/*

autoreconf --install
LD_LIBRARY_PATH="$INSTALL_DIR/lib:$LD_LIBRARY_PATH" \
    LD_RUN_PATH="$INSTALL_DIR/lib:$LD_RUN_PATH" \
    LDFLAGS="-L$INSTALL_DIR/lib $LDFLAGS" \
    CPPFLAGS="-I$INSTALL_DIR/include $CPPFLAGS" \
    FBTHRIFT_BIN="$INSTALL_DIR/bin/" \
    FBTHRIFT_TEMPLATES="$INSTALL_DIR/include/thrift/templates/" \
    ./configure --prefix="$INSTALL_DIR"
make $MAKE_ARGS && make install $MAKE_ARGS
