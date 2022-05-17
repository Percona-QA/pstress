###############################################################################################
#                                                                                             #
# Author  :  Mohit Joshi                                                                      #
# Script  :  pstress_log_to_sql_converter.sh                                                  #
# Created :  05-May-2022                                                                      #
# Purpose :  The script is used to process pstress thread logs to filter out                  #
#            SQLs from it                                                                     #
#                                                                                             #
# Usage   :                                                                                   #
# ./pstress_log_to_sql_converter.sh default.node.tld_step_1_thread-0.sql                      #
#                                                                                             #
# Update the BUILD_DIR & SOCKET information of the running server before executing the script #
# eg. BUILD_DIR=$HOME/mysql-8.0/bld_8.0.28/                                                   #
#     SOCKET=/tmp/mysql_22000.sock                                                            #
###############################################################################################

logFileName=$1

if [ ! -s $logFileName ]; then
  echo "Input File $logFileName is empty or does not exist. Exiting..."
  exit 1
fi

SCRIPT=$(readlink -f $0)
SCRIPT_PATH=`dirname $SCRIPT`
outputFileName=$SCRIPT_PATH/reduced.sql

echo "Reading the pstress logfile: $logFileName"

# Filtering out all the successfully executed SQLs from the pstress log
sed -n 's/.* S //p' $logFileName > $outputFileName

# Trimming off un-necessary information at the beginning of each SQL
sed -i 's/^ S //g' $outputFileName

# Trimming off un-necessary information at the end of each SQL
sed -i 's/rows:[0-9]*//g' $outputFileName

# Find the crashing query
crashQuery=$(awk '/Error Lost connection to MySQL server during query/{print prev} {prev=$0}' $logFileName | head -n1 | sed 's/.* F //')

# Append the crashing query at the end of output file
echo $crashQuery >> $outputFileName

# Adding semi-colon at the end of each SQL statement. In case, semi-colon
# already exists, then do not add twice
sed -i '/[^;] *$/s/$/;/' $outputFileName

echo "Converted SQL file can be found here: $outputFileName"

#################################################################################################################
# NOTE:                                                                                                         #
# 1. Make sure to start a fresh MySQL instance before running this script. Executing the SQL file on an already #
#    running server may have existing database objects (eg. general tablespaces, tables, etc) causing failures. #
#################################################################################################################
# To Execute SQLs against a running server set the BUILD_DIR and SOCKET path (Optional).
BUILD_DIR=
SOCKET=
if [[ $BUILD_DIR != "" && $SOCKET != "" ]]; then
  $BUILD_DIR/runtime_output_directory/mysql -uroot -S$SOCKET -e "source $outputFileName"

  if [ $? -eq 0 ]; then
    echo "Query execution successful. Check server logs for details"
  else
    echo "There is a failure while executing SQLs. Please check, if
  1. Server is running.
  2. You are using a stale server. In that case, please remove old datadir, and start a new server.
  3. The server has crashed while executing the SQLs. This is mostly a bug, please check server logs for details."
  fi
fi
