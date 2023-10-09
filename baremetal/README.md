# Bare-metal Testing Scripts


> ### accelConfig.sh
>
>> Set up DSA devices using accel-config.
>> - OPTIONAL Arg-1: DSA device id. Default: 0
>> - OPTIONAL Arg-2: Enable/Disable DSA device. Default: yes
>> - OPTIONAL Arg-3: SHARED WQ id. Default: 1
>> - OPTIONAL Arg-4: ENGINE count. Default: 4
>> - OUTPUT Verify DSA devices set up is correct using accel-config


> ### runTestAndConvertToJson.py
>
>> Run a single test and convert the output TXT to CSV.
>> This script uses numactl to bind all threads to node 0.
>> Run with **sudo**
>> - REQUIRED Arg-1: Cachebench config file path from root directory
>> - REQUIRED Arg-2: DSA device count
>> - REQUIRED Arg-3: Number of background evictors
>> - REQUIRED Arg-4: Eviction batch size
>> - REQUIRED Arg-5: Number of background promoters
>> - REQUIRED Arg-6: Promotion batch size
>> - REQUIRED Arg-7: Output path
>> - OUTPUT txt and json saved in same path


> ### aggregateAndFilterTestResults.py
>
>> Gather all output JSON using the file name filter
>> Run with **sudo**
>> - REQUIRED Arg-1: Output json directory path
>> - REQUIRED Arg-2: Filter filenames using this string. Pass null str to gather all files.
>> - OUTPUT Saved in a csv on the output JSON directory path


> ### parseTestResultIntoCsv.py
>
>> Parse TXT output and save in JSON format
>> - REQUIRED Arg-1: output txt file name
>> - REQUIRED Arg-2: Tag a string for the text
>> - OUTPUT Save CSV to the same path


> ### sample.sh
>
>> This shows how to create a test sequence, then parses results and aggregates it.
>> Run with **sudo**


