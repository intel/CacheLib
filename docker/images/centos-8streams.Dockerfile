FROM quay.io/centos/centos:stream8

RUN dnf install -y \
cmake \
sudo \
git \
tzdata \
vim \
gdb \
clang \
python36 \
glibc-devel.i686 \
xmlto \
uuid \
libuuid-devel \
json-c-devel

COPY ./install-cachelib-deps.sh ./install-cachelib-deps.sh
RUN ./install-cachelib-deps.sh

COPY ./install-dsa-deps.sh ./install-dsa-deps.sh
RUN ./install-dsa-deps.sh
