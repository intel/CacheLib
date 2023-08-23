#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022, Intel Corporation

echo 'Defaults env_keep += "HTTPS_PROXY https_proxy HTTP_PROXY http_proxy NO_PROXY no_proxy"' >> /etc/sudoers
./contrib/prerequisites-centos8.sh

for pkg in zstd googleflags googlelog googletest sparsemap fmt folly fizz wangle fbthrift ;
do
    sudo ./contrib/build-package.sh -j -I /opt/ "$pkg"
done

