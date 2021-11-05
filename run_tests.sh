#!/bin/bash

# Newline separated list of tests to ignore
BLACKLIST="allocator-test-AllocationClassTest
allocator-test-NvmCacheTests
common-test-TimeTests
common-test-UtilTests
shm-test-test_page_size"

find -type f \( -not -name "*bench*" -and -not -name "navy*" \) -executable | grep -vF "$BLACKLIST" | xargs -n1 bash -c
