#!/bin/bash

#Build CacheLib with flag -DCOVERAGE_ENABLED=ON

# Track coverage
lcov -c -i -b . -d . -o Coverage.baseline
./run_tests.sh
lcov -c -d . -b . -o Coverage.out
lcov -a Coverage.baseline -a Coverage.out -o Coverage.combined

# Generate report
COVERAGE_DIR='coverage_report'
genhtml Coverage.combined -o ${COVERAGE_DIR}
COVERAGE_REPORT="${COVERAGE_DIR}.tgz"
tar -zcvf ${COVERAGE_REPORT} ${COVERAGE_DIR}
echo "Created coverage report ${COVERAGE_REPORT}"

# Cleanup
rm Coverage.baseline Coverage.out Coverage.combined
rm -rf ${COVERAGE_DIR}
