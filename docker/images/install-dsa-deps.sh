#!/usr/bin/env bash
# Copyright 2023, Intel Corporation

# Install idxd-config
git clone https://github.com/intel/idxd-config.git
cd idxd-config
./autogen.sh
./configure CFLAGS='-g -O2' --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib64
make
make check
sudo make install
cd ../
rm -rf idxd-config

# Install DML Library
git clone --recursive https://github.com/intel/DML.git
cd DML
git checkout e44443c24d53552b248b9869b1b16f89cd970f52
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build . --target install
cd ../../
rm -rf DML
