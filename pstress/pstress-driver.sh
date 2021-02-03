#!/bin/bash
# Created by Mohit Joshi

# Internal variables, please do not change
RANDOM=`date +%s%N | cut -b14-19`;
RANDOMD=$(echo $RANDOM$RANDOM$RANDOM | sed 's/..\(......\).*/\1/');
CONFIG_FLAG=0;INCIDENT_FLAG=0;STEP_FLAG=0;REPEAT_FLAG=0;NORMAL_MODE=0;
RESUME_MODE=0;REPEAT_MODE=0;TRIAL_SAVED=1;WORKDIRACTIVE=0;
TRIAL=0;SCRIPT_PWD=$(cd `dirname $0` && pwd);
MYSQLD_START_TIMEOUT=60;

Help()
{
echo "usage:  $BASH_SOURCE --config <val> --incident <val> --repeat <val> --step <val>"
echo "	--config: The configuration file that must be used to run pstress"
echo "	--incident: The incident directory that must be used to repeat a scenario"
echo "	--repeat: Repeat the step provided number of times"
echo "	--step: The step from where the pstress runs will be resumed. If --repeat option
                is provided, only the mentioned step would be repeated"
}

# Read command line options. In future, if a new option is added, please ensure
# to add the option in alphabetical order. Please note that help should always
# remain the last option.
ARGUMENT_LIST=(
    "config"
    "incident"
    "repeat"
    "step"
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
    --config)
        shift
        CONFIGURATION_FILE=$1
        CONFIG_FLAG=1
        echo "Config file is: $CONFIGURATION_FILE"
        ;;
    --incident)
        shift
        INCIDENT_DIRECTORY=$1
        INCIDENT_FLAG=1
        echo "Incident Directory is: $INCIDENT_DIRECTORY"
        ;;
    --repeat)
        shift
        REPEAT=$1
        REPEAT_FLAG=1
        echo "Repeat is: $REPEAT"
        ;;
    --step)
        shift
        STEP=$1
        STEP_FLAG=1
        echo "Step is: $STEP"
        ;;
    --)
        shift
        break
        ;;
    esac
    shift
done


# Output Function
echoit(){
  echo "[$(date +'%T')] [$TRIAL] $1"
  if [ ${WORKDIRACTIVE} -eq 1 ]; then echo "[$(date +'%T')] [$TRIAL] $1" >> /${WORKDIR}/pstress-run.log; fi
}

# Trap ctrl-c
trap ctrl-c SIGINT

ctrl-c(){
  echoit "CTRL+C was pressed. Attempting to terminate running processes..."
  KILL_PIDS=`ps -ef | grep "$RANDOMD" | grep -v "grep" | awk '{print $2}' | tr '\n' ' '`
  if [ "${KILL_PIDS}" != "" ]; then
    echoit "Terminating the following PID's: ${KILL_PIDS}"
    kill -9 ${KILL_PIDS} >/dev/null 2>&1
  fi
  echoit "Done. Terminating pstress-driver.sh with exit code 2..."
  exit 2
}

# Kill the server
kill_server(){
  SIG=$1
  echoit "Killing the server with Signal $SIG";
  { kill -$SIG $MPID && wait $MPID; } 2>/dev/null
}

# Start normal pstress runs
normal_run() {
  TRIAL=$[ $TRIAL + 1 ]
  SOCKET=${RUNDIR}/${TRIAL}/socket.sock
  echoit "====== TRIAL #$TRIAL ======"
  echoit "Ensuring there are no relevant mysqld server running"
  KILLPID=$(ps -ef | grep "${RUNDIR}" | grep -v grep | awk '{print $2}' | tr '\n' ' ')
  { kill -9 $KILLPID && wait $KILLPID; } 2>/dev/null
  echoit "Clearing rundir..."
  rm -Rf ${RUNDIR}/*
  echoit "Generating new trial workdir ${RUNDIR}/${TRIAL}..."
  mkdir -p ${RUNDIR}/${TRIAL}/data ${RUNDIR}/${TRIAL}/tmp ${RUNDIR}/${TRIAL}/log
  if [ ${TRIAL} -gt 1 ]; then
    echoit "Copying datadir from Trial ${WORKDIR}/$((${TRIAL}-1)) into ${WORKDIR}/${TRIAL}..."
  else
    echoit "Copying datadir from template..."
  fi
  if [ `ls -l ${WORKDIR}/data.template/* | wc -l` -eq 0 ]; then
    echoit "Assert: Something went wrong during the data directory initialisation."
    echoit "Please check the logs at $WORKDIR/log/mysql_install_db.txt to know what went wrong. Terminating"
    exit 1
  elif [ ${TRIAL} -gt 1 ]; then
    rsync -ar --exclude='*core*' ${WORKDIR}/$((${TRIAL}-1))/data/ ${RUNDIR}/${TRIAL}/data 2>&1
  else
    cp -R ${WORKDIR}/data.template/* ${RUNDIR}/${TRIAL}/data 2>&1
  fi

  # Start server
  SOCKET=${RUNDIR}/${TRIAL}/socket.sock
  DATADIR=${RUNDIR}/${TRIAL}/data
  TEMPDIR=${RUNDIR}/${TRIAL}/tmp
  ERROR=${RUNDIR}/${TRIAL}/log/master.err
  PID_FILE=${RUNDIR}/${TRIAL}/pid.pid
  start_server

# Start pstress
  LOGDIR=${RUNDIR}/${TRIAL}
  METADATA_PATH=${WORKDIR}
  start_pstress

# Terminate mysqld
  kill_server $SIGNAL
  sleep 1 #^ Ensure the mysqld is gone completely
  echoit "pstress run details:$(grep -i 'SUMMARY.*queries failed' ${RUNDIR}/${TRIAL}/*.sql ${RUNDIR}/${TRIAL}/*.log | sed 's|.*:||')"

  if [ $TRIAL_SAVED -eq 1 ]; then
    savetrial
  fi

}

# Repeat a particular step
repeat_step() {
  TRIAL=$STEP
  echoit "====== TRIAL #$TRIAL ======"
  echoit "Ensuring there are no relevant mysqld server running..."
  KILLPID=$(ps -ef | grep ${REPEAT_DIR} | grep -v grep | awk '{print $2}' | tr '\n' ' ')
  { kill -9 $KILLPID && wait $KILLPID; } 2>/dev/null
  mkdir -p ${REPEAT_DIR}/$TRIAL.$COUNT/data ${REPEAT_DIR}/$TRIAL.$COUNT/log ${REPEAT_DIR}/$TRIAL.$COUNT/tmp
  echoit "Copying the datadir from incident directory $INCIDENT_DIRECTORY/$TRIAL/data into ${REPEAT_DIR}/$TRIAL.$COUNT";
  rsync -ar --exclude='*core*' $INCIDENT_DIRECTORY/$TRIAL/data/ ${REPEAT_DIR}/$TRIAL.$COUNT/data 2>&1

# Start server
  SOCKET=${REPEAT_DIR}/$TRIAL.$COUNT/socket.sock
  DATADIR=${REPEAT_DIR}/$TRIAL.$COUNT/data
  TEMPDIR=${REPEAT_DIR}/$TRIAL.$COUNT/tmp
  ERROR=${REPEAT_DIR}/$TRIAL.$COUNT/log/master.err
  PID_FILE=${REPEAT_DIR}/$TRIAL.$COUNT/pid.pid
  start_server

# Start pstress
  LOGDIR=${REPEAT_DIR}/$TRIAL.$COUNT/
  METADATA_PATH=${REPEAT_DIR}
  start_pstress

# Terminate mysqld
  kill_server $SIGNAL
  sleep 1 #^ Ensure the mysqld is gone completely
  echoit "pstress run details:$(grep -i 'SUMMARY.*queries failed' ${REPEAT_DIR}/$TRIAL.$COUNT/*.sql ${REPEAT_DIR}/$TRIAL.$COUNT/*.log | sed 's|.*:||')"

}

# Resume pstress runs from a particular step
resume_pstress() {
  TRIAL=$[ $TRIAL + 1 ]
  echoit "====== TRIAL #$TRIAL ======"
  echoit "Ensuring there are no relevant mysqld server running"
  KILLPID=$(ps -ef | grep ${RESUME_DIR} | grep -v grep | awk '{print $2}' | tr '\n' ' ')
  { kill -9 $KILLPID && wait $KILLPID; } 2>/dev/null
  mkdir -p ${RESUME_DIR}/$TRIAL/data ${RESUME_DIR}/$TRIAL/log ${RESUME_DIR}/$TRIAL/tmp
  echoit "Copying the datadir from previous trial ${RESUME_DIR}/$(($TRIAL-1)) into ${RESUME_DIR}/$TRIAL";
  rsync -ar --exclude='*core*' ${RESUME_DIR}/$(($TRIAL-1))/data/ ${RESUME_DIR}/$TRIAL/data 2>&1

# Start server
  SOCKET=${RESUME_DIR}/${TRIAL}/socket.sock
  DATADIR=${RESUME_DIR}/${TRIAL}/data
  TEMPDIR=${RESUME_DIR}/${TRIAL}/tmp
  ERROR=${RESUME_DIR}/${TRIAL}/log/master.err
  PID_FILE=${RESUME_DIR}/${TRIAL}/pid.pid
  start_server

# Start pstress
  LOGDIR=${RESUME_DIR}/${TRIAL}
  METADATA_PATH=${RESUME_DIR}
  start_pstress

# Terminate mysqld
  kill_server $SIGNAL
  sleep 1 #^ Ensure the mysqld is gone completely
  echoit "pstress run details:$(grep -i 'SUMMARY.*queries failed' ${RESUME_DIR}/${TRIAL}/*.sql ${RESUME_DIR}/${TRIAL}/*.log | sed 's|.*:||')"
}

savetrial() {
  echoit "Copying rundir from ${RUNDIR}/${TRIAL} to ${WORKDIR}/${TRIAL}"
  mv ${RUNDIR}/${TRIAL}/ ${WORKDIR}/ 2>&1 | tee -a /${WORKDIR}/pstress-run.log
}

# Start pstress on running server
start_pstress() {
  echoit "Starting pstress run for step:${TRIAL} ..."
  CMD="${PSTRESS_BIN} --database=test --threads=${THREADS} --queries-per-thread=${QUERIES_PER_THREAD} --logdir=${LOGDIR} --user=root --socket=$SOCKET --seed ${SEED} --step ${TRIAL} --metadata-path ${METADATA_PATH}/ --seconds ${PSTRESS_RUN_TIMEOUT} ${DYNAMIC_QUERY_PARAMETER}"
  echoit "$CMD"
  $CMD > ${LOGDIR}/pstress.log 2>&1 &
  PSPID="$!"
  echoit "pstress running (Max duration: ${PSTRESS_RUN_TIMEOUT}s)..."
  for X in $(seq 1 ${PSTRESS_RUN_TIMEOUT}); do
    sleep 1
    if [ "`ps -ef | grep $PSPID | grep -v grep`" == "" ]; then  # pstress ended
      break
    fi
    if [ $X -ge ${PSTRESS_RUN_TIMEOUT} ]; then
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
}

# Shutdown server
shutdown_server() {
  echo "Todo: shutdown the server"
}

# Start the server
start_server() {
  PORT=$[50000 + ( $RANDOM % ( 9999 ) ) ]
  echoit "Starting mysqld server..."
  CMD="${BIN} ${MYSAFE} ${MYEXTRA} --basedir=${BASEDIR} --datadir=$DATADIR --tmpdir=$TEMPDIR \
--core-file --port=$PORT --pid_file=$PID_FILE --socket=$SOCKET \
--log-output=none --log-error-verbosity=3 --log-error=$ERROR"
  echoit "$CMD"
  $CMD > ${ERROR} 2>&1 &
  MPID="$!"

  echoit "Waiting for mysqld (pid: ${MPID}) to fully start..."
  for X in $(seq 0 $MYSQLD_START_TIMEOUT); do
    sleep 1
    if [ "$MPID" == "" ]; then echoit "Assert! $MPID empty. Terminating!"; exit 1; fi
  done

# Check if mysqld is started successfully
  if ${BASEDIR}/bin/mysqladmin -uroot -S${SOCKET} ping > /dev/null 2>&1; then
    echoit "Server started ok. Client: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET}"
    ${BASEDIR}/bin/mysql -uroot -S${SOCKET} -e "CREATE DATABASE IF NOT EXISTS test;" > /dev/null 2>&1
  else
    echoit "Server (PID: $MPID | Socket: $SOCKET) failed to start after $MYSQLD_START_TIMEOUT} seconds. Will issue extra kill -9 to ensure it's gone..."
    { kill -9 $MPID && wait $MPID; } 2>/dev/null
    exit
  fi
}

# Start the PXC server
start_pxc_server() {
echo "Todo: start the pxc server"
}

if [[ $CONFIG_FLAG -eq 1 && $STEP_FLAG -eq 0 && $INCIDENT_FLAG -eq 0 && $REPEAT_FLAG -eq 0 ]]; then
  echo "Running pstress in normal mode"
  NORMAL_MODE=1
elif [[ $CONFIG_FLAG -eq 1 && $STEP_FLAG -eq 1 && $INCIDENT_FLAG -eq 1 && $REPEAT_FLAG -eq 0 ]]; then
  echo "Running pstress in resume mode"
  RESUME_MODE=1
elif [[ $CONFIG_FLAG -eq 1 && $STEP_FLAG -eq 1 && $REPEAT_FLAG -eq 1 && $INCIDENT_FLAG -eq 1 ]]; then
  echo "Running pstress in repeat mode"
  REPEAT_MODE=1
else
  echo "Invalid option(s) passed. Terminating..."
  exit 0
fi

source $SCRIPT_PWD/$CONFIGURATION_FILE
if [ "${SEED}" == "" ]; then SEED=${RANDOMD}; fi

# Find mysqld binary
if [ -r ${BASEDIR}/bin/mysqld ]; then
  BIN=${BASEDIR}/bin/mysqld
else
  echoit "Assert: there is no (script readable) mysqld binary at ${BASEDIR}/bin/mysqld ?"
  exit 1
fi

if [ $NORMAL_MODE -eq 1 ]; then
  rm -Rf ${WORKDIR} ${RUNDIR}
  mkdir ${WORKDIR} ${WORKDIR}/log ${RUNDIR}
  WORKDIRACTIVE=1
  echoit "Workdir: ${WORKDIR} | Rundir: ${RUNDIR} | Basedir: ${BASEDIR} "
  echoit "mysqld Start Timeout: ${MYSQLD_START_TIMEOUT} | Client Threads: ${THREADS} | Trials: ${TRIALS} "
  echoit "pstress Binary: ${PSTRESS_BIN}"
  if [ "${MYINIT}" != "" ]; then echoit "MYINIT: ${MYINIT}"; fi
  if [ "${MYSAFE}" != "" ]; then echoit "MYSAFE: ${MYSAFE}"; fi
  if [ "${MYEXTRA}" != "" ]; then echoit "MYEXTRA: ${MYEXTRA}"; fi
  if [ ${STORE_COPY_OF_INFILE} -eq 1 ]; then
    echoit "Making a copy of the SQL input file used (${INFILE}) to ${WORKDIR}/ for reference..."
    cp ${INFILE} ${WORKDIR}
  fi
  echoit "Making a copy of the mysqld used to ${WORKDIR}/mysqld (handy for coredump analysis and manual bundle creation)..."
  mkdir ${WORKDIR}/mysqld
  cp ${BIN} ${WORKDIR}/mysqld
  echoit "Making a copy of the conf file pstress-run.conf(useful later during repeating the crashes)..."
  cp ${SCRIPT_PWD}/$CONFIGURATION_FILE ${WORKDIR}/
  echoit "Making a copy of the seed file..."
  echo "${SEED}" > ${WORKDIR}/seed
  INIT_OPT="--no-defaults --initialize-insecure ${MYINIT}"
  echoit "Generating datadir template (using mysql_install_db or mysqld --init)..."
  ${BIN} ${INIT_OPT} --basedir=${BASEDIR} --datadir=${WORKDIR}/data.template > ${WORKDIR}/log/mysql_install_db.txt 2>&1
  echo "Starting pstress iterations"
  TRIAL=0
  for X in $(seq 1 $TRIALS); do
    normal_run
  done
elif [ $RESUME_MODE -eq 1 ]; then
  if [ ${RESUME_DIR} == "" ]; then
    echo "Please configure the path for RESUME_DIR in the $CONFIGURATION_FILE. Can not continue"
     exit
  fi
  echo "Resuming pstress iterations after step:$STEP"
  TRIAL=$STEP
  SEED=$(<$INCIDENT_DIRECTORY/seed)
  echoit "Taking backup of datadir from $INCIDENT_DIRECTORY/$TRIAL/data into ${RESUME_DIR}/$TRIAL/"
  mkdir -p ${RESUME_DIR}/$TRIAL
  rsync -ar --exclude='*core*' $INCIDENT_DIRECTORY/$TRIAL/data/ ${RESUME_DIR}/$TRIAL/data 2>&1
  echoit "Copying step_$TRIAL.dll file into ${RESUME_DIR}"
  cp $INCIDENT_DIRECTORY/step_$TRIAL.dll ${RESUME_DIR}
  LEFT_TRIALS=$[ ${TRIALS} - $TRIAL ]
  for X in $(seq 1 $LEFT_TRIALS); do
    resume_pstress
  done
elif [ $REPEAT_MODE -eq 1 ]; then
  if [ "${REPEAT_DIR}" == "" ]; then
    echo "Please configure the path for REPEAT_DIR in the $CONFIGURATION_FILE. Can not continue"
    exit
  fi
  echoit "Repeating step $STEP ($REPEAT) number of times"
  SEED=$(<$INCIDENT_DIRECTORY/seed)
  echoit "Taking backup of datadir from $INCIDENT_DIRECTORY/$STEP/data into ${REPEAT_DIR}/$STEP/"
  mkdir -p ${REPEAT_DIR}/$STEP
  rsync -ar --exclude='*core*' $INCIDENT_DIRECTORY/$STEP/data/ ${REPEAT_DIR}/$STEP/data 2>&1
  if [ $STEP -gt 1 ]; then
    echoit "Copying step_$(($STEP-1)).dll file into ${REPEAT_DIR}"
    cp $INCIDENT_DIRECTORY/step_$(($STEP-1)).dll ${REPEAT_DIR}
  elif [ $STEP -eq 1 ]; then
    echoit "If you intend to repeat step 1, please perform a normal run using same seed number"
    exit
  else
    echoit "Invalid step provided. Exiting..."
    exit
  fi
  for COUNT in $(seq 1 $REPEAT); do
    repeat_step
  done
fi

