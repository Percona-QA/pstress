#!/bin/bash
# Created by Mohit Joshi

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

# Internal flags, please do not change
CONFIG_FLAG=0;INCIDENT_FLAG=0;STEP_FLAG=0;REPEAT_FLAG=0;NORMAL_MODE=0;
RESUME_MODE=0;REPEAT_MODE=0

while true; do
    case "$1" in
    --help)
        Help
        exit
        ;;
    --config)
        shift
        CONFIG=$1
        CONFIG_FLAG=1
        echo "Config file is: $CONFIG"
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

# Start normal pstress runs
pstress_run() {
echo "Todo: Start pstress normally"
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
echo "Todo: start the server"
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

if [ $NORMAL_MODE -eq 1 ]; then
  echo "Starting pstress iterations"
  for X in $(seq 1 $TRIALS); do
    pstress_run
  done
elif [ $RESUME_MODE -eq 1 ]; then
  echo "Resuming pstress iterations after step:$TRIAL"
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

