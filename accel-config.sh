#!/bin/bash

if [ "$#" -ne 1 ]; then
    echo "ERROR: Pass dsa id-num as arg. e.g. 0 for dsa0"
    exit 1
fi
DEVID=${1}
DEV=dsa${DEVID}
DEVWQ=${DEV}/wq${DEVID}.

MAXWQ=8
echo "INFO: WQ Count hardcoded to ${MAXWQ}"
MAXENGINE=4
echo "INFO: ENGINE Count hardcoded to ${MAXENGINE}"

for ((i=0; i < $MAXWQ; i++))
do
    echo "${DEVWQ}${i}:"
    sudo accel-config disable-wq ${DEVWQ}${i}
done
echo "${DEV}:"
sudo accel-config disable-device ${DEV}

for ((i=0; i < $MAXENGINE; i++))
do
    echo "Configure ${DEV}/engine${DEVID}.${i}"
    sudo accel-config config-engine ${DEV}/engine${DEVID}.${i} --group-id=0
done

for ((i=0; i < $MAXWQ; i++))
do
    sudo accel-config config-wq ${DEVWQ}${i} --group-id=0
    sudo accel-config config-wq ${DEVWQ}${i} --priority=5
    sudo accel-config config-wq ${DEVWQ}${i} --wq-size=16
    sudo accel-config config-wq ${DEVWQ}${i} --max-batch-size=128
    sudo accel-config config-wq ${DEVWQ}${i} --type=user
    sudo accel-config config-wq ${DEVWQ}${i} --name="dsa-test"
    sudo accel-config config-wq ${DEVWQ}${i} --mode=shared
    sudo accel-config config-wq ${DEVWQ}${i} --threshold=15
    sudo accel-config config-wq ${DEVWQ}${i} --driver-name="user"
done

echo "${DEV}:"
sudo accel-config enable-device ${DEV}
for ((i=0; i < $MAXWQ; i++))
do
    echo "${DEVWQ}${i}:"
    sudo accel-config enable-wq ${DEVWQ}${i}
done

