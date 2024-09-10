#!/bin/bash

mkdir -p "logs"

LOG_FILE_PATH_PREFIX="logs/rc_test_run_"
LOG_FILE_SUFFIX=".log"

for i in {1..10}; do
    rm -f "$LOG_FILE_PATH_PREFIX$i$LOG_FILE_SUFFIX"
done

echo "Test request coalescing: START"

for i in {1..10}; do
    ( echo -e "$(date '+%Y-%m-%d %H:%M:%S') - Iteration $i:\n" >> "$LOG_FILE_PATH_PREFIX$i$LOG_FILE_SUFFIX" &
      curl -v http://localhost:8000 >> "$LOG_FILE_PATH_PREFIX$i$LOG_FILE_SUFFIX" 2>&1 & )
done

wait

echo "Test request coalescing: SUCCESS"
