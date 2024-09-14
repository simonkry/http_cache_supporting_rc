#!/bin/bash

# Check if the parameter NUM_OF_REQUESTS is provided
if [ -z "$1" ]; then
    echo "Error: Please provide the number of requests as the parameter"
    echo "Usage: $0 <NUM_OF_REQUESTS>"
    exit 1
fi

mkdir -p "logs"

LOG_FILE_PATH_PREFIX="logs/rc_test_run_"
LOG_FILE_PATH_PREFIX_DOCS="logs/docs_rc_test_run_"
LOG_FILE_PATH_PREFIX_COMM="logs/community_rc_test_run_"
LOG_FILE_PATH_PREFIX_TRAI="logs/training_rc_test_run_"
LOG_FILE_SUFFIX=".log"

rm -f "$LOG_FILE_PATH_PREFIX"*
rm -f "$LOG_FILE_PATH_PREFIX_DOCS"*
rm -f "$LOG_FILE_PATH_PREFIX_COMM"*
rm -f "$LOG_FILE_PATH_PREFIX_TRAI"*

echo "Test request coalescing: START"
echo "------------------------------"

START_TIME=$(date +%s%3N)

# Connect to the server NUM_OF_REQUESTS times in the same moment and output to log files
for i in $(seq 1 "$1"); do
    ( curl -v http://localhost:8000 >> "$LOG_FILE_PATH_PREFIX$i$LOG_FILE_SUFFIX" 2>&1 &
      curl -v http://localhost:8000/docs >> "$LOG_FILE_PATH_PREFIX_DOCS$i$LOG_FILE_SUFFIX" 2>&1 &
      curl -v http://localhost:8000/community >> "$LOG_FILE_PATH_PREFIX_COMM$i$LOG_FILE_SUFFIX" 2>&1 &
      curl -v http://localhost:8000/training >> "$LOG_FILE_PATH_PREFIX_TRAI$i$LOG_FILE_SUFFIX" 2>&1 & )
done

wait

END_TIME=$(date +%s%3N)
EXECUTION_TIME=$((END_TIME - START_TIME))

echo "Time taken: $EXECUTION_TIME ms"
echo "Test request coalescing: DONE, check log files"
