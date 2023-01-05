#!/bin/bash

COVERAGE_FILE='coverage.info'
COVERAGE_DIR='coverage_report'

lcov --capture --directory build-cachelib/ --output-file ${COVERAGE_FILE}
genhtml ${COVERAGE_FILE} -o ${COVERAGE_DIR}

# Cleanup
rm ${COVERAGE_FILE}

COVERAGE_REPORT="${COVERAGE_DIR}.tgz"
tar -zcvf ${COVERAGE_REPORT} ${COVERAGE_DIR}
rm -rf ${COVERAGE_DIR}

echo "Created coverage report ${COVERAGE_REPORT}"
