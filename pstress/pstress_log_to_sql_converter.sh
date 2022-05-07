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
# Update the BASEDIR and SOCKET information of the running server before executing the script #
#                                                                                             #
###############################################################################################

logFileName=$1

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
# 2. Comment SQL execution section(below) in case pstress logs are generated in multi-threaded mode. Reason     #
#    being that, some database objects might be created in another thread causing SQLs to fail.                 #
#################################################################################################################
# Executing SQLs against a running server (Optional)
BASEDIR=$HOME/mysql-8.0/bld_8.0.28/install
SOCKET=/tmp/mysql_22000.sock
$BASEDIR/bin/mysql -uroot -S$SOCKET -e "source $outputFileName"

