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
glibc-devel.i686

COPY ./install-cachelib-deps.sh ./install-cachelib-deps.sh
RUN ./install-cachelib-deps.sh
