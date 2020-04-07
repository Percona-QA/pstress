############################################################################################
# Created by Mohit Joshi, Percona LLC                                                      #
# Creation date: 07-APR-2020                                                               #
#                                                                                          #
# The file is created to report errors/assertions found in error logs during pstress runs. #
# The file uses exclude_patterns.txt to ignore known Bugs/Crashes/Errors.                  #
# Both the files search_string.sh and exclude_patterns must exist in the same directory.   #
# Do not move the files from <pstress-repo>/pstress directory.                             #
############################################################################################

#!/bin/bash

ERROR_LOG=$1
if [ "$ERROR_LOG" == "" ]; then
  if [ -r ./log/master.err ]; then
    ERROR_LOG=./log/master.err
  else
    echo "$0 failed to extract string from an error log, as no error log file name was passed to this script"
    exit 1
  fi
fi

SEARCH_ERROR_PATTERN="\[ERROR\].*"
SEARCH_FOR_ASSERT="\[ERROR\].*Assertion.*";
EXCLUDE_SEARCH_PATTERN="";

# Fetch the Signature tags from exclude_patterns.txt
while read SigTag 
do
 EXCLUDE_SEARCH_PATTERN+=" ${SigTag}|"
done < <(egrep -v '^#|^$' exclude_patterns.txt)

# Get rid of the last pipe (|) from the exclude string
EXCLUDE_SEARCH_PATTERN=$(echo "${EXCLUDE_SEARCH_PATTERN:: -1}")

STRING=$(egrep "$SEARCH_FOR_ASSERT" $ERROR_LOG | egrep -v "$EXCLUDE_SEARCH_PATTERN" | sed -n -e 's/^.*\[ERROR\] //p');

if [ "$STRING" != "" ]; then
  echo "The server crashed with an assertion! Please check the error log(s) for all warnings/errors"
fi

if [ "$STRING" == "" ]; then
 STRING=$(egrep "$SEARCH_ERROR_PATTERN" $ERROR_LOG | egrep -v "$EXCLUDE_SEARCH_PATTERN" | sed -n -e 's/^.*\[ERROR\] //p');
fi

# Filter out accidental thread <nr> insertions
echo "$STRING" | sed "s| thread [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]||"
