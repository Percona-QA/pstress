#!/bin/bash
# Created by Mohit Joshi

# Internal variables, please do not change
RANDOM=`date +%s%N | cut -b14-19`;
RANDOMD=$(echo $RANDOM$RANDOM$RANDOM | sed 's/..\(......\).*/\1/');
CONFIG_FLAG=0;INCIDENT_FLAG=0;STEP_FLAG=0;REPEAT_FLAG=0;NORMAL_MODE=0;
RESUME_MODE=0;REPEAT_MODE=0;
SCRIPT_PWD=$(cd `dirname $0` && pwd);
MYSQLD_START_TIMEOUT=30;

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
  echo "[$(date +'%T')] $1"
}

# Kill the server
kill_server(){
  SIG=$1
  echoit "Killing the server with Signal $SIG";
  { kill -$SIG $MPID && wait $MPID; } 2>/dev/null
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


# Start normal pstress runs
pstress_run() {
  TRIAL=$[ $TRIAL + 1 ]
  SOCKET=${RUNDIR}/${TRIAL}/socket.sock
  echoit "====== TRIAL #$TRIAL ======"
  echoit "Ensuring there are no relevant mysqld server running"
  KILLPID=$(ps -ef | grep "${RUNDIR}" | grep -v grep | awk '{print $2}' | tr '\n' ' ')
  (sleep 0.2; kill -9 $KILLPID >/dev/null 2>&1; timeout -k4 -s9 4s wait $KILLPID >/dev/null 2>&1) &
  timeout -k5 -s9 5s wait $KILLPID >/dev/null 2>&1
  echoit "Clearing rundir..."
  rm -Rf ${RUNDIR}/*
  echoit "Generating new trial workdir ${RUNDIR}/${TRIAL}..."
  mkdir -p ${RUNDIR}/${TRIAL}/data ${RUNDIR}/${TRIAL}/log
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
  start_server

  for X in $(seq 0 $mysqld_start_timeout); do
    sleep 1
    if [ "$MPID" == "" ]; then echoit "Assert! $MPID empty. Terminating!"; exit 1; fi
  done

  # Check if mysqld is started successfully, so pstress will run
  if $basedir/bin/mysqladmin -uroot -S$socket ping > /dev/null 2>&1; then
    echoit "Server started ok."
  else
    echoit "Server failed to start. Can not continue."
  fi



}

# Repeat a particular step
repeat_step() {
echo "Todo: repeat the given step mentioned number of times"
}

# Resume pstress runs from a particular step
resume_pstress_run() {
echo "Todo: resume pstress runs from the given step"
}

# Shutdown server
shutdown_server() {
echo "Todo: shutdown the server"
}

# Start the server
start_server() {
  PORT=$[50000 + ( $RANDOM % ( 9999 ) ) ]
  SOCKET=${RUNDIR}/${TRIAL}/socket.sock
  echoit "Starting mysqld server..."
  CMD="${BIN} ${MYSAFE} ${MYEXTRA} --basedir=${BASEDIR} --datadir=${RUNDIR}/${TRIAL}/data --tmpdir=${RUNDIR}/${TRIAL}/tmp \
--core-file --port=$PORT --pid_file=${RUNDIR}/${TRIAL}/pid.pid --socket=${SOCKET} \
--log-output=none --log-error-verbosity=3 --log-error=${RUNDIR}/${TRIAL}/log/master.err"
  echoit "$CMD"
  $CMD > ${RUNDIR}/${TRIAL}/log/master.err 2>&1 &
  MPID="$!"
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
  cp ${SCRIPT_PWD}/pstress-run.conf ${WORKDIR}/
  echoit "Making a copy of the seed file..."
  echo "${SEED}" > ${WORKDIR}/seed
  INIT_OPT="--no-defaults --initialize-insecure ${MYINIT}"
  echoit "Generating datadir template (using mysql_install_db or mysqld --init)..."
  ${BIN} ${INIT_OPT} --basedir=${BASEDIR} --datadir=${WORKDIR}/data.template > ${WORKDIR}/log/mysql_install_db.txt 2>&1
  echo "Starting pstress iterations"
  TRIAL=0
  for X in $(seq 1 $TRIALS); do
    pstress_run
  done
elif [ $RESUME_MODE -eq 1 ]; then
  echo "Resuming pstress iterations after step:$STEP"
  TRIAL=$STEP
  LEFT_TRIALS=$[ $TRIALS - $TRIAL ]
  for X in $(seq 1 $LEFT_TRIALS); do
    resume_pstress_run
  done
elif [ $REPEAT_MODE -eq 1 ]; then
  echo "Repeating step $step ($REPEAT) number of times"
  for X in $(seq 1 $REPEAT); do
    repeat_step
  done
fi

