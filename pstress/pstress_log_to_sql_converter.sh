###############################################################################################
#                                                                                             #
# Author  :  Mohit Joshi                                                                      #
# Script  :  pstress_log_to_sql_converter.sh                                                  #
# Created :  05-May-2022                                                                      #
# Purpose :  The script is used to process pstress thread logs to filter out                  #
#            SQLs from it                                                                     #
#                                                                                             #
# Usage   :                                                                                   #
# 1. The user can pass the path of pstress log file as shown below to convert it in SQL file  #
# ./pstress_log_to_sql_converter.sh --logdir <val>                                            #
#                                                                                             #
# 2. The user can use this script to also execute the converted SQL file against a running    #
# server by setting the path --mysql-client and --socket                                      #
# ./pstress_log_to_sql_converter.sh --logdir <val> --mysql-client <val> --socket <val>        #
#                                                                                             #
# For more info:                                                                              #
# ./pstress_run_log_to_sql_converter.sh --help                                                #
###############################################################################################

# Helper Function
Help() {
  echo "usage: $BASH_SOURCE --logfile <val>"
  echo "usage: $BASH_SOURCE --logfile <val> --mysql-client <val> --socket <val>"
  echo "--logfile: Full path to the pstress log file"
  echo "--mysql-client: Path to MySQL client"
  echo "--socket: Path to socket file to connect to running server"
  echo "--user: Database username. If not provided, it defaults to root user"
}

if [ "$#" -eq 0 ]; then
  Help
  exit
fi

ARGUMENT_LIST=(
    "logfile"
    "mysql-client"
    "socket"
    "user"
    "help"
)

# Read arguments
opts=$(getopt \
    --longoptions "$(printf "%s:," "${ARGUMENT_LIST[@]}" | sed 's/.\{2\}$//')" \
    --name "$(basename "$0")" \
    --options "" \
    -- "$@"
)

if [ $? -ne 0 ]; then
  echo "Invalid option. Exiting..."
  exit 1
fi

eval set --$opts

while true; do
    case "$1" in
    --help)
        Help
        exit
        ;;
    --mysql-client)
	shift
	MYSQL=$1
	;;
    --socket)
	shift
	SOCKET=$1
	;;
    --logfile)
        shift
	LOG_FILENAME=$1
	;;
    --user)
	shift
	USER_NAME=$1
	;;
    --)
	break
	;;
    esac
    shift
done

if [ ! -s $LOG_FILENAME ]; then
  echo "Input File $LOG_FILENAME is empty or does not exist. Exiting..."
  exit 1
fi

SCRIPT=$(readlink -f $0)
SCRIPT_PATH=$(dirname $SCRIPT)
LOGFILE=$(basename $LOG_FILENAME)
OUTPUT_FILENAME=$SCRIPT_PATH/reduced_"$LOGFILE"

echo "Reading the pstress logfile: $LOG_FILENAME"

# Filtering out all the successfully executed SQLs from the pstress log
sed -n 's/.* S //p' $LOG_FILENAME > $OUTPUT_FILENAME

# Trimming off un-necessary information at the beginning of each SQL
sed -i 's/^ S //g' $OUTPUT_FILENAME

# Trimming off un-necessary information at the end of each SQL
sed -i 's/rows:[0-9]*//g' $OUTPUT_FILENAME

# Find the crashing query
crashQuery=$(awk '/Error Lost connection to MySQL server during query/{print prev} {prev=$0}' $LOG_FILENAME | head -n1 | sed 's/.* F //')

# Append the crashing query at the end of output file
echo $crashQuery >> $OUTPUT_FILENAME

# Adding semi-colon at the end of each SQL statement. In case, semi-colon
# already exists, then do not add twice
sed -i '/[^;] *$/s/$/;/' $OUTPUT_FILENAME

echo "Converted SQL file can be found here: $OUTPUT_FILENAME"

#################################################################################################################
# NOTE:                                                                                                         #
# 1. Make sure to start a fresh MySQL instance before running this script. Executing the SQL file on an already #
#    running server may have existing database objects (eg. general tablespaces, tables, etc) causing failures. #
#################################################################################################################
# To Execute SQLs against a running server set the MYSQL and SOCKET path (Optional).
if [[ $MYSQL != "" && $SOCKET != "" ]]; then
  if [ "$USER_NAME" == "" ]; then USER_NAME=root; fi
  $MYSQL -u$USER_NAME -p -S$SOCKET -e "source $OUTPUT_FILENAME"

  if [ $? -eq 0 ]; then
    echo "Query execution successful. Check server logs for details"
  else
    echo "There is a failure while executing SQLs. Please check, if
  1. Server is running.
  2. You are using a stale server. In that case, please remove old datadir, and start a new server.
  3. The server has crashed while executing the SQLs. This is mostly a bug, please check server logs for details."
  fi
fi
