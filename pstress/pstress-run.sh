#!/bin/bash
# Created by Roel Van de Paar, Percona LLC
# Updated by Ramesh Sivaraman, Percona LLC
# Updated by Mohit Joshi, Percona LLC

# ========================================= User configurable variables ==========================================================
# Note: if an option is passed to this script, it will use that option as the configuration file instead, for example ./pstress-run.sh pstress-run.conf
CONFIGURATION_FILE=pstress-run-80.conf  # Do not use any path specifiers, the .conf file should be in the same path as pstress-run.sh

# ========================================= MAIN CODE ============================================================================
# Internal variables: DO NOT CHANGE!
RANDOM=`date +%s%N | cut -b14-19`; RANDOMD=$(echo $RANDOM$RANDOM$RANDOM | sed 's/..\(......\).*/\1/')
SCRIPT_AND_PATH=$(readlink -f $0); SCRIPT=$(echo ${SCRIPT_AND_PATH} | sed 's|.*/||'); SCRIPT_PWD=$(cd `dirname $0` && pwd)
WORKDIRACTIVE=0; SAVED=0; TRIAL=0; MYSQLD_START_TIMEOUT=60; PXC=0; GRP_RPL=0; PXC_START_TIMEOUT=60; GRP_RPL_START_TIMEOUT=60; TIMEOUT_REACHED=0; STOREANYWAY=0; REINIT_DATADIR=0;
SERVER_FAIL_TO_START_COUNT=0; ENGINE=InnoDB; UUID=$(uuidgen); GCACHE_ENCRYPTION=0;

# Read configuration
if [ "$1" != "" ]; then CONFIGURATION_FILE=$1; fi
if [ ! -r ${SCRIPT_PWD}/${CONFIGURATION_FILE} ]; then echo "Assert: the confiruation file ${SCRIPT_PWD}/${CONFIGURATION_FILE} cannot be read!"; exit 1; fi
source ${SCRIPT_PWD}/$CONFIGURATION_FILE
if [ "${SEED}" == "" ]; then SEED=${RANDOMD}; fi

if [[ ${SIGNAL} -ne 15 && ${SIGNAL} -ne 4 && ${SIGNAL} -ne 9 ]]; then
  echo "Invalid option SIGNAL=${SIGNAL} passed. Exiting...";
  exit
fi

# RocksDB does not support encryption. Disable all keyring encryption types
if [ "${ENGINE}" == "RocksDB" ]; then
  ENCRYPTION_RUN=0
fi

# Check no two encryption types are enabled at the same time
if [ ${ENCRYPTION_RUN} -eq 1 ]; then
  if [[ ${KEYRING_VAULT} -eq 1 && ${KEYRING_COMPONENT} -eq 1 ]]; then
    echo "Enable one encryption type at a time"
    exit 1
  elif [[ ${KEYRING_VAULT} -eq 1 && ${KEYRING_FILE} -eq 1 ]]; then
    echo "Enable one encryption type at a time"
    exit 1
  elif [[ ${KEYRING_FILE} -eq 1 && ${KEYRING_COMPONENT} -eq 1 ]]; then
    echo "Enable one encryption type at a time"
    exit 1
  fi
elif [ ${ENCRYPTION_RUN} -eq 0 ]; then
  KEYRING_COMPONENT=0
  KEYRING_VAULT=0
  KEYRING_FILE=0
fi

# Safety checks: ensure variables are correctly set to avoid rm -Rf issues (if not set correctly, it was likely due to altering internal variables at the top of this file)
if [ "${WORKDIR}" == "/sd[a-z][/]" ]; then echo "Assert! \$WORKDIR == '${WORKDIR}' - is it missing the \$RANDOMD suffix?"; exit 1; fi
if [ "${RUNDIR}" == "/dev/shm[/]" ]; then echo "Assert! \$RUNDIR == '${RUNDIR}' - is it missing the \$RANDOMD suffix?"; exit 1; fi
if [ "$(echo ${RANDOMD} | sed 's|[0-9]|/|g')" != "//////" ]; then echo "Assert! \$RANDOMD == '${RANDOMD}'. This looks incorrect - it should be 6 numbers exactly"; exit 1; fi
if [ "${SKIPCHECKDIRS}" == "" ]; then
  if [ "$(echo ${WORKDIR} | grep -oi "$RANDOMD" | head -n1)" != "${RANDOMD}" ]; then echo "Assert! \$WORKDIR == '${WORKDIR}' - is it missing the \$RANDOMD suffix?"; exit 1; fi
  if [ "$(echo ${RUNDIR}  | grep -oi "$RANDOMD" | head -n1)" != "${RANDOMD}" ]; then echo "Assert! \$RUNDIR == '${RUNDIR}' - is it missing the \$RANDOMD suffix?"; exit 1; fi
fi

# Other safety checks
if [ "$(echo ${PSTRESS_BIN} | sed 's|\(^/pstress\)|\1|')" == "/pstress" ]; then echo "Assert! \$PSTRESS_BIN == '${PSTRESS_BIN}' - is it missing the \$SCRIPT_PWD prefix?"; exit 1; fi
if [ ! -r ${PSTRESS_BIN} ]; then echo "${PSTRESS_BIN} specified in the configuration file used (${SCRIPT_PWD}/${CONFIGURATION_FILE}) cannot be found/read"; exit 1; fi
if [ ! -r ${OPTIONS_INFILE} ]; then echo "${OPTIONS_INFILE} specified in the configuration file used (${SCRIPT_PWD}/${CONFIGURATION_FILE}) cannot be found/read"; exit 1; fi

# Try and raise ulimit for user processes (see setup_server.sh for how to set correct soft/hard nproc settings in limits.conf)
ulimit -u 7000

#Format version string (thanks to wsrep_sst_xtrabackup-v2) 
normalize_version(){
  local major=0
  local minor=0
  local patch=0
  
  # Only parses purely numeric version numbers, 1.2.3
  # Everything after the first three values are ignored
  if [[ $1 =~ ^([0-9]+)\.([0-9]+)\.?([0-9]*)([\.0-9])*$ ]]; then
    major=${BASH_REMATCH[1]}
    minor=${BASH_REMATCH[2]}
    patch=${BASH_REMATCH[3]}
  fi
  printf %02d%02d%02d $major $minor $patch
}

#Version comparison script (thanks to wsrep_sst_xtrabackup-v2) 
check_for_version()
{
  local local_version_str="$( normalize_version $1 )"
  local required_version_str="$( normalize_version $2 )"
  
  if [[ "$local_version_str" < "$required_version_str" ]]; then
    return 1
  else
    return 0
  fi
}

# Output function
echoit(){
  echo "[$(date +'%T')] [$SAVED] $1"
  if [ ${WORKDIRACTIVE} -eq 1 ]; then echo "[$(date +'%T')] [$SAVED] $1" >> /${WORKDIR}/pstress-run.log; fi
}

# Kill the server
kill_server(){
# Receive the value of kill signal eg 9,4
  local SIG=$1
# Receive the process ID to be killed
  local MPID=$2
  { kill -$SIG ${MPID} && wait ${MPID}; } 2>/dev/null
}

# PXC Bug found display function
pxc_bug_found(){
  NODE=$1
  for i in $(seq 1 $NODE)
  do
    if [ "$(${SCRIPT_PWD}/search_string.sh ${RUNDIR}/${TRIAL}/node$i/node$i.err 2>/dev/null)" != "" ]; then
      echoit "Bug found in PXC/GR node#$i(as per error log): $(${SCRIPT_PWD}/search_string.sh ${RUNDIR}/${TRIAL}/node$i/node$i.err)";
    fi
  done;
}

create_global_manifest() {
  echoit "Creating global manifest file mysqld.my"
  cat << EOF >${BASEDIR}/bin/mysqld.my
{
  "read_local_manifest": true
}
EOF
}

create_local_manifest() {
  node=$1
  echoit "Creating local manifest file mysqld.my"
  if [ $# -eq 0 ]; then
    cat << EOF >${RUNDIR}/${TRIAL}/data/mysqld.my
{
 "components": "file://component_keyring_file"
}
EOF
  else
    cat << EOF >${RUNDIR}/${TRIAL}/node$node/mysqld.my
{
 "components": "file://component_keyring_file"
}
EOF
  fi
}

create_global_config() {
  echoit "Creating global configuration file component_keyring_file.cnf"
  cat << EOF >${BASEDIR}/lib/plugin/component_keyring_file.cnf
{
  "read_local_config": true
}
EOF
}

create_local_config() {
  node=$1
  echoit "Creating local configuration file component_keyring_file.cnf"
  if [ $# -eq 0 ]; then
    cat << EOF >${RUNDIR}/${TRIAL}/data/component_keyring_file.cnf
{
 "path": "${RUNDIR}/${TRIAL}/data/component_keyring_file",
 "read_only": false
}
EOF
  else
    cat << EOF >${RUNDIR}/${TRIAL}/node$node/component_keyring_file.cnf
{
 "path": "${RUNDIR}/${TRIAL}/node$node/component_keyring_file",
 "read_only": false
}
EOF
  fi
}

# Find mysqld binary
if [ -r ${BASEDIR}/bin/mysqld ]; then
  BIN=${BASEDIR}/bin/mysqld
else
  # Check if this is a debug build by checking if debug string is present in dirname
  if [[ ${BASEDIR} = *debug* ]]; then
    if [ -r ${BASEDIR}/bin/mysqld-debug ]; then
      BIN=${BASEDIR}/bin/mysqld-debug
    else
      echoit "Assert: there is no (script readable) mysqld binary at ${BASEDIR}/bin/mysqld[-debug] ?"
      exit 1
    fi
  else
    echoit "Assert: there is no (script readable) mysqld binary at ${BASEDIR}/bin/mysqld ?"
    exit 1
  fi
fi

#Store MySQL version string
MYSQL_VERSION=$(${BASEDIR}/bin/mysqld --version 2>&1 | grep -oe '[0-9]\.[0-9][\.0-9]*' | head -n1)

if [ "${CONFIGURATION_FILE}" == "pstress-run-PXC80.conf" -o "${CONFIGURATION_FILE}" == "pstress-run-PXC57.conf" ]; then PXC=1; fi
if [ "$(whoami)" == "root" ]; then MYEXTRA="--user=root ${MYEXTRA}"; fi
if [ "${PXC_CLUSTER_RUN}" == "1" ]; then
  echoit "As PXC_CLUSTER_RUN=1, this script is auto-assuming this is a PXC run and will set PXC=1"
  PXC=1
  THREADS=$(cat ${PXC_CLUSTER_CONFIG} | grep ^threads | head -n1 | sed 's/.*=//' | sed 's/^ *//g')
elif [ "${GRP_RPL_CLUSTER_RUN}" == "1" ]; then
  echoit "As GRP_RPL_CLUSTER_RUN=1, this script is auto-assuming this is a Group Replication run and will set GRP_RPL=1"
  GRP_RPL=1
  THREADS=$(cat ${GR_CLUSTER_CONFIG} | grep ^threads | grep -v "#" | head -n1 | sed 's/.*=//' | sed 's/^ *//g')
fi
if [ "${PXC}" == "1" ]; then
  if [ ${QUERIES_PER_THREAD} -lt 2147483647 ]; then  # Starting up a cluster takes more time, so don't rotate too quickly
    echoit "Note: As this is a PXC=1 run, and QUERIES_PER_THREAD was set to only ${QUERIES_PER_THREAD}, this script is setting the queries per thread to the required minimum of 2147483647 for this run."
    QUERIES_PER_THREAD=2147483647  # Max int
  fi
  if [ ${PSTRESS_RUN_TIMEOUT} -lt 60 ]; then  # Starting up a cluster takes more time, so don't rotate too quickly
    echoit "Note: As this is a PXC=1 run, and PSTRESS_RUN_TIMEOUT was set to only ${PSTRESS_RUN_TIMEOUT}, this script is setting the timeout to the required minimum of 60 for this run."
    PSTRESS_RUN_TIMEOUT=60
  fi
  ADD_RANDOM_OPTIONS=0
  ADD_RANDOM_ROCKSDB_OPTIONS=0
  GRP_RPL=0
  GRP_RPL_CLUSTER_RUN=0
fi

if [ "${GRP_RPL}" == "1" ]; then
  if [ ${PSTRESS_RUN_TIMEOUT} -lt 60 ]; then  # Starting up a cluster takes more time, so don't rotate too quickly
    echoit "Note: As this is a GRP_RPL=1 run, and PSTRESS_RUN_TIMEOUT was set to only ${PSTRESS_RUN_TIMEOUT}, this script is setting the timeout to the required minimum of 60 for this run."
    PSTRESS_RUN_TIMEOUT=60
  fi
  ADD_RANDOM_ROCKSDB_OPTIONS=0
  PXC=0
  PXC_CLUSTER_RUN=0
fi

# Both PXC and Group replication expects that all tables must have a Primary key. Also discard tablespace is not supported
if [ ${PXC} -eq 1 -o ${GRP_RPL} -eq 1 ]; then
  if [ "${ENCRYPTION_RUN}" == "1" ]; then
    DYNAMIC_QUERY_PARAMETER="$DYNAMIC_QUERY_PARAMETER --primary-key-probability 100 --alt-discard-tbs 0"
  elif [ "${ENCRYPTION_RUN}" == "0" ]; then
    DYNAMIC_QUERY_PARAMETER="$DYNAMIC_QUERY_PARAMETER --primary-key-probability 100 --alt-discard-tbs 0 --no-encryption"
  fi
elif [ ${PXC} -eq 0 -a ${GRP_RPL} -eq 0 ]; then
  if [ "${ENCRYPTION_RUN}" == "0" ]; then
    DYNAMIC_QUERY_PARAMETER="$DYNAMIC_QUERY_PARAMETER --no-encryption"
  fi
fi

# Disable GCache MK rotation DDL if GCache encryption is not enabled
if [ ${PXC} -eq 1 ]; then
  if [ ${GCACHE_ENCRYPTION} -eq 0 ]; then
    DYNAMIC_QUERY_PARAMETER="$DYNAMIC_QUERY_PARAMETER --rotate-gcache-key 0"
  elif [ ${GCACHE_ENCRYPTION} -eq 1 ]; then
    WSREP_PROVIDER_OPT="gcache.encryption=ON"
  fi
fi

if [ ${THREADS} -eq 1 ]; then
  echoit "MODE: Single threaded pstress testing"
else
  echoit "MODE: Multi threaded pstress testing"
fi
if [ ${THREADS} -gt 1 ]; then  # We may want to drop this to 20 seconds required?
  if [ ${PSTRESS_RUN_TIMEOUT} -lt 30 ]; then
    echoit "Note: As this is a multi-threaded run, and PSTRESS_RUN_TIMEOUT was set to only ${PSTRESS_RUN_TIMEOUT}, this script is setting the timeout to the required minimum of 60 for this run."
    PSTRESS_RUN_TIMEOUT=60
  fi
fi

# Trap ctrl-c
trap ctrl-c SIGINT

ctrl-c(){
  echoit "CTRL+C Was pressed. Attempting to terminate running processes..."
  KILL_PIDS1=`ps -ef | grep "$RANDOMD" | grep -v "grep" | awk '{print $2}' | tr '\n' ' '`
  KILL_PIDS2=
  KILL_PIDS="${KILL_PIDS1} ${KILL_PIDS2}"
  if [ "${KILL_PIDS}" != "" ]; then
    echoit "Terminating the following PID's: ${KILL_PIDS}"
    kill -9 ${KILL_PIDS} >/dev/null 2>&1
  fi
  if [ -d ${RUNDIR}/${TRIAL}/ ]; then
    echoit "Done. Moving the trial $0 was currently working on to workdir as ${WORKDIR}/${TRIAL}/..."
    mv ${RUNDIR}/${TRIAL}/ ${WORKDIR}/ 2>&1 | tee -a /${WORKDIR}/pstress-run.log
  fi
  echoit "Attempting to cleanup the pstress rundir ${RUNDIR}..."
  rm -Rf ${RUNDIR}
  if [ $SAVED -eq 0 -a ${SAVE_SQL} -eq 0 ]; then
    echoit "There were no coredumps saved, and SAVE_SQL=0, so the workdir can be safely deleted. Doing so..."
    WORKDIRACTIVE=0
    rm -Rf ${WORKDIR}
  else
    echoit "The results of this run can be found in the workdir ${WORKDIR}..."
  fi
  echoit "Done. Terminating pstress-run.sh with exit code 2..."
  exit 2
}

savetrial(){  # Only call this if you definitely want to save a trial
  echoit "Copying rundir from ${RUNDIR}/${TRIAL} to ${WORKDIR}/${TRIAL}"
  mv ${RUNDIR}/${TRIAL}/ ${WORKDIR}/ 2>&1 | tee -a /${WORKDIR}/pstress-run.log
  SAVED=$[ $SAVED + 1 ]
}

removetrial(){
  echoit "Removing trial rundir ${RUNDIR}/${TRIAL}"
  if [ "${RUNDIR}" != "" -a "${TRIAL}" != "" -a -d ${RUNDIR}/${TRIAL}/ ]; then  # Protection against dangerous rm's
    rm -Rf ${RUNDIR}/${TRIAL}/
  fi
}

removelasttrial(){
  if [ ${TRIAL} -gt 2 ]; then
    echoit "Removing last successful trial workdir ${WORKDIR}/$((${TRIAL}-2))"
    if [ "${WORKDIR}" != "" -a "${TRIAL}" != "" -a -d ${WORKDIR}/$((${TRIAL}-2))/ ]; then
      rm -Rf ${WORKDIR}/$((${TRIAL}-2))/
    fi
    echoit "Removing the ${WORKDIR}/step_$((${TRIAL}-2)).dll file"
    rm ${WORKDIR}/step_$((${TRIAL}-2)).dll
  fi
}

savesql(){
  echoit "Copying sql trace(s) from ${RUNDIR}/${TRIAL} to ${WORKDIR}/${TRIAL}"
  mkdir ${WORKDIR}/${TRIAL}
  cp ${RUNDIR}/${TRIAL}/*.sql ${WORKDIR}/${TRIAL}/
  rm -Rf ${RUNDIR}/${TRIAL}
  sync; sleep 0.2
  if [ -d ${RUNDIR}/${TRIAL} ]; then
    echoit "Assert: tried to remove ${RUNDIR}/${TRIAL}, but it looks like removal failed. Check what is holding lock? (lsof tool may help)."
    echoit "As this is not necessarily a fatal error (there is likely enough space on ${RUNDIR} to continue working), pstress-run.sh will NOT terminate."
    echoit "However, this looks like a shortcoming in pstress-run.sh (likely in the mysqld termination code) which needs debugging and fixing. Please do."
  fi
}

check_cmd(){
  CMD_PID=$1
  ERROR_MSG=$2
  if [ ${CMD_PID} -ne 0 ]; then echo -e "\nERROR: $ERROR_MSG. Terminating!"; exit 1; fi
}

if [[ $PXC -eq 1 ]];then
  # Creating default my.cnf file
  SUSER=root
  SPASS=
  rm -rf ${BASEDIR}/my.cnf
  echo "[mysqld]" > ${BASEDIR}/my.cnf
  echo "basedir=${BASEDIR}" >> ${BASEDIR}/my.cnf
  echo "wsrep-debug=1" >> ${BASEDIR}/my.cnf
  echo "pxc_strict_mode=ENFORCING" >> ${BASEDIR}/my.cnf
  echo "innodb_file_per_table" >> ${BASEDIR}/my.cnf
  echo "innodb_autoinc_lock_mode=2" >> ${BASEDIR}/my.cnf
  if ! check_for_version $MYSQL_VERSION "8.0.0" ; then
    echo "wsrep_sst_auth=$SUSER:$SPASS" >> ${BASEDIR}/my.cnf
  else
    echo "log-error-verbosity=3" >> ${BASEDIR}/my.cnf
  fi
  echo "wsrep-provider=${BASEDIR}/lib/libgalera_smm.so" >> ${BASEDIR}/my.cnf
  echo "wsrep_sst_method=xtrabackup-v2" >> ${BASEDIR}/my.cnf
  echo "core-file" >> ${BASEDIR}/my.cnf
  echo "log-output=none" >> ${BASEDIR}/my.cnf
  echo "wsrep_slave_threads=2" >> ${BASEDIR}/my.cnf
  echo "gtid_mode=ON" >> ${BASEDIR}/my.cnf
  echo "enforce_gtid_consistency=ON" >> ${BASEDIR}/my.cnf
  echo "master_verify_checksum=on" >> ${BASEDIR}/my.cnf
  echo "binlog_checksum=CRC32" >> ${BASEDIR}/my.cnf
fi
pxc_startup(){
  IS_STARTUP=$1
  ADDR="127.0.0.1"
  RPORT=$(( (RANDOM%21 + 10)*1000 ))
  SOCKET1=${RUNDIR}/${TRIAL}/node1/node1_socket.sock
  SOCKET2=${RUNDIR}/${TRIAL}/node2/node2_socket.sock
  SOCKET3=${RUNDIR}/${TRIAL}/node3/node3_socket.sock
  if check_for_version $MYSQL_VERSION "5.7.0" ; then
    MID="${BASEDIR}/bin/mysqld --no-defaults --initialize-insecure --basedir=${BASEDIR}"
  else
    MID="${BASEDIR}/bin/mysqld --no-defaults --basedir=${BASEDIR}"
  fi

  if [ "$IS_STARTUP" != "startup" ]; then
    echo "echo '=== Starting PXC cluster for recovery...'" > ${RUNDIR}/${TRIAL}/start_pxc_recovery
    echo "sed -i 's|safe_to_bootstrap:.*$|safe_to_bootstrap: 1|' ${WORKDIR}/${TRIAL}/node1/grastate.dat" >> ${RUNDIR}/${TRIAL}/start_pxc_recovery
  fi
  pxc_startup_status(){
    NR=$1
    for X in $(seq 0 ${PXC_START_TIMEOUT}); do
      sleep 1
      if ${BASEDIR}/bin/mysqladmin -uroot -S${SOCKET} ping > /dev/null 2>&1; then
        break
      fi
    done
  }
  unset PXC_PORTS
  unset PXC_LADDRS
  PXC_PORTS=""
  PXC_LADDRS=""
  for i in `seq 1 3`;do
    if [ "$IS_STARTUP" == "startup" ]; then
      node="${WORKDIR}/node${i}.template"
      if ! check_for_version $MYSQL_VERSION "5.7.0" ; then
        mkdir -p $node
      fi
      DATADIR=${WORKDIR}
    else
      node="${RUNDIR}/${TRIAL}/node${i}"
      DATADIR="${RUNDIR}/${TRIAL}"
    fi
    mkdir -p $DATADIR/tmp${i}
    RBASE1="$(( RPORT + ( 100 * $i ) ))"
    LADDR1="127.0.0.1:$(( RBASE1 + 8 ))"
    PXC_PORTS+=("$RBASE1")
    PXC_LADDRS+=("$LADDR1")
    cp ${BASEDIR}/my.cnf ${DATADIR}/n${i}.cnf
    sed -i "2i server-id=10${i}" ${DATADIR}/n${i}.cnf
    sed -i "2i wsrep_node_incoming_address=$ADDR" ${DATADIR}/n${i}.cnf
    sed -i "2i wsrep_node_address=$ADDR" ${DATADIR}/n${i}.cnf
    sed -i "2i log-error=$node/node${i}.err" ${DATADIR}/n${i}.cnf
    sed -i "2i port=$RBASE1" ${DATADIR}/n${i}.cnf
    sed -i "2i datadir=$node" ${DATADIR}/n${i}.cnf
    if [ ${ENCRYPTION_RUN} -eq 1 -a "$IS_STARTUP" != "startup" ]; then
      sed -i "2i pxc_encrypt_cluster_traffic=ON" ${DATADIR}/n${i}.cnf
      if check_for_version $MYSQL_VERSION "8.0.0" ; then
        sed -i "2i binlog_encryption=ON" ${DATADIR}/n${i}.cnf
      fi
      if [ ${KEYRING_FILE} -eq 1 ]; then
        sed -i "2i early-plugin-load=keyring_file.so" ${DATADIR}/n${i}.cnf
        if [ "$IS_STARTUP" == "startup" ]; then
          sed -i "2i keyring_file_data=${DATADIR}/node${i}.template/keyring_storage/keyring" ${DATADIR}/n${i}.cnf
        else
          sed -i "2i keyring_file_data=${DATADIR}/node${i}/keyring_storage/keyring" ${DATADIR}/n${i}.cnf
        fi
      elif [ ${KEYRING_VAULT} -eq 1 ]; then
        sed -i "2i early-plugin-load=keyring_vault.so" ${DATADIR}/n${i}.cnf
        sed -i "2i keyring_vault_config=$WORKDIR/vault/keyring_vault_pxc${i}.cnf" ${DATADIR}/n${i}.cnf
      fi
    fi
    if [ ${ENCRYPTION_RUN} -eq 0 ];then
      sed -i "2i wsrep_provider_options=\"gmcast.listen_addr=tcp://$LADDR1;$WSREP_PROVIDER_OPT\"" ${DATADIR}/n${i}.cnf
    else
      sed -i "2i wsrep_provider_options=\"gmcast.listen_addr=tcp://$LADDR1;$WSREP_PROVIDER_OPT;socket.ssl_key=${WORKDIR}/cert/server-key.pem;socket.ssl_cert=${WORKDIR}/cert/server-cert.pem;socket.ssl_ca=${WORKDIR}/cert/ca.pem\"" ${DATADIR}/n${i}.cnf
    fi
    sed -i "2i socket=$node/node${i}_socket.sock" ${DATADIR}/n${i}.cnf
    sed -i "2i tmpdir=$DATADIR/tmp${i}" ${DATADIR}/n${i}.cnf
    echo "ssl-ca = ${WORKDIR}/cert/ca.pem" >> ${DATADIR}/n${i}.cnf
    echo "ssl-cert = ${WORKDIR}/cert/server-cert.pem" >> ${DATADIR}/n${i}.cnf
    echo "ssl-key = ${WORKDIR}/cert/server-key.pem" >> ${DATADIR}/n${i}.cnf
    echo "[client]" >> ${DATADIR}/n${i}.cnf
    echo "ssl-ca = ${WORKDIR}/cert/ca.pem" >> ${DATADIR}/n${i}.cnf
    echo "ssl-cert = ${WORKDIR}/cert/client-cert.pem" >> ${DATADIR}/n${i}.cnf
    echo "ssl-key = ${WORKDIR}/cert/client-key.pem" >> ${DATADIR}/n${i}.cnf
    echo "[sst]" >> ${DATADIR}/n${i}.cnf
    echo "encrypt = 4" >> ${DATADIR}/n${i}.cnf
    echo "ssl-ca = ${WORKDIR}/cert/ca.pem" >> ${DATADIR}/n${i}.cnf
    echo "ssl-cert = ${WORKDIR}/cert/server-cert.pem" >> ${DATADIR}/n${i}.cnf
    echo "ssl-key = ${WORKDIR}/cert/server-key.pem" >> ${DATADIR}/n${i}.cnf
    if [ "$IS_STARTUP" == "startup" ]; then
      ${MID} --datadir=$node  > ${WORKDIR}/startup_node${i}.err 2>&1 || exit 1;
    fi
  done

  if [ "$IS_STARTUP" == "startup" ]; then
    mkdir ${WORKDIR}/cert
    cp ${WORKDIR}/node1.template/*.pem ${WORKDIR}/cert/
  fi

  get_error_socket_file(){
    NR=$1
    if [ "$IS_STARTUP" == "startup" ]; then
      ERR_FILE="${WORKDIR}/node${NR}.template/node${NR}.err"
      SOCKET="${WORKDIR}/node${NR}.template/node${NR}_socket.sock"
    else
      ERR_FILE="${RUNDIR}/${TRIAL}/node${NR}/node${NR}.err"
      SOCKET="${RUNDIR}/${TRIAL}/node${NR}/node${NR}_socket.sock"
    fi
  }

  sed -i "2i wsrep_cluster_address=gcomm://${PXC_LADDRS[1]},${PXC_LADDRS[2]},${PXC_LADDRS[3]}" ${DATADIR}/n1.cnf
  sed -i "2i wsrep_cluster_address=gcomm://${PXC_LADDRS[1]},${PXC_LADDRS[2]},${PXC_LADDRS[3]}" ${DATADIR}/n2.cnf
  sed -i "2i wsrep_cluster_address=gcomm://${PXC_LADDRS[1]},${PXC_LADDRS[2]},${PXC_LADDRS[3]}" ${DATADIR}/n3.cnf

  get_error_socket_file 1
  ${BASEDIR}/bin/mysqld --defaults-file=${DATADIR}/n1.cnf $STARTUP_OPTION $MYEXTRA $PXC_MYEXTRA --wsrep-new-cluster > ${ERR_FILE} 2>&1 &
  pxc_startup_status 1

  get_error_socket_file 2
  ${BASEDIR}/bin/mysqld --defaults-file=${DATADIR}/n2.cnf $STARTUP_OPTION $MYEXTRA $PXC_MYEXTRA > ${ERR_FILE} 2>&1 &
  pxc_startup_status 2

  get_error_socket_file 3
  ${BASEDIR}/bin/mysqld --defaults-file=${DATADIR}/n3.cnf $STARTUP_OPTION $MYEXTRA $PXC_MYEXTRA > ${ERR_FILE} 2>&1 &
  pxc_startup_status 3
  
  if [ "$IS_STARTUP" == "startup" ]; then
    ${BASEDIR}/bin/mysql -uroot -S${WORKDIR}/node1.template/node1_socket.sock -e "create database if not exists test" > /dev/null 2>&1
  fi
}

gr_startup(){
  IS_STARTUP=$1
  if [ "$1" == "startup" ]; then
    ADDR="127.0.0.1"
    RPORT=$(( RANDOM%21 + 10 ))
    RBASE="$(( RPORT*1000 ))"
    RBASE1="$(( RBASE + 1 ))"
    RBASE2="$(( RBASE + 2 ))"
    RBASE3="$(( RBASE + 3 ))"
    LADDR1="$ADDR:$(( RBASE + 101 ))"
    LADDR2="$ADDR:$(( RBASE + 102 ))"
    LADDR3="$ADDR:$(( RBASE + 103 ))"
  fi
  SOCKET1=${RUNDIR}/${TRIAL}/node1/node1_socket.sock
  SOCKET2=${RUNDIR}/${TRIAL}/node2/node2_socket.sock
  SOCKET3=${RUNDIR}/${TRIAL}/node3/node3_socket.sock

  SUSER=root
  SPASS=

  MID="${BASEDIR}/bin/mysqld --no-defaults --initialize-insecure --basedir=${BASEDIR}"
  if [ "$1" == "startup" ]; then
    if [ ${GRP_RPL_CLUSTER_RUN} -eq 1 ]; then
      MYEXTRA="$MYEXTRA --plugin-load=group_replication.so --group_replication_single_primary_mode=OFF"
    else
      MYEXTRA="$MYEXTRA --plugin-load=group_replication.so"
    fi
  fi
  if [ "$1" == "startup" ]; then
    DATADIR_1="${WORKDIR}/node1.template"
    DATADIR_2="${WORKDIR}/node2.template"
    DATADIR_3="${WORKDIR}/node3.template"
  else
    DATADIR_1="${RUNDIR}/${TRIAL}/node1"
    DATADIR_2="${RUNDIR}/${TRIAL}/node2"
    DATADIR_3="${RUNDIR}/${TRIAL}/node3"
  fi

  if [ "$1" == "startup" ]; then
    ${MID} --datadir=$DATADIR_1  > ${WORKDIR}/startup_node1.err 2>&1
    ${MID} --datadir=$DATADIR_2  > ${WORKDIR}/startup_node2.err 2>&1
    ${MID} --datadir=$DATADIR_3  > ${WORKDIR}/startup_node3.err 2>&1
  fi

  echo "
[mysqld]

# General replication settings

disabled_storage_engines="MyISAM,BLACKHOLE,FEDERATED,ARCHIVE,MEMORY"
gtid_mode = ON
enforce_gtid_consistency = ON
master_info_repository = TABLE
relay_log_info_repository = TABLE
binlog_checksum = NONE
log_slave_updates = ON
log_bin = binlog
binlog_format = ROW
transaction_write_set_extraction = XXHASH64
loose-group_replication_bootstrap_group = OFF
loose-group_replication_start_on_boot = OFF
loose-group_replication_ssl_mode = REQUIRED
loose-group_replication_recovery_use_ssl = 1

# Shared replication group configuration
loose-group_replication_group_name = "$UUID"
loose-group_replication_ip_whitelist = "$ADDR,$ADDR,$ADDR"
loose-group_replication_group_seeds = "$LADDR1,$LADDR2,$LADDR3"

# Host specific replication configuration
server_id = 1
bind-address = "$ADDR"
report_host = "$ADDR"
loose-group_replication_local_address = "$LADDR1"
" > $DATADIR_1/n1.cnf

if [ "${GRP_RPL_CLUSTER_RUN}" == "1" ]; then
  echo "loose-group_replication_single_primary_mode = OFF" >> $DATADIR_1/n1.cnf
  echo "loose-group_replication_enforce_update_everywhere_checks = ON" >> $DATADIR_1/n1.cnf
fi

echo "
[mysqld]

# General replication settings
disabled_storage_engines="MyISAM,BLACKHOLE,FEDERATED,ARCHIVE,MEMORY"
gtid_mode = ON
enforce_gtid_consistency = ON
master_info_repository = TABLE
relay_log_info_repository = TABLE
binlog_checksum = NONE
log_slave_updates = ON
log_bin = binlog
binlog_format = ROW
transaction_write_set_extraction = XXHASH64
loose-group_replication_bootstrap_group = OFF
loose-group_replication_start_on_boot = OFF
loose-group_replication_ssl_mode = REQUIRED
loose-group_replication_recovery_use_ssl = 1
loose-group_replication_group_name = "$UUID"
loose-group_replication_ip_whitelist = "$ADDR,$ADDR,$ADDR"
loose-group_replication_group_seeds = "$LADDR1,$LADDR2,$LADDR3"

# Host specific replication configuration
server_id = 2
bind-address = "$ADDR"
report_host = "$ADDR"
loose-group_replication_local_address = "$LADDR2"
" > $DATADIR_2/n2.cnf

if [ "${GRP_RPL_CLUSTER_RUN}" == "1" ]; then
  echo "loose-group_replication_single_primary_mode = OFF" >> $DATADIR_2/n2.cnf
  echo "loose-group_replication_enforce_update_everywhere_checks = ON" >> $DATADIR_2/n2.cnf
fi

echo "
[mysqld]

# General replication settings
disabled_storage_engines="MyISAM,BLACKHOLE,FEDERATED,ARCHIVE,MEMORY"
gtid_mode = ON
enforce_gtid_consistency = ON
master_info_repository = TABLE
relay_log_info_repository = TABLE
binlog_checksum = NONE
log_slave_updates = ON
log_bin = binlog
binlog_format = ROW
transaction_write_set_extraction = XXHASH64
loose-group_replication_bootstrap_group = OFF
loose-group_replication_start_on_boot = OFF
loose-group_replication_ssl_mode = REQUIRED
loose-group_replication_recovery_use_ssl = 1

# Shared replication group configuration
loose-group_replication_group_name = "$UUID"
loose-group_replication_ip_whitelist = "$ADDR,$ADDR,$ADDR"
loose-group_replication_group_seeds = "$LADDR1,$LADDR2,$LADDR3"

# Host specific replication configuration
server_id = 3
bind-address = "$ADDR"
report_host = "$ADDR"
loose-group_replication_local_address = "$LADDR3"
" > $DATADIR_3/n3.cnf

if [ "${GRP_RPL_CLUSTER_RUN}" == "1" ]; then
  echo "loose-group_replication_single_primary_mode = OFF" >> $DATADIR_3/n3.cnf
  echo "loose-group_replication_enforce_update_everywhere_checks = ON" >> $DATADIR_3/n3.cnf
fi

  get_error_socket_file(){
    NR=$1
    if [ "$IS_STARTUP" == "startup" ]; then
      ERR_FILE="${WORKDIR}/node${NR}.template/node${NR}.err"
      SOCKET="${WORKDIR}/node${NR}.template/node${NR}_socket.sock"
    else
      ERR_FILE="${RUNDIR}/${TRIAL}/node${NR}/node${NR}.err"
      SOCKET="${RUNDIR}/${TRIAL}/node${NR}/node${NR}_socket.sock"
    fi
  }

  gr_startup_status(){
    NR=$1
    for X in $(seq 0 ${GRP_RPL_START_TIMEOUT}); do
      sleep 1
      if ${BASEDIR}/bin/mysqladmin -uroot -S${SOCKET} ping > /dev/null 2>&1; then
        break
      fi
      if [ $X -eq ${GRP_RPL_START_TIMEOUT} ]; then
        echoit "ERROR: Node$NR failed to start within the stipulated ${GRP_RPL_START_TIMEOUT}s timeout period"
	echoit "Check error logs: $ERR_FILE"
	exit 1
      fi
    done
  }

  get_error_socket_file 1
  if [ ${ENCRYPTION_RUN} -eq 1 ]; then
    if [ ${KEYRING_COMPONENT} -eq 1  ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_1/n1.cnf --basedir=${BASEDIR} --datadir=$DATADIR_1 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE1 $MYEXTRA > $ERR_FILE 2>&1 &
    elif [ ${KEYRING_FILE} -eq 1 ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_1/n1.cnf --basedir=${BASEDIR} --datadir=$DATADIR_1 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE1 $MYEXTRA ${KEYRING_PARAM} > $ERR_FILE 2>&1 &
    elif [ ${KEYRING_VAULT} -eq 1 ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_1/n1.cnf --basedir=${BASEDIR} --datadir=$DATADIR_1 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE1 $MYEXTRA ${VAULT_PARAM} > $ERR_FILE 2>&1 &
    else
      echoit "ERROR: Atleast one encryption type must be enabled or else set ENCRYPTION_RUN=0 to continue"
      exit 1
    fi
  else
    ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_1/n1.cnf --basedir=${BASEDIR} --datadir=$DATADIR_1 \
    --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE1 $MYEXTRA > $ERR_FILE 2>&1 &
  fi
  gr_startup_status 1

  if ${BASEDIR}/bin/mysqladmin -uroot -S$SOCKET ping > /dev/null 2>&1; then
    if [ "$1" == "startup" ]; then
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "SET SQL_LOG_BIN=0;CREATE USER rpl_user@'%' IDENTIFIED BY 'rpl_pass' REQUIRE SSL;GRANT REPLICATION SLAVE ON *.* TO rpl_user@'%';FLUSH PRIVILEGES;SET SQL_LOG_BIN=1;" > /dev/null 2>&1
      if [ "$MYSQL_VERSION" == "8.0" ]; then
        ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CHANGE REPLICATION SOURCE TO SOURCE_USER='rpl_user', SOURCE_PASSWORD='rpl_pass' FOR CHANNEL 'group_replication_recovery';" > /dev/null
 2>&1
      elif [ "$MYSQL_VERSION" == "5.7" ]; then
        ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CHANGE MASTER TO MASTER_USER='rpl_user', MASTER_PASSWORD='rpl_pass' FOR CHANNEL 'group_replication_recovery';" > /dev/null 2>&1
      fi
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "SET GLOBAL group_replication_bootstrap_group=ON;START GROUP_REPLICATION;SET GLOBAL group_replication_bootstrap_group=OFF;SELECT SLEEP(10);" > /dev/null 2>&1
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CREATE DATABASE IF NOT EXISTS test" > /dev/null 2>&1
    else
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "SET GLOBAL group_replication_bootstrap_group=ON;START GROUP_REPLICATION;SET GLOBAL group_replication_bootstrap_group=OFF;" > /dev/null 2>&1
      sleep 5;
    fi
  else
    echoit "ERROR: Unable to ping Node 1"
    exit 1
  fi

  get_error_socket_file 2

  if [ "${ENCRYPTION_RUN}" == "1" ]; then
    if [ ${KEYRING_COMPONENT} -eq 1 ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_2/n2.cnf --basedir=${BASEDIR} --datadir=$DATADIR_2 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE2 $MYEXTRA > $ERR_FILE 2>&1 &
    elif [ ${KEYRING_FILE} -eq 1 ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_2/n2.cnf --basedir=${BASEDIR} --datadir=$DATADIR_2 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE2 $MYEXTRA ${KEYRING_PARAM} > $ERR_FILE 2>&1 &
    elif [ ${KEYRING_VAULT} -eq 1 ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_2/n2.cnf --basedir=${BASEDIR} --datadir=$DATADIR_2 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE2 $MYEXTRA ${VAULT_PARAM} > $ERR_FILE 2>&1 &
    else
      echoit "ERROR: Atleast one encryption type must be enabled or else set ENCRYPTION_RUN=0 to continue"
      exit 1
    fi
  else
    ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_2/n2.cnf --basedir=${BASEDIR} --datadir=$DATADIR_2 \
    --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE2 $MYEXTRA > $ERR_FILE 2>&1 &
  fi

  gr_startup_status 2

  if ${BASEDIR}/bin/mysqladmin -uroot -S$SOCKET ping > /dev/null 2>&1; then
    if [ "$1" == "startup" ]; then
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "SET SQL_LOG_BIN=0;CREATE USER rpl_user@'%' IDENTIFIED BY 'rpl_pass' REQUIRE SSL;GRANT REPLICATION SLAVE ON *.* TO rpl_user@'%';FLUSH PRIVILEGES;SET SQL_LOG_BIN=1;" > /dev/null 2>&1
      if [ "$MYSQL_VERSION" == "8.0" ]; then
        ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CHANGE REPLICATION SOURCE TO SOURCE_USER='rpl_user', SOURCE_PASSWORD='rpl_pass' FOR CHANNEL 'group_replication_recovery';" > /dev/null
 2>&1
      elif [ "$MYSQL_VERSION" == "5.7" ]; then
        ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CHANGE MASTER TO MASTER_USER='rpl_user', MASTER_PASSWORD='rpl_pass' FOR CHANNEL 'group_replication_recovery';" > /dev/null 2>&1
      fi
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CHANGE REPLICATION SOURCE TO SOURCE_USER='rpl_user', SOURCE_PASSWORD='rpl_pass' FOR CHANNEL 'group_replication_recovery';" > /dev/null 2>&1
    else
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "START GROUP_REPLICATION;" > /dev/null 2>&1
      sleep 5;
    fi
  else
    echoit "ERROR: Unable to ping Node 2"
    exit 1
  fi

  get_error_socket_file 3

  if [ ${ENCRYPTION_RUN} -eq 1 ]; then
    if [ ${KEYRING_COMPONENT} -eq 1 ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_3/n3.cnf --basedir=${BASEDIR} --datadir=$DATADIR_3 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE3 $MYEXTRA > $ERR_FILE 2>&1 &
    elif [ ${KEYRING_FILE} -eq 1  ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_3/n3.cnf --basedir=${BASEDIR} --datadir=$DATADIR_3 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE3 $MYEXTRA ${KEYRING_PARAM} > $ERR_FILE 2>&1 &
    elif [ ${KEYRING_VAULT} -eq 1 ]; then
      ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_3/n3.cnf --basedir=${BASEDIR} --datadir=$DATADIR_3 \
      --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE3 $MYEXTRA ${VAULT_PARAM} > $ERR_FILE 2>&1 &
    else
      echoit "ERROR: Atleast one encryption type must be enabled or else set ENCRYPTION_RUN=0 to continue"
      exit 1
    fi
  else
    ${BASEDIR}/bin/mysqld --defaults-file=$DATADIR_3/n3.cnf --basedir=${BASEDIR} --datadir=$DATADIR_3 \
    --core-file --log-error=$ERR_FILE --socket=$SOCKET --port=$RBASE3 $MYEXTRA > $ERR_FILE 2>&1 &
  fi

  gr_startup_status 3

  if ${BASEDIR}/bin/mysqladmin -uroot -S$SOCKET ping > /dev/null 2>&1; then
    if [ "$1" == "startup" ]; then
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "SET SQL_LOG_BIN=0;CREATE USER rpl_user@'%' IDENTIFIED BY 'rpl_pass' REQUIRE SSL;GRANT REPLICATION SLAVE ON *.* TO rpl_user@'%';FLUSH PRIVILEGES;SET SQL_LOG_BIN=1;" > /dev/null 2>&1
      if [ "$MYSQL_VERSION" == "8.0" ]; then
        ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CHANGE REPLICATION SOURCE TO SOURCE_USER='rpl_user', SOURCE_PASSWORD='rpl_pass' FOR CHANNEL 'group_replication_recovery';" > /dev/null
 2>&1
      elif [ "$MYSQL_VERSION" == "5.7" ]; then
        ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "CHANGE MASTER TO MASTER_USER='rpl_user', MASTER_PASSWORD='rpl_pass' FOR CHANNEL 'group_replication_recovery';" > /dev/null 2>&1
      fi
    else
      ${BASEDIR}/bin/mysql -uroot -S$SOCKET -Bse "START GROUP_REPLICATION;" > /dev/null 2>&1
      sleep 5;
    fi
  else
    echoit "ERROR: Unable to ping Node 3"
    exit 1
  fi
}

pstress_test(){
  TRIAL=$[ ${TRIAL} + 1 ]
  SOCKET=${RUNDIR}/${TRIAL}/socket.sock
  echoit "====== TRIAL #${TRIAL} ======"
  echoit "Ensuring there are no relevant servers running..."
  KILLPID=$(ps -ef | grep "${RUNDIR}" | grep -v grep | awk '{print $2}' | tr '\n' ' ')
  for i in "${KILLPID[@]}"
  do
    kill_server 9 $i
  done
  echoit "Clearing rundir..."
  rm -Rf ${RUNDIR}/*
  echoit "Generating new trial workdir ${RUNDIR}/${TRIAL}..."
  ISSTARTED=0
  if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
    if check_for_version $MYSQL_VERSION "8.0.0" ; then
      mkdir -p ${RUNDIR}/${TRIAL}/data ${RUNDIR}/${TRIAL}/tmp ${RUNDIR}/${TRIAL}/log  # Cannot create /data/test, /data/mysql in 8.0
    else
      mkdir -p ${RUNDIR}/${TRIAL}/data/test ${RUNDIR}/${TRIAL}/data/mysql ${RUNDIR}/${TRIAL}/tmp ${RUNDIR}/${TRIAL}/log
    fi
    if [[ ${TRIAL} -gt 1 && $REINIT_DATADIR -eq 0 ]]; then
      echoit "Copying datadir from Trial $WORKDIR/$((${TRIAL}-1)) into $WORKDIR/${TRIAL}..."
    else
      echoit "Copying datadir from template..."
    fi
    if [[ ${TRIAL} -gt 1 && $REINIT_DATADIR -eq 0 ]]; then
      rsync -ar --exclude='*core*' ${WORKDIR}/$((${TRIAL}-1))/data/ ${RUNDIR}/${TRIAL}/data 2>&1
      if [ ${KEYRING_COMPONENT} -eq 1 ]; then
        sed -i "s/\/$((${TRIAL}-1))\//\/${TRIAL}\//" ${RUNDIR}/${TRIAL}/data/component_keyring_file.cnf
      fi
    else
      cp -R ${WORKDIR}/data.template/* ${RUNDIR}/${TRIAL}/data 2>&1
      if [ ${KEYRING_COMPONENT} -eq 1 ]; then
        create_local_manifest
        create_local_config
      fi
    fi
    MYEXTRA=
    if [ "${ADD_RANDOM_OPTIONS}" == "" ]; then  # Backwards compatibility for .conf files without this option
       ADD_RANDOM_OPTIONS=0
    fi
    if [ ${ADD_RANDOM_OPTIONS} -eq 1 ]; then  # Add random mysqld --options to MYEXTRA
      OPTIONS_TO_ADD=
      NR_OF_OPTIONS_TO_ADD=$(( RANDOM % MAX_NR_OF_RND_OPTS_TO_ADD + 1 ))
      OPTION_NAME=()
      for X in $(seq 1 ${NR_OF_OPTIONS_TO_ADD}); do
        OPTION_TO_ADD="$(shuf --random-source=/dev/urandom ${OPTIONS_INFILE} | head -n1)"
        if [ ${#OPTION_NAME[@]} -eq 0 ]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        elif [[ ! ${OPTION_NAME[@]} =~ ${OPTION_TO_ADD%=*} ]]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        fi
      done
      echoit "ADD_RANDOM_OPTIONS=1: adding mysqld option(s) ${OPTIONS_TO_ADD} to this run's MYEXTRA..."
      MYEXTRA="${MYEXTRA} ${OPTIONS_TO_ADD}"
    fi
    if [ "${ADD_RANDOM_ROCKSDB_OPTIONS}" == "" ]; then  # Backwards compatibility for .conf files without this option
      ADD_RANDOM_ROCKSDB_OPTIONS=0
    fi
    if [ ${ADD_RANDOM_ROCKSDB_OPTIONS} -eq 1 ]; then  # Add random rocksdb --options to MYEXTRA
      OPTIONS_TO_ADD=
      NR_OF_OPTIONS_TO_ADD=$(( RANDOM % MAX_NR_OF_RND_OPTS_TO_ADD + 1 ))
      OPTION_NAME=()
      for X in $(seq 1 ${NR_OF_OPTIONS_TO_ADD}); do
        OPTION_TO_ADD="$(shuf --random-source=/dev/urandom ${ROCKSDB_OPTIONS_INFILE} | head -n1)"
	if [ ${#OPTION_NAME[@]} -eq 0 ]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        elif [[ ! ${OPTION_NAME[@]} =~ ${OPTION_TO_ADD%=*} ]]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        fi
      done
      echoit "ADD_RANDOM_ROCKSDB_OPTIONS=1: adding RocksDB mysqld option(s) ${OPTIONS_TO_ADD} to this run's MYEXTRA..."
      MYEXTRA="${MYEXTRA} ${OPTIONS_TO_ADD}"
    fi
    echo "${MYEXTRA}" | if grep -qi "innodb[_-]log[_-]checksum[_-]algorithm"; then
      # Ensure that mysqld server startup will not fail due to a mismatched checksum algo between the original MID and the changed MYEXTRA options
      rm ${RUNDIR}/${TRIAL}/data/ib_log*
    fi
    PORT=$[50000 + ( $RANDOM % ( 9999 ) ) ]
    echoit "Starting mysqld. Error log is stored at ${RUNDIR}/${TRIAL}/log/master.err"
    if [ ${ENCRYPTION_RUN} -eq 1 ]; then
      if [ ${KEYRING_VAULT} -eq 1 ]; then
        CMD="${BIN} ${MYEXTRA} ${VAULT_PARAM} --basedir=${BASEDIR} --datadir=${RUNDIR}/${TRIAL}/data \
          --tmpdir=${RUNDIR}/${TRIAL}/tmp --core-file --port=$PORT --pid_file=${RUNDIR}/${TRIAL}/pid.pid --socket=${SOCKET} \
          --log-output=none --log-error-verbosity=3 --log-error=${RUNDIR}/${TRIAL}/log/master.err"
      elif [ ${KEYRING_FILE} -eq 1 ]; then
        CMD="${BIN} ${MYEXTRA} ${KEYRING_PARAM} --basedir=${BASEDIR} --datadir=${RUNDIR}/${TRIAL}/data \
          --tmpdir=${RUNDIR}/${TRIAL}/tmp --core-file --port=$PORT --pid_file=${RUNDIR}/${TRIAL}/pid.pid --socket=${SOCKET} \
          --log-output=none --log-error-verbosity=3 --log-error=${RUNDIR}/${TRIAL}/log/master.err"
      elif [ ${KEYRING_COMPONENT} -eq 1 ]; then
        CMD="${BIN} ${MYEXTRA} --basedir=${BASEDIR} --datadir=${RUNDIR}/${TRIAL}/data \
          --tmpdir=${RUNDIR}/${TRIAL}/tmp --core-file --port=$PORT --pid_file=${RUNDIR}/${TRIAL}/pid.pid --socket=${SOCKET} \
          --log-output=none --log-error-verbosity=3 --log-error=${RUNDIR}/${TRIAL}/log/master.err"
      else
        echoit "ERROR: Atleast one encryption type must be enabled or else set ENCRYPTION_RUN=0 to continue"
        exit 1
      fi
    else
      CMD="${BIN} ${MYEXTRA} --basedir=${BASEDIR} --datadir=${RUNDIR}/${TRIAL}/data \
        --tmpdir=${RUNDIR}/${TRIAL}/tmp --core-file --port=$PORT --pid_file=${RUNDIR}/${TRIAL}/pid.pid --socket=${SOCKET} \
        --log-output=none --log-error-verbosity=3 --log-error=${RUNDIR}/${TRIAL}/log/master.err"
    fi

    echo $CMD
    $CMD > ${RUNDIR}/${TRIAL}/log/master.err 2>&1 &
    MPID="$!"
	
    echoit "Waiting for mysqld (pid: ${MPID}) to fully start..."
    BADVALUE=0
    FAILEDSTARTABORT=0
    for X in $(seq 0 ${MYSQLD_START_TIMEOUT}); do
      sleep 1
      if [ "${MPID}" == "" ]; then echoit "Assert! ${MPID} empty. Terminating!"; exit 1; fi
        if grep -qi "ERROR. Aborting" ${RUNDIR}/${TRIAL}/log/master.err; then
        if grep -qi "TCP.IP port. Address already in use" ${RUNDIR}/${TRIAL}/log/master.err; then
          echoit "Assert! The text '[ERROR] Aborting' was found in the error log due to a IP port conflict (the port was already in use)"
          removetrial
        else
          if [ ${ADD_RANDOM_OPTIONS} -eq 0 ]; then  # Halt for ADD_RANDOM_OPTIONS=0 runs, they should not produce errors like these, as MYEXTRA should be high-quality/non-faulty
            echoit "Assert! '[ERROR] Aborting' was found in the error log. This is likely an issue with one of the \$MEXTRA startup parameters. Saving trial for further analysis, and dumping error log here for quick analysis. Please check the output against the \$MYEXTRA settings."
            grep "ERROR" ${RUNDIR}/${TRIAL}/log/master.err | tee -a /${WORKDIR}/pstress-run.log
            savetrial
            echoit "Remember to cleanup/delete the rundir:  rm -Rf ${RUNDIR}"
            exit 1
          else  # Do not halt for ADD_RANDOM_OPTIONS=1 runs, they are likely to produce errors like these as MYEXTRA was randomly changed
            echoit "'[ERROR] Aborting' was found in the error log. This is likely an issue with one of the MYEXTRA startup parameters. As ADD_RANDOM_OPTIONS=1, this is likely to be encountered. Not saving trial. If you see this error for every trial however, set \$ADD_RANDOM_OPTIONS=0 & try running pstress-run.sh again. If it still fails, your base \$MYEXTRA setting is faulty."
          grep "ERROR" ${RUNDIR}/${TRIAL}/log/master.err | tee -a /${WORKDIR}/pstress-run.log
            FAILEDSTARTABORT=1
            break
          fi
        fi
      fi

      if [ $(ls -l ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null | wc -l) -ge 1 ]; then break; fi  # Break the wait-for-server-started loop if a core file is found. Handling of core is done below.
      # Check if mysqld is alive and if so, set ISSTARTED=1 so pstress will run
      if ${BASEDIR}/bin/mysqladmin -uroot -S${SOCKET} ping > /dev/null 2>&1; then
        ISSTARTED=1
        echoit "Server started ok. Client: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET}"
        ${BASEDIR}/bin/mysql -uroot -S${SOCKET} -e "CREATE DATABASE IF NOT EXISTS test;" > /dev/null 2>&1
        break;
      fi
    done
  elif [[ "${PXC}" == "1" ]]; then
    if [[ ${TRIAL} -gt 1 && $REINIT_DATADIR -eq 0 ]]; then
      mkdir -p ${RUNDIR}/${TRIAL}/
      echoit "Copying datadir from $WORKDIR/$((${TRIAL}-1))/node1 into ${RUNDIR}/${TRIAL}/node1 ..."
      rsync -ar --exclude={'*core*','node1.err'} ${WORKDIR}/$((${TRIAL}-1))/node1/ ${RUNDIR}/${TRIAL}/node1/ 2>&1
      echoit "Copying datadir from $WORKDIR/$((${TRIAL}-1))/node2 into ${RUNDIR}/${TRIAL}/node2 ..."
      rsync -ar --exclude={'*core*','node2.err'} ${WORKDIR}/$((${TRIAL}-1))/node2/ ${RUNDIR}/${TRIAL}/node2/ 2>&1
      echoit "Copying datadir from $WORKDIR/$((${TRIAL}-1))/node3 into ${RUNDIR}/${TRIAL}/node3 ..."
      rsync -ar --exclude={'*core*','node3.err'} ${WORKDIR}/$((${TRIAL}-1))/node3/ ${RUNDIR}/${TRIAL}/node3/ 2>&1
      sed -i 's|safe_to_bootstrap:.*$|safe_to_bootstrap: 1|' ${RUNDIR}/${TRIAL}/node1/grastate.dat
      if [ ${KEYRING_COMPONENT} -eq 1 ]; then
        sed -i "s/\/$((${TRIAL}-1))\//\/${TRIAL}\//" ${RUNDIR}/${TRIAL}/node1/component_keyring_file.cnf
        sed -i "s/\/$((${TRIAL}-1))\//\/${TRIAL}\//" ${RUNDIR}/${TRIAL}/node2/component_keyring_file.cnf
        sed -i "s/\/$((${TRIAL}-1))\//\/${TRIAL}\//" ${RUNDIR}/${TRIAL}/node3/component_keyring_file.cnf
      fi
    else
      mkdir -p ${RUNDIR}/${TRIAL}/
      echoit "Copying datadir from template..."
      cp -R ${WORKDIR}/node1.template ${RUNDIR}/${TRIAL}/node1 2>&1
      cp -R ${WORKDIR}/node2.template ${RUNDIR}/${TRIAL}/node2 2>&1
      cp -R ${WORKDIR}/node3.template ${RUNDIR}/${TRIAL}/node3 2>&1
      if [ ${KEYRING_COMPONENT} -eq 1 ]; then
	create_local_manifest 1
	create_local_manifest 2
	create_local_manifest 3
	create_local_config 1
	create_local_config 2
	create_local_config 3
      fi
    fi

    PXC_MYEXTRA=
    # === PXC Options Stage 1: Add random mysqld options to PXC_MYEXTRA
    if [ ${PXC_ADD_RANDOM_OPTIONS} -eq 1 ]; then
      OPTIONS_TO_ADD=
      NR_OF_OPTIONS_TO_ADD=$(( RANDOM % PXC_MAX_NR_OF_RND_OPTS_TO_ADD + 1 ))
      OPTION_NAME=()
      for X in $(seq 1 ${NR_OF_OPTIONS_TO_ADD}); do
	OPTION_TO_ADD="$(shuf --random-source=/dev/urandom ${PXC_OPTIONS_INFILE} | head -n1)"
        if [ ${#OPTION_NAME[@]} -eq 0 ]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        elif [[ ! ${OPTION_NAME[@]} =~ ${OPTION_TO_ADD%=*} ]]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        fi
      done
      echoit "PXC_ADD_RANDOM_OPTIONS=1: adding mysqld option(s) ${OPTIONS_TO_ADD} to this run's PXC_MYEXTRA..."
      PXC_MYEXTRA="${OPTIONS_TO_ADD}"
    fi
    # === PXC Options Stage 2: Add random wsrep mysqld options to PXC_MYEXTRA
    if [ ${PXC_WSREP_ADD_RANDOM_WSREP_MYSQLD_OPTIONS} -eq 1 ]; then
      OPTIONS_TO_ADD=
      NR_OF_OPTIONS_TO_ADD=$(( RANDOM % PXC_WSREP_MAX_NR_OF_RND_OPTS_TO_ADD + 1 ))
      OPTION_NAME=()
      for X in $(seq 1 ${NR_OF_OPTIONS_TO_ADD}); do
        OPTION_TO_ADD="$(shuf --random-source=/dev/urandom ${PXC_WSREP_OPTIONS_INFILE} | head -n1)"
	if [ ${#OPTION_NAME[@]} -eq 0 ]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        elif [[ ! ${OPTION_NAME[@]} =~ ${OPTION_TO_ADD%=*} ]]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        fi
      done
      echoit "PXC_WSREP_ADD_RANDOM_WSREP_MYSQLD_OPTIONS=1: adding wsrep provider mysqld option(s) ${OPTIONS_TO_ADD} to this run's PXC_MYEXTRA..."
      PXC_MYEXTRA="${PXC_MYEXTRA} ${OPTIONS_TO_ADD}"
    fi
    # === PXC Options Stage 3: Add random wsrep (Galera) configuration options
    if [ ${PXC_WSREP_PROVIDER_ADD_RANDOM_WSREP_PROVIDER_CONFIG_OPTIONS} -eq 1 ]; then
      OPTIONS_TO_ADD=
      NR_OF_OPTIONS_TO_ADD=$(( RANDOM % PXC_WSREP_PROVIDER_MAX_NR_OF_RND_OPTS_TO_ADD + 1 ))
      OPTION_NAME=()
      for X in $(seq 1 ${NR_OF_OPTIONS_TO_ADD}); do
        OPTION_TO_ADD="$(shuf --random-source=/dev/urandom ${PXC_WSREP_PROVIDER_OPTIONS_INFILE} | head -n1)"
        if [ ${#OPTION_NAME[@]} -eq 0 ]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        elif [[ ! ${OPTION_NAME[@]} =~ ${OPTION_TO_ADD%=*} ]]; then
          OPTIONS_TO_ADD="$OPTIONS_TO_ADD $OPTION_TO_ADD"
          OPTION_NAME+=(${OPTION_TO_ADD%=*})
        fi
      done
      echoit "PXC_WSREP_PROVIDER_ADD_RANDOM_WSREP_PROVIDER_CONFIG_OPTIONS=1: adding wsrep provider configuration option(s) ${OPTIONS_TO_ADD} to this run..."
      WSREP_PROVIDER_OPT="$OPTIONS_TO_ADD"
    fi
    echo "${MYEXTRA} ${KEYRING_PARAM} ${PXC_MYEXTRA}" > ${RUNDIR}/${TRIAL}/MYEXTRA
    echo "${MYINIT}" > ${RUNDIR}/${TRIAL}/MYINIT
    echo "$WSREP_PROVIDER_OPT" > ${RUNDIR}/${TRIAL}/WSREP_PROVIDER_OPT
    pxc_startup
    echoit "Checking 3 node PXC Cluster startup..."
    for X in $(seq 0 10); do
      sleep 1
      CLUSTER_UP=0;
      if ${BASEDIR}/bin/mysqladmin -uroot -S${SOCKET1} ping > /dev/null 2>&1; then
        if [ `${BASEDIR}/bin/mysql -uroot -S${SOCKET1} -e"show global status like 'wsrep_cluster_size'" | sed 's/[| \t]\+/\t/g' | grep "wsrep_cluster" | awk '{print $2}'` -eq 3 ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
        if [ `${BASEDIR}/bin/mysql -uroot -S${SOCKET2} -e"show global status like 'wsrep_cluster_size'" | sed 's/[| \t]\+/\t/g' | grep "wsrep_cluster" | awk '{print $2}'` -eq 3 ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
        if [ `${BASEDIR}/bin/mysql -uroot -S${SOCKET3} -e"show global status like 'wsrep_cluster_size'" | sed 's/[| \t]\+/\t/g' | grep "wsrep_cluster" | awk '{print $2}'` -eq 3 ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
        if [ "`${BASEDIR}/bin/mysql -uroot -S${SOCKET1} -e"show global status like 'wsrep_local_state_comment'" | sed 's/[| \t]\+/\t/g' | grep "wsrep_local" | awk '{print $2}'`" == "Synced" ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
        if [ "`${BASEDIR}/bin/mysql -uroot -S${SOCKET2} -e"show global status like 'wsrep_local_state_comment'" | sed 's/[| \t]\+/\t/g' | grep "wsrep_local" | awk '{print $2}'`" == "Synced" ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
        if [ "`${BASEDIR}/bin/mysql -uroot -S${SOCKET3} -e"show global status like 'wsrep_local_state_comment'" | sed 's/[| \t]\+/\t/g' | grep "wsrep_local" | awk '{print $2}'`" == "Synced" ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
      fi
      # If count reached 6 (there are 6 checks), then the Cluster is up & running and consistent in it's Cluster topology views (as seen by each node)
      if [ ${CLUSTER_UP} -eq 6 ]; then
        ISSTARTED=1
        echoit "3 Node PXC Cluster started ok. Clients:"
        echoit "Node #1: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET1}"
        echoit "Node #2: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET2}"
        echoit "Node #3: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET3}"
        break
      fi
    done
  elif [[ ${GRP_RPL} -eq 1 ]]; then
    if [[ ${TRIAL} -gt 1 && $REINIT_DATADIR -eq 0 ]]; then
      mkdir -p ${RUNDIR}/${TRIAL}/
      echoit "Copying datadir from $WORKDIR/$((${TRIAL}-1))/node1 into ${RUNDIR}/${TRIAL}/node1 ..."
      rsync -ar --exclude={'*core*','node1.err'} ${WORKDIR}/$((${TRIAL}-1))/node1/ ${RUNDIR}/${TRIAL}/node1/ 2>&1
      echoit "Copying datadir from $WORKDIR/$((${TRIAL}-1))/node2 into ${RUNDIR}/${TRIAL}/node2 ..."
      rsync -ar --exclude={'*core*','node2.err'} ${WORKDIR}/$((${TRIAL}-1))/node2/ ${RUNDIR}/${TRIAL}/node2/ 2>&1
      echoit "Copying datadir from $WORKDIR/$((${TRIAL}-1))/node3 into ${RUNDIR}/${TRIAL}/node3 ..."
      rsync -ar --exclude={'*core*','node3.err'} ${WORKDIR}/$((${TRIAL}-1))/node3/ ${RUNDIR}/${TRIAL}/node3/ 2>&1
      for i in $(seq 1 3); do
        if [ ${KEYRING_COMPONENT} -eq 1 -a ${ENCRYPTION_RUN} -eq 1 ]; then
          sed -i "s/\/$((${TRIAL}-1))\//\/${TRIAL}\//" ${RUNDIR}/${TRIAL}/node$i/component_keyring_file.cnf
        fi
      done
    else
      mkdir -p ${RUNDIR}/${TRIAL}/
      echoit "Copying datadir from template..."
      cp -R ${WORKDIR}/node1.template ${RUNDIR}/${TRIAL}/node1 2>&1
      cp -R ${WORKDIR}/node2.template ${RUNDIR}/${TRIAL}/node2 2>&1
      cp -R ${WORKDIR}/node3.template ${RUNDIR}/${TRIAL}/node3 2>&1
      if [ ${KEYRING_COMPONENT} -eq 1 -a ${ENCRYPTION_RUN} -eq 1 ]; then
        for i in $(seq 1 3); do
          echoit "Creating local manifest file mysqld.my for node$i"
          cat << EOF >${RUNDIR}/${TRIAL}/node$i/mysqld.my
{
 "components": "file://component_keyring_file"
}
EOF
          echoit "Creating local configuration file component_keyring_file.cnf for node$i"
          cat << EOF >${RUNDIR}/${TRIAL}/node$i/component_keyring_file.cnf
{
 "path": "${RUNDIR}/${TRIAL}/node$i/component_keyring_file",
 "read_only": false
}
EOF
        done
      fi
    fi

    gr_startup
    echoit "Checking 3 node Group Replication Cluster startup..."
    for X in $(seq 1 3); do
      sleep 10
      CLUSTER_UP=0;
      if [ `${BASEDIR}/bin/mysql -uroot -S${SOCKET1} -Bse "select count(1) from performance_schema.replication_group_members where member_state='ONLINE'"` -eq 3 ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
      if [ `${BASEDIR}/bin/mysql -uroot -S${SOCKET2} -Bse "select count(1) from performance_schema.replication_group_members where member_state='ONLINE'"` -eq 3 ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
      if [ `${BASEDIR}/bin/mysql -uroot -S${SOCKET3} -Bse "select count(1) from performance_schema.replication_group_members where member_state='ONLINE'"` -eq 3 ]; then CLUSTER_UP=$[ ${CLUSTER_UP} + 1]; fi
      # If count reached 3, then the Cluster is up & running and consistent in it's Cluster topology views (as seen by each node)
      if [ ${CLUSTER_UP} -eq 3 ]; then
        ISSTARTED=1
        echoit "3 Node Group Replication Cluster started ok. Clients:"
        echoit "Node #1: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET1}"
        echoit "Node #2: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET2}"
        echoit "Node #3: `echo ${BIN} | sed 's|/mysqld|/mysql|'` -uroot -S${SOCKET3}"
	break
      fi
      if [ $X -eq 3 ]; then
        echoit "Node inconsistency detected or server crashed. Check error logs for details."
      fi
    done
  fi

  if [ ${ISSTARTED} -eq 1 ]; then
    rm -f ${RUNDIR}/${TRIAL}/startup_failure_thread-0.sql  # Remove the earlier created fake (SELECT 1; only) file present for startup issues (server is started OK now)
    if [[ "${TRIAL}" == "1" || $REINIT_DATADIR -eq 1 ]]; then
      echoit "Creating metadata randomly using random seed ${SEED} ..."
    else
      echoit "Loading metadata from ${WORKDIR}/step_$((${TRIAL}-1)).dll ..."
    fi

    if [ ${ENGINE} == "RocksDB" ]; then
      if [[ ${TRIAL} -eq 1 || $REINIT_DATADIR -eq 1 ]]; then
        ${BASEDIR}/bin/ps-admin --enable-rocksdb -uroot -S${SOCKET}
      fi
    fi

    if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
      CMD="${PSTRESS_BIN} --database=test --threads=${THREADS} --queries-per-thread=${QUERIES_PER_THREAD} --logdir=${RUNDIR}/${TRIAL} --user=root --socket=${SOCKET} --seed ${SEED} --step ${TRIAL} --metadata-path ${WORKDIR}/ --seconds ${PSTRESS_RUN_TIMEOUT} ${DYNAMIC_QUERY_PARAMETER} --engine=${ENGINE}"
    elif [ ${PXC_CLUSTER_RUN} -eq 1 ]; then
      cat ${PXC_CLUSTER_CONFIG} \
          | sed -e "s|\/tmp|${RUNDIR}\/${TRIAL}|" \
          > ${RUNDIR}/${TRIAL}/pstress-cluster-run.cfg
      CMD="${PSTRESS_BIN} --database=test --config-file=${RUNDIR}/${TRIAL}/pstress-cluster-run.cfg --queries-per-thread=${QUERIES_PER_THREAD} --seed ${SEED} --step ${TRIAL} --metadata-path ${WORKDIR}/ --seconds ${PSTRESS_RUN_TIMEOUT} ${DYNAMIC_QUERY_PARAMETER}"
    elif [ ${GRP_RPL_CLUSTER_RUN} -eq 1 ]; then
      cat ${GR_CLUSTER_CONFIG} \
          | sed -e "s|\/tmp|${RUNDIR}\/${TRIAL}|" \
          > ${RUNDIR}/${TRIAL}/pstress-cluster-run.cfg
      CMD="${PSTRESS_BIN} --database=test --config-file=${RUNDIR}/${TRIAL}/pstress-cluster-run.cfg --queries-per-thread=${QUERIES_PER_THREAD} --seed ${SEED} --step ${TRIAL} --metadata-path ${WORKDIR}/ --seconds ${PSTRESS_RUN_TIMEOUT} ${DYNAMIC_QUERY_PARAMETER}"
   else
      CMD="${PSTRESS_BIN} --database=test --threads=${THREADS} --queries-per-thread=${QUERIES_PER_THREAD} --logdir=${RUNDIR}/${TRIAL}/ --user=root --socket=${SOCKET1} --seed ${SEED} --step ${TRIAL} --metadata-path ${WORKDIR}/ --seconds ${PSTRESS_RUN_TIMEOUT} ${DYNAMIC_QUERY_PARAMETER}"
    fi
    if [ $REINIT_DATADIR -eq 1 ]; then
      CMD="$CMD --prepare"
      REINIT_DATADIR=0
    fi

    echoit "$CMD"
    $CMD >${RUNDIR}/${TRIAL}/pstress.log 2>&1 &
    PQPID="$!"
    TIMEOUT_REACHED=0
    echoit "pstress running (Max duration: ${PSTRESS_RUN_TIMEOUT}s)..."
    for X in $(seq 1 ${PSTRESS_RUN_TIMEOUT}); do
      sleep 1
      if grep -qi "error while loading shared libraries" ${RUNDIR}/${TRIAL}/pstress.log; then
        if grep -qi "error while loading shared libraries.*libssl" ${RUNDIR}/${TRIAL}/pstress.log; then
          echoit "$(grep -i "error while loading shared libraries" ${RUNDIR}/${TRIAL}/pstress.log)"
          echoit "Assert: There was an error loading the shared/dynamic libssl library linked to from within pstress. You may want to try and install a package similar to libssl-dev. If that is already there, try instead to build pstress on this particular machine. Sometimes there are differences seen between Centos and Ubuntu. Perhaps we need to have a pstress build for each of those separately."
        else
          echoit "Assert: There was an error loading the shared/dynamic mysql client library linked to from within pstress. Ref. ${RUNDIR}/${TRIAL}/pstress.log to see the error. The solution is to ensure that LD_LIBRARY_PATH is set correctly (for example: execute '$ export LD_LIBRARY_PATH=<your_mysql_base_directory>/lib' in your shell. This will happen only if you use pstress without statically linked client libraries, and this in turn would happen only if you compiled pstress yourself instead of using the pre-built binaries available in https://github.com/Percona-QA/percona-qa (ref subdirectory/files ./pstress/pstress*) - which are normally used by this script (hence this situation is odd to start with). The pstress binaries in percona-qa all include a statically linked mysql client library matching the mysql flavor (PS,MS,MD,WS) it was built for. Another reason for this error may be that (having used pstress without statically linked client binaries as mentioned earlier) the client libraries are not available at the location set in LD_LIBRARY_PATH (which is currently set to '${LD_LIBRARY_PATH}'."
        fi
        exit 1
      fi
      if [ "`ps -ef | grep ${PQPID} | grep -v grep`" == "" ]; then  # pstress ended
        break
      fi
      if [ $X -ge ${PSTRESS_RUN_TIMEOUT} ]; then
        echoit "${PSTRESS_RUN_TIMEOUT}s timeout reached. Terminating this trial..."
        TIMEOUT_REACHED=1
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
    if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
      echoit "Server (PID: ${MPID} | Socket: ${SOCKET}) failed to start after ${MYSQLD_START_TIMEOUT} seconds. Will issue extra kill -9 to ensure it's gone..."
      kill_server 9 ${MPID}
      SERVER_FAIL_TO_START_COUNT=$[ $SERVER_FAIL_TO_START_COUNT + 1 ]
      if [ $SERVER_FAIL_TO_START_COUNT -gt 0 ]; then
        echoit "Server failed to start. Reinitializing the data directory"
        REINIT_DATADIR=1
      fi
    elif [[ ${PXC} -eq 1 ]]; then
      echoit "3 Node PXC Cluster failed to start after ${PXC_START_TIMEOUT} seconds. Will issue an extra cleanup to ensure nothing remains..."
      MPID=( $(ps -ef | grep -e 'n[0-9].cnf' | grep ${RUNDIR} | grep -v grep | awk '{print $2}') )
      for i in "${MPID[@]}"
      do
        kill_server 9 $i
      done
      SERVER_FAIL_TO_START_COUNT=$[ $SERVER_FAIL_TO_START_COUNT + 1 ]
      if [ $SERVER_FAIL_TO_START_COUNT -gt 0 ]; then
        echoit "Server failed to start. Reinitializing the data directory"
        REINIT_DATADIR=1
      fi
    elif [[ ${GRP_RPL} -eq 1 ]]; then
      echoit "3 Node Group Replication Cluster failed to start after ${GRP_RPL_START_TIMEOUT} seconds. Will issue an extra cleanup to ensure nothing remains..."
      MPID=( $(ps -ef | grep -e 'n[0-9].cnf' | grep ${RUNDIR} | grep -v grep | awk '{print $2}') )
      for i in "${MPID[@]}"
      do
        kill_server 9 $i
      done
      SERVER_FAIL_TO_START_COUNT=$[ $SERVER_FAIL_TO_START_COUNT + 1 ]
      if [ $SERVER_FAIL_TO_START_COUNT -gt 0 ]; then
	echoit "Server failed to start. Reinitializing the data directory"
        REINIT_DATADIR=1
      fi
    fi
  fi
  echoit "Cleaning up & saving results if needed..."
  TRIAL_SAVED=0;
  sleep 2  # Delay to ensure core was written completely (if any)
  # NOTE**: Do not kill PQPID here/before shutdown. The reason is that pstress may still be writing queries it's executing to the log. The only way to halt pstress properly is by
  # actually shutting down the server which will auto-terminate pstress due to 250 consecutive queries failing. If 250 queries failed and ${PSTRESS_RUN_TIMEOUT}s timeout was reached,
  # and if there is no core and there is no output of percona-qa/search_string.sh either (in case core dumps are not configured correctly, and thus no core file is
  # generated, search_string.sh will still produce output in case the server crashed based on the information in the error log), then we do not need to save this trial (as it is a
  # standard occurence for this to happen). If however we saw 250 queries failed before the timeout was complete, then there may be another problem and the trial should be saved.
  if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
    echoit "Killing the server with Signal $SIGNAL";
    kill_server $SIGNAL $MPID
    sleep 1  # <^ Make sure all is gone
  elif [[ ${PXC} -eq 1 || ${GRP_RPL} -eq 1 ]]; then
    MPID=( $(ps -ef | grep -e 'n[0-9].cnf' | grep ${RUNDIR} | grep -v grep | awk '{print $2}') )
    if [ ${PXC} -eq 1 ]; then
      echoit "Killing the PXC servers with Signal ${SIGNAL}"
    else
      echoit "Killing the Group replication servers with Signal ${SIGNAL}"
    fi
    for i in "${MPID[@]}"
    do
      kill_server $SIGNAL $i
    done
  fi
  if [ ${ISSTARTED} -eq 1 -a ${TRIAL_SAVED} -ne 1 ]; then  # Do not try and print pstress log for a failed mysqld start
    echoit "pstress run details:$(grep -i 'SUMMARY.*queries failed' ${RUNDIR}/${TRIAL}/*.sql ${RUNDIR}/${TRIAL}/*.log | sed 's|.*:||')"
  fi
  if [ ${TRIAL_SAVED} -eq 0 ]; then
    if [[ ${SIGNAL} -ne 4 ]]; then
      if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
        ISSUE_FOUND=0
        if [ $(ls -l ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null | wc -l) -ge 1 ]; then
          echoit "mysqld coredump detected at $(ls ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null)"
          ISSUE_FOUND=1
        elif [ "$(${SCRIPT_PWD}/search_string.sh ${RUNDIR}/${TRIAL}/log/master.err 2>/dev/null)" != "" ]; then
          echoit "mysqld error detected in the log via search_string.sh scan"
          ISSUE_FOUND=1
        fi
        if [ $ISSUE_FOUND = 1 ]; then
          echoit "Bug found (as per error log): $(${SCRIPT_PWD}/search_string.sh ${RUNDIR}/${TRIAL}/log/master.err)"
        fi
      elif [[ ${PXC} -eq 1 || ${GRP_RPL} -eq 1 ]]; then
        if [ $(ls -l ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null | wc -l) -ge 1 ]; then
          echoit "mysqld coredump detected at $(ls ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null)"
        else
          echoit "mysqld error detected in the log via search_string.sh scan"
          pxc_bug_found 3
        fi
      fi
      savetrial
      TRIAL_SAVED=1
    elif [ ${SIGNAL} -eq 4 ]; then
      if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
        ISSUE_FOUND=0
        if [[ $(grep -i "mysqld got signal 4" ${RUNDIR}/${TRIAL}/log/master.err 2>/dev/null | wc -l) -ge 1 ]]; then
          echoit "mysqld coredump detected due to SIGNAL(kill -4) at $(ls ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null)"
        else
          echoit "mysqld coredump detected at $(ls ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null)"
          ISSUE_FOUND=1
        fi
        if [ "$(${SCRIPT_PWD}/search_string.sh ${RUNDIR}/${TRIAL}/log/master.err 2>/dev/null)" != "" ]; then
          echoit "mysqld error detected in the log via search_string.sh scan"
          ISSUE_FOUND=1
        fi
        if [ $ISSUE_FOUND = 1 ]; then
          echoit "Bug found (as per error log): $(${SCRIPT_PWD}/search_string.sh ${RUNDIR}/${TRIAL}/log/master.err)"
        fi
      elif [[ ${PXC} -eq 1 || ${GRP_RPL} -eq 1 ]]; then
        if [[ $(grep -i "mysqld got signal 4" ${RUNDIR}/${TRIAL}/node1/node1.err 2>/dev/null | wc -l) -ge 1 || $(grep -i "mysqld got signal 4" ${RUNDIR}/${TRIAL}/node2/node2.err 2>/dev/null | wc -l) -ge 1 || $(grep -i "mysqld got signal 4" ${RUNDIR}/${TRIAL}/node3/node3.err 2>/dev/null | wc -l) -ge 1 ]]; then
          echoit "mysqld coredump detected due to SIGNAL(kill -4) at $(ls ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null)"
        else
          echoit "mysqld coredump detected at $(ls ${RUNDIR}/${TRIAL}/*/*core* 2>/dev/null)"
        fi
      fi
      pxc_bug_found 3
      savetrial
      TRIAL_SAVED=1
    elif [ $(grep "SIGKILL myself" ${RUNDIR}/${TRIAL}/log/master.err 2>/dev/null | wc -l) -ge 1 ]; then
      echoit "'SIGKILL myself' detected in the mysqld error log for this trial; saving this trial"
      savetrial
      TRIAL_SAVED=1
    elif [ $(grep "MySQL server has gone away" ${RUNDIR}/${TRIAL}/*.sql 2>/dev/null | wc -l) -ge 200 -a ${TIMEOUT_REACHED} -eq 0 ]; then
      echoit "'MySQL server has gone away' detected >=200 times for this trial, and the pstress timeout was not reached; saving this trial for further analysis"
      savetrial
      TRIAL_SAVED=1
    elif [ $(grep "ERROR:" ${RUNDIR}/${TRIAL}/log/master.err 2>/dev/null | wc -l) -ge 1 ]; then
      echoit "ASAN issue detected in the mysqld error log for this trial; saving this trial"
      savetrial
      TRIAL_SAVED=1
    elif [ ${SAVE_TRIALS_WITH_CORE_ONLY} -eq 0 ]; then
      echoit "Saving full trial outcome (as SAVE_TRIALS_WITH_CORE_ONLY=0 and so trials are saved irrespective of whether an issue was detected or not)"
      savetrial
      TRIAL_SAVED=1
    elif [ ${TRIAL} -gt 1 ]; then
       savetrial
       removelasttrial
       TRIAL_SAVED=1
    elif [ ${TRIAL} -eq 1 ]; then
       savetrial
       TRIAL_SAVED=1
    elif [[ ${CRASH_CHECK} -eq 1 ]]; then
      echoit "Saving this trial for backup restore analysis"
      savetrial
      TRIAL_SAVED=1
      CRASH_CHECK=0
    else
      if [ ${SAVE_SQL} -eq 1 ]; then
        echoit "Not saving anything for this trial (as SAVE_TRIALS_WITH_CORE_ONLY=1, and no issue was seen), except the SQL trace (as SAVE_SQL=1)"
        savesql
      else
        echoit "Not saving anything for this trial (as SAVE_TRIALS_WITH_CORE_ONLY=1 and SAVE_SQL=0, and no issue was seen)"
      fi
    fi
  fi
  if [ ${TRIAL_SAVED} -eq 0 ]; then
    removetrial
  fi
}

# Setup
if [[ "${INFILE}" == *".tar."* ]]; then
  echoit "The input file is a compressed tarball. This script will untar the file in the same location as the tarball. Please note this overwrites any existing files with the same names as those in the tarball, if any. If the sql input file needs patching (and is part of the github repo), please remember to update the tarball with the new file."
  STORECURPWD=${PWD}
  cd $(echo ${INFILE} | sed 's|/[^/]\+\.tar\..*|/|')  # Change to the directory containing the input file
  tar -xf ${INFILE}
  cd ${STORECURPWD}
  INFILE=$(echo ${INFILE} | sed 's|\.tar\..*||')
fi
rm -Rf ${WORKDIR} ${RUNDIR}
mkdir ${WORKDIR} ${WORKDIR}/log ${RUNDIR}
WORKDIRACTIVE=1
ONGOING=
# User for recovery testing
echo "create user recovery@'%';grant all on *.* to recovery@'%';flush privileges;" > ${WORKDIR}/recovery-user.sql
if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
  ONGOING="Workdir: ${WORKDIR} | Rundir: ${RUNDIR} | Basedir: ${BASEDIR} "
  echoit "${ONGOING}"
elif [[ ${PXC} -eq 1 ]]; then
  ONGOING="Workdir: ${WORKDIR} | Rundir: ${RUNDIR} | Basedir: ${BASEDIR} | PXC Mode: TRUE | PXC START TIMEOUT: ${PXC_START_TIMEOUT}"
  echoit "${ONGOING}"
  if [ ${PXC_CLUSTER_RUN} -eq 1 ]; then
    echoit "PXC Cluster run: 'YES'"
  else
    echoit "PXC Cluster run: 'NO'"
  fi
  if [ ${ENCRYPTION_RUN} -eq 1 ]; then
    if [ ${GCACHE_ENCRYPTION} -eq 1 ]; then
      echoit "PXC Encryption run: 'YES' | GCache Encryption: 'YES'"
    else
      echoit "PXC Encryption run: 'YES' | GCache Encryption: 'NO'"
    fi
  else
    echoit "PXC Encryption run: 'NO'"
  fi
elif [[ ${GRP_RPL} -eq 1 ]]; then
  ONGOING="Workdir: ${WORKDIR} | Rundir: ${RUNDIR} | Basedir: ${BASEDIR} | Group Replication Mode: TRUE | GR START TIMEOUT: ${GRP_RPL_START_TIMEOUT}"
  echoit "${ONGOING}"
  if [ ${GRP_RPL_CLUSTER_RUN} -eq 1 ]; then
    echoit "Group Replication Cluster run: 'YES'"
  else
    echoit "Group Replication Cluster run: 'NO'"
  fi
  if [ ${ENCRYPTION_RUN} -eq 1 ]; then
    echoit "GR Encryption run: 'YES'"
  else
    echoit "GR Encryption run: 'NO'"
  fi
fi
ONGOING=

echoit "mysqld Start Timeout: ${MYSQLD_START_TIMEOUT} | Client Threads: ${THREADS} | Queries/Thread: ${QUERIES_PER_THREAD} | Trials: ${TRIALS} | Save coredump issue trials only: `if [ ${SAVE_TRIALS_WITH_CORE_ONLY} -eq 1 ]; then echo -n 'TRUE'; if [ ${SAVE_SQL} -eq 1 ]; then echo ' + save all SQL traces'; else echo ''; fi; else echo 'FALSE'; fi`"

echoit "Storage Engine: ${ENGINE}"
SQL_INPUT_TEXT="SQL file used: ${INFILE}"
echoit "pstress timeout: ${PSTRESS_RUN_TIMEOUT}"
echoit "pstress Binary: ${PSTRESS_BIN}"
if [ "${MYINIT}" != "" ]; then echoit "MYINIT: ${MYINIT}"; fi
if [ "${MYEXTRA}" != "" ]; then echoit "MYEXTRA: ${MYEXTRA}"; fi
echoit "Making a copy of the pstress binary used (${PSTRESS_BIN}) to ${WORKDIR}/ (handy for later re-runs/reference etc.)"
cp ${PSTRESS_BIN} ${WORKDIR}
if [ ${STORE_COPY_OF_INFILE} -eq 1 ]; then
  echoit "Making a copy of the SQL input file used (${INFILE}) to ${WORKDIR}/ for reference..."
  cp ${INFILE} ${WORKDIR}
fi

# Get version specific options
MID=
if [ -r ${BASEDIR}/scripts/mysql_install_db ]; then MID="${BASEDIR}/scripts/mysql_install_db"; fi
if [ -r ${BASEDIR}/bin/mysql_install_db ]; then MID="${BASEDIR}/bin/mysql_install_db"; fi
START_OPT="--core-file"  # Compatible with 5.6,5.7,8.0
INIT_OPT="--no-defaults --initialize-insecure ${MYINIT}"  # Compatible with 5.7,8.0 (mysqld init)
INIT_TOOL="${BIN}"  # Compatible with 5.7,8.0 (mysqld init), changed to MID later if version <=5.6
VERSION_INFO=$(${BIN} --version | grep -oe '[58]\.[01567]' | head -n1)
if [ "${VERSION_INFO}" == "5.1" -o "${VERSION_INFO}" == "5.5" -o "${VERSION_INFO}" == "5.6" ]; then
  if [ "${MID}" == "" ]; then
    echo "Assert: Version was detected as ${VERSION_INFO}, yet ./scripts/mysql_install_db nor ./bin/mysql_install_db is present!"
    exit 1
  fi
  INIT_TOOL="${MID}"
  INIT_OPT="--no-defaults --force ${MYINIT}"
  START_OPT="--core"
elif [ "${VERSION_INFO}" != "5.7" -a "${VERSION_INFO}" != "8.0" ]; then
  echo "WARNING: mysqld (${BIN}) version detection failed. This is likely caused by using this script with a non-supported distribution or version of mysqld. Please expand this script to handle (which shoud be easy to do). Even so, the script will now try and continue as-is, but this may fail."
fi

if [ "${VERSION_INFO}" == "5.7" ]; then
  if [ "${KEYRING_COMPONENT}" == "1" ]; then
    echoit "Keyring component is un-supported on PS-5.7 and PXC-5.7"
    exit 1
  fi
  KEYRING_COMPONENT=0
fi

if [ ${ENCRYPTION_RUN} -eq 1 ]; then
  if [ ${KEYRING_VAULT} -eq 0 -a ${KEYRING_FILE} -eq 0 -a ${KEYRING_COMPONENT} -eq 0 ]; then
    echoit "Enable atleast one encryption type (keyring_vault or keyring_file) if ENCRYPTION_RUN=1"
    exit 1
  fi
fi

# Start vault server for pstress encryption run
if [ ${KEYRING_VAULT} -eq 1 ];then
  echoit "Setting up vault server"
  mkdir $WORKDIR/vault
  rm -rf $WORKDIR/vault/*
  killall vault > /dev/null 2>&1
  if [[ $PXC -eq 1 ]];then
    ${SCRIPT_PWD}/vault_test_setup.sh --workdir=$WORKDIR/vault --setup-pxc-mount-points --use-ssl > /dev/null 2>&1
  else
    ${SCRIPT_PWD}/vault_test_setup.sh --workdir=$WORKDIR/vault --use-ssl > /dev/null 2>&1
    VAULT_PARAM="--early-plugin-load=keyring_vault.so --keyring_vault_config=$WORKDIR/vault/keyring_vault_ps.cnf"
  fi
elif [ ${KEYRING_FILE} -eq 1 ]; then
  KEYRING_PARAM="--early-plugin-load=keyring_file.so --keyring_file_data=keyring"
fi

if [ ${KEYRING_COMPONENT} -eq 1 ]; then
  create_global_manifest
  create_global_config
fi

echoit "Making a copy of the mysqld binary into ${WORKDIR}/mysqld (handy for coredump analysis and manually starting server)..."
mkdir ${WORKDIR}/mysqld
cp -R ${BASEDIR}/bin ${WORKDIR}/mysqld/
echoit "Making a copy of the library files required for starting server from incident directory"
cp -R ${BASEDIR}/lib ${WORKDIR}/mysqld/
echoit "Making a copy of the conf file $CONFIGURATION_FILE (useful later during repeating the crashes)..."
cp ${SCRIPT_PWD}/$CONFIGURATION_FILE ${WORKDIR}/
echoit "Making a copy of the seed file..."
echo "${SEED}" > ${WORKDIR}/seed

if [[ ${PXC} -eq 0 && ${GRP_RPL} -eq 0 ]]; then
  echoit "Generating datadir template (using mysql_install_db or mysqld --init)..."
  ${INIT_TOOL} ${INIT_OPT} --basedir=${BASEDIR} --datadir=${WORKDIR}/data.template > ${WORKDIR}/log/mysql_install_db.txt 2>&1
elif [[ ${PXC} -eq 1 || ${GRP_RPL} -eq 1 ]]; then
  if [ ${PXC} -eq 1 ]; then
    echoit "Ensuring PXC templates created for pstress run.."
    pxc_startup startup
    sleep 5
    if ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node1.template/node1_socket.sock  ping > /dev/null 2>&1; then
      echoit "PXC node1.template started" ;
    else
      echoit "Assert: PXC data template creation failed.."
      exit 1
    fi
    if ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node2.template/node2_socket.sock  ping > /dev/null 2>&1; then
      echoit "PXC node2.template started" ;
    else
      echoit "Assert: PXC data template creation failed.."
      exit 1
    fi
    if ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node3.template/node3_socket.sock  ping > /dev/null 2>&1; then
      echoit "PXC node3.template started" ;
    else
      echoit "Assert: PXC data template creation failed.."
      exit 1
    fi
    echoit "Created PXC data templates for pstress run.."
    ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node3.template/node3_socket.sock  shutdown > /dev/null 2>&1
    ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node2.template/node2_socket.sock  shutdown > /dev/null 2>&1
    ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node1.template/node1_socket.sock  shutdown > /dev/null 2>&1
  elif [[ ${GRP_RPL} -eq 1 ]]; then
    echoit "Ensuring Group Replication templates created for pstress run.."
    gr_startup startup
    sleep 5
    if ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node1.template/node1_socket.sock  ping > /dev/null 2>&1; then
      echoit "Group Replication node1.template started"
    else
      echoit "Assert: GR data template creation failed.."
      exit 1
    fi
    if ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node2.template/node2_socket.sock  ping > /dev/null 2>&1; then
      echoit "Group Replication node2.template started"
    else
      echoit "Assert: GR data template creation failed.."
      exit 1
    fi
    if ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node3.template/node3_socket.sock  ping > /dev/null 2>&1; then
      echoit "Group Replication node3.template started"
    else
      echoit "Assert: GR data template creation failed.."
      exit 1
    fi
    echoit "Created Group Replication data templates for pstress run.."
    ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node3.template/node3_socket.sock  shutdown > /dev/null 2>&1
    ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node2.template/node2_socket.sock  shutdown > /dev/null 2>&1
    ${BASEDIR}/bin/mysqladmin -uroot -S${WORKDIR}/node1.template/node1_socket.sock  shutdown > /dev/null 2>&1
  fi
fi

# Start actual pstress testing
echoit "Starting pstress testing iterations..."
COUNT=0
for X in $(seq 1 ${TRIALS}); do
  pstress_test
  COUNT=$[ $COUNT + 1 ]
done
# All done, wrap up pstress run
echoit "pstress finished requested number of trials (${TRIALS})... Terminating..."
if [[ ${PXC} -eq 1 || ${GRP_RPL} -eq 1 ]]; then
  echoit "Cleaning up any leftover processes..."
  KILL_PIDS=`ps -ef | grep "$RANDOMD" | grep -v "grep" | awk '{print $2}' | tr '\n' ' '`
  if [ "${KILL_PIDS}" != "" ]; then
    echoit "Terminating the following PID's: ${KILL_PIDS}"
    kill -9 ${KILL_PIDS} >/dev/null 2>&1
  fi
else
  (ps -ef | grep 'node[0-9]_socket' | grep ${RUNDIR} | grep -v grep | awk '{print $2}' | xargs kill -9 >/dev/null 2>&1 || true)
  sleep 2; sync
fi
echoit "Done. Attempting to cleanup the pstress rundir ${RUNDIR}..."
rm -Rf ${RUNDIR}
echoit "The results of this run can be found in the workdir ${WORKDIR}..."
echoit "Done. Exiting $0 with exit code 0..."
exit 0
