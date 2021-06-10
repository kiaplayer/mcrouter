#!/usr/bin/env bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

source common.sh

if [[ ! -d "$PKG_DIR/fbthrift" ]]; then
  git clone https://github.com/facebook/fbthrift
  cd "$PKG_DIR/fbthrift" || die "cd fail"
  git checkout v2021.05.24.00
fi

cd "$PKG_DIR/fbthrift/build" || die "cd fbthrift failed"

CXXFLAGS="$CXXFLAGS -fPIC" \
cmake .. -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
make $MAKE_ARGS && make install $MAKE_ARGS
