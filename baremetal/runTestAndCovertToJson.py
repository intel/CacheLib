#!/usr/bin/python3

import os
import sys
import subprocess
import re
import json
import pandas as pd
import time

gDsaDevList = ["0", "2", "4", "6"]

# Convert to int
def num(s):
    try:
        return int(s)
    except ValueError:
        return float(s)


# Parse the result
def parseResult(outputPath):
    with open(outputPath) as file:
        data = file.readlines()

    ## Parse config
    config = []
    seen = False
    for line in data:
        line.strip()
        if line == "{\n":
            seen = True
        elif line == "Welcome to OSS version of cachebench\n":
            break
        elif "reading distribution params from" in line or line == '\n':
            continue
        if seen:
            config.append(line)

    jconfig = json.loads("".join(config))

    ## Split cachebench and system results
    cbResults = []
    sysResults = []
    seen = 0
    for line in data:
        line.strip()
        if "== Test Results ==" in line:
            seen = 1
            continue
        elif "Performance counter stats for 'system wide'" in line:
            seen = 2
            continue
        elif "== KVReplayGenerator Stats ==" in line:
            seen = 0
            continue

        if seen == 1:
            cbResults.append(line)
        elif seen == 2:
            sysResults.append(line)

    ## Parse cachebench metrics
    cbMetrics = {}
    for line in cbResults:
        if ':' in line:
            key, value, *_ = [x.rstrip(', \n').replace(" ","_").lower() for x in line.split(':') if not len(x) == 2]
            key = re.sub('__+' , '_', key)
            key = re.sub('[^\d\w]', '', key)

            value = re.sub('[^0-9.]', '', value)
            value = num(value)

            cbMetrics[key] = value

    cbMetrics = {'cachebench_metrics': cbMetrics}

    ## Parse system metrics
    sysMetrics = {}
    for line in sysResults:
        if "Elapsed (wall clock) time" in line:
            continue

        elif "numactl" in line:
            value = line.split(':', 1)[-1].strip().replace('"','')
            sysMetrics["command"] = value

        elif "seconds time elapsed" in line:
            value, key = [x.strip().lower() for x in line.split('seconds')]
            sysMetrics["time_elapsed_in_secs"] = num(value.strip())

        elif ",event_category=0x" in line:
            value, key = [x.strip().lower() for x in line.split('dsa')]
            key = "dsa" + key.strip()
            sysMetrics[key] = num(value.strip())

        elif ':' in line:
            key, value, *_ = [x.rstrip(', \n').replace(" ","_").lower() for x in line.split(':') if not len(x) == 2]
            key = re.sub('__+' , '_', key)
            key = re.sub('[^\d\w]', '', key)
            value = re.sub('[^0-9.]', '', value)
            value = num(value)
            sysMetrics[key] = value

    sysMetrics = {'system_metrics': sysMetrics}

    ## Save the unified view
    dictionary = {**jconfig, **cbMetrics, **sysMetrics}
    return json.dumps(dictionary, indent=2)


# Update the dsa device setup and cachebench config
def updateConfig(config, dsaDevs, evictors, evictBatch, promoters, promoBatch):
    pwd = os.getcwd()
    dsaEnabled = str(True if dsaDevs > 0 else False).lower()
    for i in range(len(gDsaDevList)):
        if i < dsaDevs:
            cmd = [os.path.join(pwd, "baremetal/accelConfig.sh"), gDsaDevList[i], "yes"]
            subprocess.run(cmd)
        else:
            cmd = [os.path.join(pwd, "baremetal/accelConfig.sh"), gDsaDevList[i], "no"]
            subprocess.run(cmd)

    absPath = os.path.join(pwd, config)
    data = []
    with open(absPath, "r") as f:
        data = json.load(f)
        data["cache_config"]["dsaEnabled"]         = dsaEnabled
        data["cache_config"]["evictorThreads"]     = evictors
        data["cache_config"]["maxEvictionBatch"]   = evictBatch
        data["cache_config"]["promoterThreads"]    = promoters
        data["cache_config"]["maxPromotionBatch"] = promoBatch

    os.remove(absPath)
    with open(absPath, "w") as f:
        json.dump(data, f, indent=2)


# Run a test
def runTest(config, outputPath):
    timeCmd = ["time", "-v"]
    numaCmd = ["numactl","-N", "0"]
    perfCmd = ["perf", "stat", "-e",
               "dsa0/event=0x1,event_category=0x0/,\
                dsa0/event=0x10,event_category=0x1/,\
                dsa0/event=0x2,event_category=0x3/,\
                dsa2/event=0x1,event_category=0x0/,\
                dsa2/event=0x10,event_category=0x1/,\
                dsa2/event=0x2,event_category=0x3/,\
                dsa4/event=0x1,event_category=0x0/,\
                dsa4/event=0x10,event_category=0x1/,\
                dsa4/event=0x2,event_category=0x3/,\
                dsa6/event=0x1,event_category=0x0/,\
                dsa6/event=0x10,event_category=0x1/,\
                dsa6/event=0x2,event_category=0x3/,\
                dsa8/event=0x1,event_category=0x0/,\
                dsa8/event=0x10,event_category=0x1/,\
                dsa8/event=0x2,event_category=0x3/".replace(' \\', '').replace(' ', '')]
    cbCmd = ["opt/cachelib/bin/cachebench", "--json_test_config"]
    extraFlags = ["--report_api_latency", ">&", outputPath]
    recipe = timeCmd + numaCmd + perfCmd + cbCmd + [config] + extraFlags

    with open(outputPath, 'w') as f:
        ret = subprocess.run(recipe, stdout=f, stderr=f)

    return ret.check_returncode()


# Orchestrate config change, run the test and parse the result
def main():
    args = sys.argv[1:]
    if len(args) != 7:
        print('Invalid args. Required : config-file, dsa-device-count, evictor-threads, '
              'evictor-batch-size, promoter-threads, promoter-batch-size, output-path')
        exit()

    config     = args[0]
    dsaDevs    = num(args[1])
    evictors   = num(args[2])
    evictBatch = num(args[3])
    promoters  = num(args[4])
    promoBatch = num(args[5])
    outputPath = args[6]

    updateConfig(config, dsaDevs, evictors, evictBatch, promoters, promoBatch)

    outputTxt = outputPath + ".txt"
    runTest(config, outputTxt)

    outputJson = outputPath + ".json"
    jsonObject = parseResult(outputTxt)
    with open(outputJson, "w") as outfile:
        outfile.write(jsonObject)
    print("Test {0} complete".format(outputPath))

    time.sleep(15)

if __name__ == '__main__':
    main()


