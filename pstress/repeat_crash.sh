############################################################################################
# Created by Mohit Joshi, Percona LLC                                                      #
# Creation date: 07-JAN-2021                                                               #
#                                                                                          #
# This script is created to repeat crashes found during pstress runs.                      #
# The script requires the incident directory, the mysql basedir, mode and the nth step     #
# from where the pstress runs will be resumed to repeat crashes.                           #
# The script requires to be run from <source>/pstress directory                            #
# Usage: ./repeat_crash.sh -i$HOME/path/to/incident/dir -b/path/to/basedir -s3 -mcurrent   #
############################################################################################

#!/bin/bash

Help()
{
echo "Syntax: repeat_crash [-i|s|m|b]";
echo "options:
-i	Path to the Incident Directory where pstress was executed
-s	Nth step where the crash is seen
-m	Select the mode: previous|current|after
        previous=> Resume pstress from (Nth-2) step
        current => Resume pstress from (Nth-1) step
        after   => Resume pstress from (Nth) step
-b      Path to the mysql base directory"
}

if [ $# -eq 0 ]
then
  Help
  exit 1
fi

while getopts i:s:m:b: flag
do
  case "${flag}" in
    i) incident_directory=${OPTARG};;
    s) step=${OPTARG};;
    m) mode=${OPTARG};;
    b) basedir=${OPTARG};;
  esac
done

# Output Function
echoit() {
  echo "[$(date +'%T')] $1"
}

echoit "========================================================="
echoit "Incident Directory: $incident_directory";
echoit "Nth step: $step";
echoit "Mode previous|current|after: $mode";
echoit "Base directory: $basedir";
echoit "========================================================="

RANDOM=`date +%s%N | cut -b14-19`; RANDOMD=$(echo $RANDOM$RANDOM$RANDOM | sed 's/..\(......\).*/\1/')
mkdir $RANDOMD
workdir=`pwd`/$RANDOMD
SCRIPT_PWD=$(cd `dirname $0` && pwd)
CONFIGURATION_FILE=pstress-run.conf
source $incident_directory/$CONFIGURATION_FILE
SEED=$(<$incident_directory/seed)


if [ ! -d "$incident_directory" ]; then
  echo "Error: ${incident_directory} not found. Can not continue."
  exit 1
fi

if [ ! -d "$incident_directory/$step" ]; then
  echo "Error: Invalid step=$step. Can not continue."
  exit 1
fi

if [ ! -d "$basedir" ]; then
  echo "Error: Invalid basedir=$basedir. Can not continue."
  exit 1
fi

if [[ "$mode" == "current" ]]; then
  TRIAL=$[ $step - 1 ]
  if [ $TRIAL -le 0 ]; then
    echo "The value of either mode or nth step is incorrect."
    echo "Cannot continue. Exiting"
    exit
  else
    echoit "Taking backup of datadir from $incident_directory/$TRIAL/data into $workdir/$TRIAL/data"
    mkdir $workdir/$TRIAL
    rsync -ar --exclude='*core*' $incident_directory/$TRIAL/data/ $workdir/$TRIAL/data 2>&1
    echoit "Copying step_$TRIAL.dll file into $workdir"
    cp $incident_directory/step_$TRIAL.dll $workdir
  fi
elif [[ "$mode" == "previous" ]]; then
  TRIAL=$[ $step - 2 ]
  if [ $TRIAL -le 0 ]; then
    echo "The value of either mode or nth step is incorrect."
    echo "Cannot continue. Exiting"
    exit
  else
    echoit "Taking backup of datadir from $incident_directory/$TRIAL/data into $workdir/$TRIAL/data"
    mkdir $workdir/$TRIAL
    rsync -ar --exclude='*core*' $incident_directory/$TRIAL/data/ $workdir/$TRIAL/data 2>&1
    echoit "Copying step_$TRIAL.dll file into $workdir"
    cp $incident_directory/step_$TRIAL.dll $workdir
  fi
elif [[ "$mode" == "after" ]]; then
  TRIAL=$step
  echoit "Taking backup of datadir from $incident_directory/$TRIAL/data into $workdir/$TRIAL/data"
  mkdir $workdir/$TRIAL
  rsync -ar --exclude='*core*' $incident_directory/$TRIAL/data/ $workdir/$TRIAL/data 2>&1
  echoit "Copying step_$TRIAL.dll file into $workdir"
  cp $incident_directory/step_$TRIAL.dll $workdir
else
  echo "Error: Invalid mode=$mode. Can not continue."
  exit 1
fi

myextra=$(<$incident_directory/$step/MYEXTRA)
mysqld_start_timeout=30
port=50661
socket=/tmp/mysql_50661.sock

# Kill the server
kill_server(){
  SIG=$1
  echoit "Killing the server with Signal $SIG";
  { kill -$SIG ${MPID} && wait ${MPID}; } 2>/dev/null
}

repeat_crash() {

TRIAL=$[ $TRIAL + 1 ]
echoit "====== TRIAL #$TRIAL ======"
echoit "Ensuring there are no relevant mysqld server running"
KILLPID=$(ps -ef | grep "mysqld" | grep -v grep | awk '{print $2}' | tr '\n' ' ')
  (sleep 0.2; kill -9 $KILLPID >/dev/null 2>&1; timeout -k4 -s9 4s wait $KILLPID >/dev/null 2>&1) &
echoit "Copying the datadir from previous trial $workdir/$(($TRIAL-1)) into $workdir/$TRIAL";
mkdir $workdir/$TRIAL $workdir/$TRIAL/log
rsync -ar --exclude='*core*' $workdir/$(($TRIAL-1))/data/ $workdir/$TRIAL/data 2>&1
datadir=$workdir/$TRIAL/data

ISSTARTED=0
echoit "Starting the mysqld server..."
MYSQLD="$basedir/bin/mysqld $myextra --datadir=$datadir --plugin-dir=$basedir/lib/plugin --core-file --port=$port --socket=$socket --log-output=none --log-error=$workdir/$TRIAL/log/master.err"
echoit "$MYSQLD"
$MYSQLD > $workdir/$TRIAL/log/master.err 2>&1 &
MPID="$!"

for X in $(seq 0 $mysqld_start_timeout); do
 sleep 1
 if [ "$MPID" == "" ]; then echoit "Assert! $MPID empty. Terminating!"; exit 1; fi
done

# Check if mysqld is started successfully, so pstress will run
if $basedir/bin/mysqladmin -uroot -S$socket ping > /dev/null 2>&1; then
  echoit "Server started ok."
  ISSTARTED=1
else
  echoit "Server failed to start. Can not continue."
fi

if [ $ISSTARTED -eq 1 ]; then
  echoit "Starting pstress run for step:$TRIAL ..."
  CMD="${PSTRESS_BIN} --database=test --threads=${THREADS} --queries-per-thread=${QUERIES_PER_THREAD} --logdir=$workdir/$TRIAL/ --user=root --socket=$socket --seed $SEED --step $TRIAL --metadata-path $workdir/ --seconds ${PSTRESS_RUN_TIMEOUT} ${DYNAMIC_QUERY_PARAMETER}"
  echoit "$CMD"
  $CMD > $workdir/$TRIAL/pstress.log 2>&1 &
  PSPID="$!"
  echoit "pstress running (Max duration: ${PSTRESS_RUN_TIMEOUT}s)..."
  for X in $(seq 1 $PSTRESS_RUN_TIMEOUT); do
    sleep 1
    if [ "`ps -ef | grep $PSPID | grep -v grep`" == "" ]; then  # pstress ended
      break
    fi
    if [ $X -ge $PSTRESS_RUN_TIMEOUT ]; then
      echoit "${PSTRESS_RUN_TIMEOUT}s timeout reached. Terminating this trial..."
      if [ ${TIMEOUT_INCREMENT} != 0 ]; then
        echoit "TIMEOUT_INCREMENT option was enabled and set to ${TIMEOUT_INCREMENT} sec"
        echoit "${TIMEOUT_INCREMENT}s will be added to the next trial timeout."
      else
        echoit "TIMEOUT_INCREMENT option was disabled and set to 0"
      fi
      PSTRESS_RUN_TIMEOUT=$[ ${PSTRESS_RUN_TIMEOUT} + ${TIMEOUT_INCREMENT} ]
      break
    fi
  done
else
  echoit "Server (PID: $MPID | Socket: $SOCKET) failed to start after $mysqld_start_timeout} seconds. Will issue extra kill -9 to ensure it's gone..."
  (sleep 0.2; kill -9 $MPID >/dev/null 2>&1; timeout -k4 -s9 4s wait $MPID >/dev/null 2>&1) &
   timeout -k5 -s9 5s wait $MPID >/dev/null 2>&1
   sleep 2; sync
  exit 1
fi

kill_server $SIGNAL
sleep 1 #^ Ensure the mysqld is gone completely
echoit "pstress run details:$(grep -i 'SUMMARY.*queries failed' $workdir/$TRIAL/*.sql $workdir/$TRIAL/*.log | sed 's|.*:||')"

if [ $SIGNAL -ne 4 ]; then
  if [ $(ls -l $workdir/$TRIAL/*/*core* 2>/dev/null | wc -l) -ge 1 ]; then
    echoit "mysqld coredump detected at $(ls $workdir/$TRIAL/*/*core* 2>/dev/null)"
    echoit "Bug found (as per error log): $(${SCRIPT_PWD}/search_string.sh $workdir/$TRIAL/log/master.err)"
  fi
elif [ $SIGNAL -eq 4 ]; then
  if [[ $(grep -i "mysqld got signal 4" $workdir/$TRIAL/log/master.err 2>/dev/null | wc -l) -ge 1 ]]; then
    echoit "mysqld coredump detected due to SIGNAL(kill -4) at $(ls ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null)"
  else
    echoit "mysqld coredump detected at $(ls $workdir/$TRIAL/*/*core* 2>/dev/null)"
    echoit "Bug found (as per error log): $(${SCRIPT_PWD}/search_string.sh $workdir/$TRIAL/log/master.err)"
  fi
fi

}

# Start actual pstress runs
echoit "Resuming pstress iterations after step:$TRIAL"
LEFT_TRIALS=$[ $TRIALS - $TRIAL ]
for X in $(seq 1 $LEFT_TRIALS); do
  repeat_crash
done

# All done
echoit "pstress finished requested number of trials ... Terminating"
KILL_PIDS=`ps -ef | grep "$RANDOMD" | grep -v "grep" | awk '{print $2}' | tr '\n' ' '`
if [ "$KILL_PIDS" != "" ]; then
  echoit "Terminating the following PID's: $KILL_PIDS"
  kill -9 $KILL_PIDS >/dev/null 2>&1
fi
exit 0
