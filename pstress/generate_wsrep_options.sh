#####################################################################################
#!/bin/bash                                                                         #
# Created by Puneet Kaushik, Percona LLC                                               #
# Created on: 28-JUNE-2023                                                           #
#                                                                                   #
# Purpose: This script auto-generates the wsrep_options.txt file                  #
# Usage: ./generate_wsrep_options.sh <path to mysql install/base dir>             #
# References: https://docs.percona.com/percona-xtradb-cluster/8.0/wsrep-system-index.html #
#####################################################################################

# Script expected argument
BASEDIR=$1

# User variables
OUTPUT_FILE=/tmp/mysqld_options_pxc_wsrep_80.txt

# Internal variables, do not change
TEMP_FILE=/tmp/mysqld_options_pxc_wsrep_80.tmp
PORT=22000
SOCKET=/tmp/mysql_22000.sock
DATADIR=$BASEDIR/data
MYSQLD_START_TIMEOUT=30

# Check if mysqld binaries exists
if [ ! -r $BASEDIR/bin/mysqld ]; then
    echo "Error: no ./bin/mysqld or ./mysqld found!"
    exit 1
fi

# Display function
echoit(){
  echo "[$(date +'%T')] $1"
  echo "[$(date +'%T')] $1" >> /tmp/generate_wsrep_options.log
}

echoit "Starting MySQL server"
$BASEDIR/bin/mysqld --no-defaults --datadir=$DATADIR --initialize-insecure > /tmp/mysql_install_db.txt 2>&1
$BASEDIR/bin/mysqld --no-defaults --datadir=$DATADIR --port=$PORT --socket=$SOCKET 2>&1 > /tmp/master.err 2>&1 &
MPID=$!

echoit "Waiting for the server to fully start..."
for X in $(seq 0 ${MYSQLD_START_TIMEOUT}); do
  sleep 1
  if [ "$MPID" == "" ]; then echoit "Assert! ${MPID} empty. Terminating!"; exit 1; fi
  if $BASEDIR/bin/mysqladmin -uroot -S${SOCKET} ping > /dev/null 2>&1; then
    echoit "MySQL server started successfully"
    break
  fi
done

# Extract all options, their default values into temp file
$BASEDIR/bin/mysql --no-defaults -S$SOCKET -uroot -e "SHOW VARIABLES LIKE '%pxc%'" > $TEMP_FILE
$BASEDIR/bin/mysql --no-defaults -S$SOCKET -uroot -e "SHOW VARIABLES LIKE '%wsrep%'" >> $TEMP_FILE

# Remove the first line from the temp file
# Variable_name, Variable_value (we do not need the column name displayed in temp file)
sed -i '1d;6d' $TEMP_FILE
sed -i 's/_/\-/g' $TEMP_FILE

# List of rocksdb variables which must not be changed.
EXCLUDED_LIST=( --wsrep-applier-FK-checks --wsrep-applier-UK-checks --wsrep-causal-reads --wsrep-cluster-address --wsrep-cluster-name --wsrep-data-home-dir --wsrep-dbug-option --wsrep-drupal-282555-workaround --wsrep-forced-binlog-format --wsrep-ignore-apply-errors --wsrep-min-log-verbosity --wsrep-node-address --wsrep-node-incoming-address --wsrep-node-name --wsrep-notify-cmd --wsrep-provider --wsrep-provider-options --wsrep-restart-slave --wsrep-slave-FK-checks --wsrep-slave-threads --wsrep-slave-UK-checks --wsrep-sst-donor --wsrep-sst-method --wsrep-sst-receive-address --wsrep-start-position --wsrep-disk-pages-encrypt --wsrep-gcache-encrypt --wsrep-SR-store --wsrep-sst-allowed-methods)

# Create an output file which contains all the options/values
rm -rf $OUTPUT_FILE
touch $OUTPUT_FILE

while read line; do
  OPTION="--$(echo ${line} | awk '{print $1}')"
  VALUE="$(echo ${line} | awk '{print $2}' | sed 's|^[ \t]*||;s|[ \t]*$||')"	
  if [ "$VALUE" == "(No" ]; then
    echoit "Working on option '$OPTION' which has no default value..."
  else
    echoit "Working on option '$OPTION' with default value '$VALUE'..."
  fi
  if [[ " ${EXCLUDED_LIST[@]} " =~ " ${OPTION} " ]]; then
    echoit "Option '$OPTION' is logically excluded from being handled by this script..."
  elif [[ "$OPTION" == "--pxc-encrypt-cluster-traffic" ]]; then
    echoit " > Adding possible values 0 and 1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
  elif [[ "$OPTION" == "--pxc-maint-mode" ]]; then
    echoit " > Adding possible values disabled, shutdown and maintainance for option '${OPTION}' to the final list..."
    echo "$OPTION=DISABLED" >> $OUTPUT_FILE
    echo "$OPTION=SHUTDOWN" >> $OUTPUT_FILE  
    echo "$OPTION=MAINTENANCE" >> $OUTPUT_FILE
  elif [[ "$OPTION" == "--pxc-maint-transition-period" ]]; then
    echoit " > Adding possible values 0,2,10,16 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=2" >> $OUTPUT_FILE
    echo "$OPTION=10" >> $OUTPUT_FILE
    echo "$OPTION=16" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--pxc-strict-mode" ]]; then
    echoit " > Adding possible values DISABLED, PERMISSIVE, ENFORCING, MASTER for option '${OPTION}' to the final list..."
    echo "$OPTION=ENFORCING" >> $OUTPUT_FILE
    echo "$OPTION=DISABLED" >> $OUTPUT_FILE
    echo "$OPTION=PERMISSIVE" >> $OUTPUT_FILE
    echo "$OPTION=MASTER" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-applier-threads" ]]; then
    echoit " > Adding possible values 1,2,12,24 for option '${OPTION}' to the final list..."
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=2" >> $OUTPUT_FILE
    echo "$OPTION=12" >> $OUTPUT_FILE
    echo "$OPTION=24" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-auto-increment-control" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-certification-rules" ]]; then
    echoit " > Adding possible values STRICT, OPTIMIZED for option '${OPTION}' to the final list..."
    echo "$OPTION=STRICT" >> $OUTPUT_FILE
    echo "$OPTION=OPTIMIZED" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-certify-nonPK" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-debug" ]]; then
    echoit " > Adding possible values NONE, SERVER, TRANSACTION, STREAMING , CLIENT  for option '${OPTION}' to the final list..."
    echo "$OPTION=NONE" >> $OUTPUT_FILE
    echo "$OPTION=SERVER" >> $OUTPUT_FILE
    echo "$OPTION=TRANSACTION" >> $OUTPUT_FILE
    echo "$OPTION=STREAMING" >> $OUTPUT_FILE
    echo "$OPTION=CLIENT" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-desync" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-dirty-reads" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-load-data-splitting" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-log-conflicts" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-max-ws-rows" ]]; then
    echoit " > Adding possible values 0,1,2,12,254,1023,2047,1048576 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=2" >> $OUTPUT_FILE
    echo "$OPTION=12" >> $OUTPUT_FILE
    echo "$OPTION=254" >> $OUTPUT_FILE
    echo "$OPTION=1023" >> $OUTPUT_FILE
    echo "$OPTION=2047" >> $OUTPUT_FILE
    echo "$OPTION=1048576" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-max-ws-size" ]]; then
    echoit " > Adding possible values 0,1,2,12,254,1023,2047,1048576 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=2" >> $OUTPUT_FILE
    echo "$OPTION=12" >> $OUTPUT_FILE
    echo "$OPTION=254" >> $OUTPUT_FILE
    echo "$OPTION=1023" >> $OUTPUT_FILE
    echo "$OPTION=2047" >> $OUTPUT_FILE
    echo "$OPTION=1048576" >> $OUTPUT_FILE
    echo "$OPTION=2147483647" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-mode" ]]; then
    echoit " > Adding possible values Empty, IGNORE_NATIVE_REPLICATION_FILTER_RULES for option '${OPTION}' to the final list..."
    echo "$OPTION=EMPTY" >> $OUTPUT_FILE
    echo "$OPTION=IGNORE_NATIVE_REPLICATION_FILTER_RULES" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-on" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-OSU-method" ]]; then
    echoit " > Adding possible values TOI, RSU, NBO for option '${OPTION}' to the final list..."
    echo "$OPTION=TOI" >> $OUTPUT_FILE
    echo "$OPTION=RSU" >> $OUTPUT_FILE
    echo "$OPTION=NBO" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-recover" ]]; then
    echoit " > Adding possible values 0,1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-reject-queries" ]]; then
    echoit " > Adding possible values NONE, ALL , ALL_KILL for option '${OPTION}' to the final list..."
    echo "$OPTION=NONE" >> $OUTPUT_FILE
    echo "$OPTION=ALL" >> $OUTPUT_FILE
    echo "$OPTION=ALL_KILL" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-replicate-myisam" ]]; then
    echoit " > Adding possible values 0, 1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-restart-replica" ]]; then
    echoit " > Adding possible values 0, 1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-retry-autocommit" ]]; then
    echoit " > Adding possible values 0, 1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-RSU-commit-timeout" ]]; then
    echoit " > Adding possible values 5000, 10000, 250000, 31536000000000 for option '${OPTION}' to the final list..."
    echo "$OPTION=5000" >> $OUTPUT_FILE
    echo "$OPTION=10000" >> $OUTPUT_FILE
    echo "$OPTION=250000" >> $OUTPUT_FILE
    echo "$OPTION=31536000000000" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-sync-wait" ]]; then
    echoit " > Adding possible values 0,1,2,3,4,5,6,7 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=2" >> $OUTPUT_FILE
    echo "$OPTION=3" >> $OUTPUT_FILE
    echo "$OPTION=4" >> $OUTPUT_FILE
    echo "$OPTION=5" >> $OUTPUT_FILE
    echo "$OPTION=6" >> $OUTPUT_FILE
    echo "$OPTION=7" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-trx-fragment-size" ]]; then
    echoit " > Adding possible values 0,1,2,12,254,1023,2047,1048576 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=2" >> $OUTPUT_FILE
    echo "$OPTION=12" >> $OUTPUT_FILE
    echo "$OPTION=254" >> $OUTPUT_FILE
    echo "$OPTION=1023" >> $OUTPUT_FILE
    echo "$OPTION=2047" >> $OUTPUT_FILE
    echo "$OPTION=1048576" >> $OUTPUT_FILE
    echo "$OPTION=2147483647" >> $OUTPUT_FILE	 
 elif [[ "$OPTION" == "--wsrep-trx-fragment-unit" ]]; then
    echoit " > Adding possible values bytes, rows , statements for option '${OPTION}' to the final list..."
    echo "$OPTION=bytes" >> $OUTPUT_FILE
    echo "$OPTION=rows" >> $OUTPUT_FILE
    echo "$OPTION=statements" >> $OUTPUT_FILE
 elif [[ "$OPTION" == "--wsrep-sst-donor-rejects-queries" ]]; then
    echoit " > Adding possible values 0, 1 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE   
  elif [ "${VALUE}" == "" ]; then
    echoit "  > Assert: Option '${OPTION}' is blank by default and not programmed into the script yet, please cover this in the script..."
    exit 1
  else
    echoit "  > ${OPTION} IS NOT COVERED YET, PLEASE ADD!!!"
    exit 1
  fi
done < $TEMP_FILE
rm -rf $TEMP_FILE
echo "Done! Output file: ${OUTPUT_FILE}"
