FROM quay.io/centos/centos:stream9

RUN dnf install -y \
cmake \
sudo \
git \
tzdata \
vim \
gdb \
clang \
xmlto \
uuid \
libuuid-devel \
perf \
numactl

RUN dnf -y install gcc-toolset-12
RUN echo "source /opt/rh/gcc-toolset-12/enable" >> /etc/bashrc

SHELL ["/bin/bash", "--login", "-c"]

COPY ./contrib ./contrib
COPY ./docker ./docker
COPY ./cachelib/external ./cachelib/external
RUN ls ./cachelib/external/
RUN ./docker/images/install-cachelib-deps.sh
RUN ./docker/images/install-dsa-deps.sh
