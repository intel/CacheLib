#!/usr/bin/env bash
# Copyright 2023, Intel Corporation

git clone --recursive https://github.com/intel/DML.git
cd DML

mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build . --target install

cd ../../
rm -rf DML
