#!/bin/bash


# Check whether you're in the right directory
CHECK_DIR="baremetal"
if [ ! -d "${CHECK_DIR}" ]; then
  echo "You're not in the right directory. You need to be on the base CacheLib directory for the script to work."
  exit 1
fi

# Check whether CacheLib was installed using contrib/build.sh
if [ ! -f "opt/cachelib/bin/cachebench" ]; then
  echo "CacheLib was not installed using contrib/build.sh."
  echo "runTestAndCovertToJson.py would need modification otherwise"
  exit 1
fi

CDN_DIR="${CHECK_DIR}/cdn"
NOW=$( date '+%F_%H:%M:%S' )
RES_DIR="${CDN_DIR}/data-${NOW}/"
mkdir ${RES_DIR}
echo "Create results directory ${RES_DIR}."


# Run tests for DSA = 0
# Evictor threads = 4, 8, 12, 16 or 20
# Eviction batch size = 100
# Promoter threads = 0
# Promotion batch size = 0
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0  4 100  0   0  ${RES_DIR}/dsa0_et04_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0  8 100  0   0  ${RES_DIR}/dsa0_et08_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100  0   0  ${RES_DIR}/dsa0_et12_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 16 100  0   0  ${RES_DIR}/dsa0_et16_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 20 100  0   0  ${RES_DIR}/dsa0_et20_eb100_pt00_pb000

# Run tests for DSA = 4
# Evictor threads = 4, 8, 12, 16 or 20
# Eviction batch size = 100
# Promoter threads = 0
# Promotion batch size = 0
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4  4 100  0   0  ${RES_DIR}/dsa4_et04_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4  8 100  0   0  ${RES_DIR}/dsa4_et08_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100  0   0  ${RES_DIR}/dsa4_et12_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 16 100  0   0  ${RES_DIR}/dsa4_et16_eb100_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 20 100  0   0  ${RES_DIR}/dsa4_et20_eb100_pt00_pb000

# Aggregate results for eb100_pt00_pb000
./${CHECK_DIR}/aggregateAndFilterTestResults.py ${RES_DIR} eb100_pt00_pb000


# Run tests for DSA = 0
# Evictor threads = 12
# Eviction batch size = 100
# Promoter threads = 4, 8, 12, 16 or 20
# Promotion batch size = 100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100  4 100  ${RES_DIR}/dsa0_et12_eb100_pt04_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100  8 100  ${RES_DIR}/dsa0_et12_eb100_pt08_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100 12 100  ${RES_DIR}/dsa0_et12_eb100_pt12_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100 16 100  ${RES_DIR}/dsa0_et12_eb100_pt16_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100 20 100  ${RES_DIR}/dsa0_et12_eb100_pt20_pb100

# Run tests for DSA = 4
# Evictor threads = 12
# Eviction batch size = 100
# Promoter threads = 4, 8, 12, 16 or 20
# Promotion batch size = 100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100  4 100  ${RES_DIR}/dsa4_et12_eb100_pt04_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100  8 100  ${RES_DIR}/dsa4_et12_eb100_pt08_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100 12 100  ${RES_DIR}/dsa4_et12_eb100_pt12_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100 16 100  ${RES_DIR}/dsa4_et12_eb100_pt16_pb100
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100 20 100  ${RES_DIR}/dsa4_et12_eb100_pt20_pb100

# Aggregate results for pb100
./${CHECK_DIR}/aggregateAndFilterTestResults.py ${RES_DIR} pb100


# Run tests for DSA = 0
# Evictor threads = 4, 8, 12, 16 or 20
# Eviction batch size = 200
# Promoter threads = 0
# Promotion batch size = 0
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0  4 200  0   0  ${RES_DIR}/dsa0_et04_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0  8 200  0   0  ${RES_DIR}/dsa0_et08_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 200  0   0  ${RES_DIR}/dsa0_et12_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 16 200  0   0  ${RES_DIR}/dsa0_et16_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 20 200  0   0  ${RES_DIR}/dsa0_et20_eb200_pt00_pb000

# Run tests for DSA = 4
# Evictor threads = 4, 8, 12, 16 or 20
# Eviction batch size = 200
# Promoter threads = 0
# Promotion batch size = 0
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4  4 200  0   0  ${RES_DIR}/dsa4_et04_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4  8 200  0   0  ${RES_DIR}/dsa4_et08_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 200  0   0  ${RES_DIR}/dsa4_et12_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 16 200  0   0  ${RES_DIR}/dsa4_et16_eb200_pt00_pb000
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 20 200  0   0  ${RES_DIR}/dsa4_et20_eb200_pt00_pb000

# Aggregate results for eb200_pt00_pb000
./${CHECK_DIR}/aggregateAndFilterTestResults.py ${RES_DIR} eb200_pt00_pb000


# Run tests for DSA = 0
# Evictor threads = 12
# Eviction batch size = 100
# Promoter threads = 4, 8, 12, 16 or 20
# Promotion batch size = 200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100  4 200  ${RES_DIR}/dsa0_et12_eb100_pt04_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100  8 200  ${RES_DIR}/dsa0_et12_eb100_pt08_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100 12 200  ${RES_DIR}/dsa0_et12_eb100_pt12_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100 16 200  ${RES_DIR}/dsa0_et12_eb100_pt16_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 0 12 100 20 200  ${RES_DIR}/dsa0_et12_eb100_pt20_pb200

# Run tests for DSA = 4
# Evictor threads = 12
# Eviction batch size = 100
# Promoter threads = 4, 8, 12, 16 or 20
# Promotion batch size = 200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100  4 200  ${RES_DIR}/dsa4_et12_eb100_pt04_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100  8 200  ${RES_DIR}/dsa4_et12_eb100_pt08_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100 12 200  ${RES_DIR}/dsa4_et12_eb100_pt12_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100 16 200  ${RES_DIR}/dsa4_et12_eb100_pt16_pb200
./${CHECK_DIR}/runTestAndCovertToJson.py ${CDN_DIR}/config.json 4 12 100 20 200  ${RES_DIR}/dsa4_et12_eb100_pt20_pb200

# Aggregate results for pb200
./${CHECK_DIR}/aggregateAndFilterTestResults.py ${RES_DIR} pb200

