###############################################################################################
#                                                                                             #
# Author  :  Mohit Joshi                                                                      #
# Script  :  pstress_log_to_sql_converter.sh                                                  #
# Created :  05-May-2022                                                                      #
# Purpose :  The script is used to process pstress thread logs to filter out                  #
#            SQLs from it                                                                     #
#                                                                                             #
# Usage   :                                                                                   #
# ./pstress_log_to_sql_converter.sh --logdir <val>                                            #
# ./pstress_log_to_sql_converter.sh --logdir <val> --build-dir <val> --socket <val>           #
#                                                                                             #
# For more info:                                                                              #
# ./pstress_run_log_to_sql_converter.sh --help                                                #
###############################################################################################

# Helper Function
Help() {
  echo "usage:  $BASH_SOURCE --logfile <val> --build-dir <val> --socket <val>"
  echo "--logfile: Full path to the pstress log file"
  echo "--build-dir: Path to MySQL build/installation directory"
  echo "--socket: Path to socket file to connect to running server"
}

if [ "$#" -eq 0 ]; then
  Help
  exit
fi

ARGUMENT_LIST=(
    "logfile"
    "build-dir"
    "socket"
    "help"
)

# Read arguments
opts=$(getopt \
    --longoptions "$(printf "%s:," "${ARGUMENT_LIST[@]}" | sed 's/.\{2\}$//')" \
    --name "$(basename "$0")" \
    --options "" \
    -- "$@"
)

eval set --$opts

while true; do
    case "$1" in
    --help)
        Help
        exit
        ;;
    --build-dir)
	shift
	BUILD_DIR=$1
	break
	;;
    --socket)
	shift
	SOCKET=$1
	break
	;;
    --logfile)
        shift
	LOG_FILENAME=$1
	break
	;;
    --)
	shift
	Help
	exit
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
# To Execute SQLs against a running server set the BUILD_DIR and SOCKET path (Optional).
if [[ $BUILD_DIR != "" && $SOCKET != "" ]]; then
  MYSQL=$(find $BUILD_DIR -type f -name mysql | head -n1)
  $MYSQL -uroot -S$SOCKET -e "source $OUTPUT_FILENAME"

  if [ $? -eq 0 ]; then
    echo "Query execution successful. Check server logs for details"
  else
    echo "There is a failure while executing SQLs. Please check, if
  1. Server is running.
  2. You are using a stale server. In that case, please remove old datadir, and start a new server.
  3. The server has crashed while executing the SQLs. This is mostly a bug, please check server logs for details."
  fi
fi
