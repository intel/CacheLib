#!/usr/bin/python3

import sys
import json
import re
import os
import pandas as pd
import csv


def listJson(path, filterStr):
    jsonFiles = [jsonFile for jsonFile in os.listdir(path) if jsonFile.endswith('.json') and filterStr in jsonFile]
    return [path + f for f in jsonFiles]


def getMetrics(files, cbCols, sysCols):
    values = {}
    for entry in files:
        with open(entry, 'r') as jsonFile :
            data = json.load(jsonFile)
        key = os.path.basename(entry).rsplit(".", 1)[0]
        values[key] = {}
        for col in cbCols:
            values[key][col] = data['cachebench_metrics'][col]
        for col in sysCols:
            values[key][col] = data['system_metrics'][col]
    return values


def main():
    args = sys.argv[1:]
    if len(args) < 1 or len(args) > 2:
        print("Invalid Args. Required : path, filter-string")
        exit()

    path      = args[0]
    filterStr = args[1] if len(args) == 2 else ''
    files     = listJson(path, filterStr)

    cbCols = [
                'cache_allocate_api_latency_p90_in_ns',
                'cache_allocate_api_latency_p99_in_ns',
                'cache_find_api_latency_p90_in_ns',
                'cache_find_api_latency_p99_in_ns',
                'cache_background_eviction_latency_p90_in_ns',
                'cache_background_eviction_latency_p99_in_ns',
                'cache_evict_dml_large_item_wait_latency_p90_in_ns',
                'cache_evict_dml_large_item_wait_latency_p99_in_ns',
                'cache_evict_dml_small_item_wait_latency_p90_in_ns',
                'cache_evict_dml_small_item_wait_latency_p99_in_ns',
                'cache_background_promotion_latency_p90_in_ns',
                'cache_background_promotion_latency_p99_in_ns',
                'cache_promote_dml_large_item_wait_latency_p90_in_ns',
                'cache_promote_dml_large_item_wait_latency_p99_in_ns',
                'cache_promote_dml_small_item_wait_latency_p90_in_ns',
                'cache_promote_dml_small_item_wait_latency_p99_in_ns'
            ]

    sysCols = [
                'dsa0/event=0x1,event_category=0x0/',
                'dsa0/event=0x10,event_category=0x1/',
                'dsa0/event=0x2,event_category=0x3/',
                'time_elapsed_in_secs',
                'user_time_seconds',
                'percent_of_cpu_this_job_got'
            ]
    metrics = getMetrics(files, cbCols, sysCols)

    ''' Save metrics to csv '''
    fields = ['test'] + cbCols + sysCols
    csvFile = os.path.join(path , 'metrics.' + filterStr + '.csv')
    with open(csvFile, 'w') as f:
        w = csv.DictWriter(f, fields)
        w.writeheader()
        for key, val in sorted(metrics.items()):
            row = {'test': key}
            row.update(val)
            w.writerow(row)
    print("Filter: {0} ; Results gathered in {1}".format(filterStr, csvFile))


if __name__ == '__main__':
    main()
