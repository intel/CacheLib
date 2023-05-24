#!/bin/bash

if [ "$#" -ne 3 ]; then
    echo "ERROR: Pass DSA id, SHARED WQ id and ENGINE count as args."
    echo "e.g. 0 for dsa0, 1 for dsa0/wq0.1 and 4 for engine count"
    exit 1
fi

DEVID=${1}
SWQID=${2}
NENGS=${3}

DEV=dsa${DEVID}
SWQ=${DEV}/wq${DEVID}.${SWQID}

echo "=> ${SWQ}:"
sudo accel-config disable-wq ${SWQ}

echo "=> ${DEV}:"
sudo accel-config disable-device ${DEV}

for ((i=0; i < ${NENGS}; i++))
do
    echo "=> ${DEV}/engine${DEVID}.${i}"
    echo "configured"
    sudo accel-config config-engine ${DEV}/engine${DEVID}.${i} --group-id=0
done

sudo accel-config config-wq ${SWQ} --group-id=0
sudo accel-config config-wq ${SWQ} --priority=1
sudo accel-config config-wq ${SWQ} --wq-size=128
sudo accel-config config-wq ${SWQ} --max-batch-size=1024
sudo accel-config config-wq ${SWQ} --max-transfer-size=4194304
sudo accel-config config-wq ${SWQ} --block-on-fault=0
sudo accel-config config-wq ${SWQ} --type=user
sudo accel-config config-wq ${SWQ} --name="dsa-test"
sudo accel-config config-wq ${SWQ} --mode=shared
sudo accel-config config-wq ${SWQ} --threshold=127
sudo accel-config config-wq ${SWQ} --driver-name="user"

echo "=> ${DEV}:"
sudo accel-config enable-device ${DEV}

echo "=> ${SWQ}:"
sudo accel-config enable-wq ${SWQ}

