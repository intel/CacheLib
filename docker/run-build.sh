#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022, Intel Corporation

source /opt/rh/gcc-toolset-12/enable
cd /
if [ -d "build-cachelib" ]; then
  rm -rf build-cachelib
fi
./$WORKDIR/contrib/build-package.sh -t -j -v -I /opt/ cachelib
cd /opt/tests && $WORKDIR/run_tests.sh
