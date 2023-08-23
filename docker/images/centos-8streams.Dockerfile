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
json-c-devel \
perf \
numactl

# updated to fix compile errors and better symbol
# resolving in VTune
RUN dnf -y install gcc-toolset-12
RUN echo "source /opt/rh/gcc-toolset-12/enable" >> /etc/bashrc
SHELL ["/bin/bash", "--login", "-c"]

COPY ./contrib ./contrib
COPY ./docker ./docker
COPY ./cachelib/external ./cachelib/external

RUN ./docker/images/install-cachelib-deps.sh
RUN ./docker/images/install-dsa-deps.sh
