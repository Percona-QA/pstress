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

while true; do
    case "$1" in
    --help)
        Help
        exit
        ;;
    --config)  
        shift
        CONFIG=$1
        echo "Config file is: $CONFIG"
        ;;
    --incident)  
        shift
        INCIDENT_DIRECTORY=$1
        echo "Incident Directory is: $INCIDENT_DIRECTORY"
        ;;
    --repeat)  
        shift
        REPEAT=$1
        echo "Repeat is: $REPEAT"
        ;;
    --step)
        shift
        STEP=$1
        echo "Step is: $STEP"
        ;;
    --)
        shift
        break
        ;;
    esac
    shift
done

echo "Welcome to the new world of pstress-driver";
