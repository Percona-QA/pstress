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

ASSERTION_FLAG=0;
ERROR_FLAG=0;
IS_NEW_ASSERT=1;
IS_NEW_ERROR=1;
SEARCH_ERROR_PATTERN="\[ERROR\].*"
SEARCH_ASSERT_PATTERN="\[ERROR\].*Assertion.*";
SEARCH_FILE=exclude_patterns.txt

# Search assertion string in Error log
PATTERN=$(egrep "$SEARCH_ASSERT_PATTERN" $ERROR_LOG | sed -n -e 's/^.*\[ERROR\] //p');
if [ "$PATTERN" != "" ]; then
  ASSERT_STRING=$PATTERN;
  ASSERTION_FLAG=1;
fi

# Search error string in Error Log, excluding assertion failure error
PATTERN=$(egrep "$SEARCH_ERROR_PATTERN" $ERROR_LOG | grep -v "Assertion failure" | sed -n -e 's/^.*\[ERROR\] //p');
if [ "PATTERN" != "" ]; then
  ERROR_STRING=$PATTERN;
  ERROR_FLAG=1;
fi

# Search Assertion string in Known bug list
if [ $ASSERTION_FLAG -eq 1 ]; then
  while read SigTag
    do
      if [[ $STRING =~ ${SigTag} ]]; then
        echo "Known Bug reported in JIRA found. Please check the Bug status for more details";
        egrep -B1 "${SigTag}" $SEARCH_FILE
        IS_NEW_ASSERT=0
      fi
    done < <(egrep -v '^#|^$' $SEARCH_FILE)
fi

# Search Error string in Known bug list
if [ $ERROR_FLAG -eq 1 ]; then
  while read SigTag
    do
      if [[ $ERROR_STRING =~ ${SigTag} ]]; then
        echo "Some known ERROR(s) were found in the error log(s)";
        IS_NEW_ERROR=0
      fi
    done < <(egrep -v '^#|^$' $SEARCH_FILE)
fi

if [[ $ASSERTION_FLAG -eq 1 && $IS_NEW_ASSERT -eq 1 ]]; then
  echo "New Assertion has been found in the error log(s). Potentially a Bug, please investigate";
  echo $ASSERT_STRING
fi

if [[ $ERROR_FLAG -eq 1 && $IS_NEW_ERROR -eq 1 ]]; then
  echo "New Error has been found in error log(s). Potentially a Bug, please investigate";
  echo $ERROR_STRING
fi
