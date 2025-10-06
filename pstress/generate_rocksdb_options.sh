#####################################################################################
#!/bin/bash                                                                         #
# Created by Mohit Joshi, Percona LLC                                               #
# Created on: 06-SEP-2021                                                           #
#                                                                                   #
# Purpose: This script auto-generates the rocksdb_options.txt file                  #
# Usage: ./generate_rocksdb_options.sh <path to mysql install/base dir>             #
# References: https://www.percona.com/doc/percona-server/8.0/myrocks/variables.html #
#####################################################################################

# Script expected argument
BASEDIR=${BASEDIR:-$1}

# User variables
OUTPUT_PATH=${OUTPUT_PATH:-/tmp/gen_rocksdb_options}
OUTPUT_PATH=$(realpath "$OUTPUT_PATH")
OUTPUT_FILE=${OUTPUT_PATH}/rocksdb_options_80.txt

# Internal variables, do not change
TEMP_FILE=${OUTPUT_PATH}/rocksdb_options_80.tmp
PORT=22000
SOCKET=${OUTPUT_PATH}/mysql_22000.sock
DATADIR=$BASEDIR/data
MYSQLD_START_TIMEOUT=30

MYSQLD_BIN=$BASEDIR/bin/mysqld
# Check if mysqld binaries exists
if [ ! -x ${MYSQLD_BIN} ]; then MYSQLD_BIN=$BASEDIR/bin/mysqld-debug; fi
if [ ! -x ${MYSQLD_BIN} ]; then
    echo "Error: BASEDIR=$BASEDIR; no $BASEDIR/bin/mysqld or $MYSQLD_BIN found!"
    exit 1
fi

# Display function
echoit(){
  echo "[$(date +'%T')] $1"
  echo "[$(date +'%T')] $1" >> ${OUTPUT_PATH}/generate_rocksdb_options.log
}

mkdir -p ${OUTPUT_PATH}
echoit "Starting MySQL server"
$MYSQLD_BIN --no-defaults --datadir=$DATADIR --initialize-insecure > ${OUTPUT_PATH}/mysql_install_db.txt 2>&1
$MYSQLD_BIN --no-defaults --datadir=$DATADIR --port=$PORT --socket=$SOCKET 2>&1 > ${OUTPUT_PATH}/master.err 2>&1 &
MPID=$!

echoit "Waiting for the server to fully start..."
for X in $(seq 0 ${MYSQLD_START_TIMEOUT}); do
  sleep 1
  if [ "$MPID" == "" ]; then echoit "Assert! ${MPID} empty. Terminating!"; exit 1; fi
  if $BASEDIR/bin/mysqladmin -uroot -S${SOCKET} ping > /dev/null 2>&1; then
    echoit "MySQL server started successfully"
    $BASEDIR/bin/ps-admin --enable-rocksdb -uroot -S${OUTPUT_PATH}/mysql_22000.sock 2>&1 > /dev/null
    echoit "RocksDB loaded successfully"
    break
  fi
done

# Extract all options, their default values into temp file
$BASEDIR/bin/mysql --no-defaults -S$SOCKET -uroot -e "
  SELECT
    vi.VARIABLE_NAME,
    gv.VARIABLE_VALUE AS current_value,
    vi.MIN_VALUE,
    vi.MAX_VALUE
  FROM performance_schema.variables_info vi
  JOIN performance_schema.global_variables gv
    ON vi.VARIABLE_NAME = gv.VARIABLE_NAME
  WHERE vi.VARIABLE_NAME LIKE 'rocksdb_%';
" > $TEMP_FILE

# Remove the first line from the temp file
# Variable_name, Variable_value (we do not need the column name displayed in temp file)
sed -i 1d $TEMP_FILE

# List of rocksdb variables which must not be changed.
EXCLUDED_LIST=(
  rocksdb_enable_pipelined_write    # with rocksdb_two_write_queues=OFF: MemTableInserter::MarkCommit(const rocksdb::Slice&): Assertion `!write_after_commit_ || log_number_ref_ > 0' failed.
  rocksdb_write_policy              # Status: Not implemented: WriteCommitted txn tag when write_before_prepare_ is enabled (in WriteUnprepared mode). If it is not due to corruption, the WAL must be emptied before changing the WritePolicy.
                                    # Status: Not implemented: WriteCommitted txn tag when write_after_commit_ is disabled (in WritePrepared/WriteUnprepared mode). If it is not due to corruption, the WAL must be emptied before changing the WritePolicy.'
  rocksdb_error_if_exists           # Status: Invalid argument: ./.rocksdb: exists (error_if_exists is true)
  rocksdb_protection_bytes_per_key  # WriteBatch::WriteBatch(size_t, size_t, size_t, size_t): Assertion `protection_bytes_per_key == 0 || protection_bytes_per_key == 8' failed.
  rocksdb_no_block_cache            # Status Code: 4, Status: Invalid argument: Enable cache_index_and_filter_blocks, , but block cache is disabled'
  rocksdb_create_if_missing         # Status Code: 4, Status: Invalid argument: ./.rocksdb/CURRENT: does not exist (create_if_missing is false)
  rocksdb_write_batch_max_bytes     # ERROR HY000: Status error 10 received from RocksDB: Operation aborted: Memory limit reached.
  rocksdb_fs_uri                    # Custom filesystem URI
  rocksdb_cancel_manual_compactions # just a trigger
  rocksdb_disable_instant_ddl       # deprecated
  rocksdb_io_error_action
  rocksdb_corrupt_data_action
  rocksdb_invalid_create_option_action
  rocksdb_alter_table_comment_inplace
  rocksdb_column_default_value_as_expression
  rocksdb_compact_cf rocksdb_info_log_level rocksdb_update_cf_options rocksdb_delete_cf rocksdb_override_cf_options rocksdb_create_checkpoint rocksdb_create_temporary_checkpoint rocksdb_fault_injection_options rocksdb_read_free_rpl_tables rocksdb_persistent_cache_path rocksdb_strict_collation_exceptions rocksdb_tmpdir rocksdb_trace_block_cache_access rocksdb_trace_queries rocksdb_wal_dir rocksdb_wsenv_path rocksdb_datadir )

# Create an output file which contains all the options/values
rm -rf $OUTPUT_FILE
touch $OUTPUT_FILE

fix_min_value() {
    local max32=4294967296  # 2^32
    local half32=2147483648 # 2^31

    if (( MIN_VALUE >= half32 )); then
        MIN_VALUE=$(( MIN_VALUE - max32 ))
    fi
}

check_range() {
    local value="$1"

    # bc will return 1 if condition true, 0 if false
    if (( $(echo "$value > $MIN_VALUE && $value < $MAX_VALUE && $value != $DEFAULT_VALUE" | bc) )); then
        echo "$OPTION=$value" >> "$OUTPUT_FILE"
    fi
}

while read line; do
  COMMAND=$(echo $line | awk '{print $1}')
  OPTION="--loose-$COMMAND"
  VALUE="$(echo $line | awk '{print $2}')"
  MIN_VALUE="$(echo $line | awk '{print $3}')"
  MAX_VALUE="$(echo $line | awk '{print $4}')"
  if [ "$VALUE" == "" ]; then
    echoit "Working on option '$OPTION' which has no default value..."
  else
    echoit "Working on option '$OPTION' with default value=$VALUE MIN_VALUE=$MIN_VALUE MAX_VALUE=$MAX_VALUE"
  fi
  if [[ " ${EXCLUDED_LIST[@]} " =~ " ${COMMAND} " ]]; then
    echoit "Option '$OPTION' is logically excluded from being handled by this script..."
  elif [[ "$COMMAND" == "rocksdb_rate_limiter_bytes_per_sec" ]]; then
    echoit " > Adding possible values for option '${OPTION}' to the final list..."
    for value in 0 4k 256k 16m 1g 64g; do
        echo "$OPTION=$value" >> "$OUTPUT_FILE"
    done
  elif [[ "$COMMAND" == "rocksdb_merge_buf_size" ]]; then
    echoit " > Adding possible values for option '${OPTION}' to the final list..."
    for value in 4k 16k 64k 256k 1m 4m 16m 64m 256m 1g; do
        echo "$OPTION=$value" >> "$OUTPUT_FILE"
    done
  elif [[ "$COMMAND" == "rocksdb_manifest_preallocation_size" ]]; then
    echoit " > Adding possible values for option '${OPTION}' to the final list..."
    for value in 0 4k 32k 256k 2m 16m 128m 1g; do
        echo "$OPTION=$value" >> "$OUTPUT_FILE"
    done
  elif [[ "$COMMAND" == "rocksdb_max_total_wal_size" ]]; then
    echoit " > Adding possible values for option '${OPTION}' to the final list..."
    for value in 0 4k 32k 256k 2m 16m 128m 1g 8g 64g; do
        echo "$OPTION=$value" >> "$OUTPUT_FILE"
    done
  elif [[ "$COMMAND" == "rocksdb_persistent_cache_size_mb" ]]; then
    echoit " > Adding possible values for option '${OPTION}' to the final list..."
    for value in 100 254 1023 2047 4096; do
        echo "$OPTION=$value --loose-rocksdb_persistent_cache_path=." >> "$OUTPUT_FILE"
    done
  elif [[ "$COMMAND" == "rocksdb_access_hint_on_compaction_start" ]]; then
    echoit " > Adding possible values NONE, NORMAL, SEQUENTIAL, WILLNEED for option '${OPTION}' to the final list..."
    echo "$OPTION=NONE" >> $OUTPUT_FILE
    echo "$OPTION=NORMAL" >> $OUTPUT_FILE
    echo "$OPTION=SEQUENTIAL" >> $OUTPUT_FILE
    echo "$OPTION=WILLNEED" >> $OUTPUT_FILE
  elif [[  "$COMMAND" == "rocksdb_index_type" ]]; then
    echoit " > Adding possible values kBinarySearch, kHashSearch for option '${OPTION}' to the final list..."
    echo "$OPTION=kBinarySearch" >> $OUTPUT_FILE
    echo "$OPTION=kHashSearch --loose-rocksdb_default_cf_options=prefix_extractor=capped:12" >> $OUTPUT_FILE
  elif [[ "$COMMAND" == "rocksdb_manual_compaction_bottommost_level" ]]; then
    echoit " > Adding possible values kSkip, kIfHaveCompactionFilter, kForce, kForceOptimized for option '${OPTION}' to the final list..."
    echo "$OPTION=kSkip" >> $OUTPUT_FILE
    echo "$OPTION=kIfHaveCompactionFilter" >> $OUTPUT_FILE
    echo "$OPTION=kForce" >> $OUTPUT_FILE
    echo "$OPTION=kForceOptimized" >> $OUTPUT_FILE
  elif [[ "$COMMAND" == "rocksdb_cache_high_pri_pool_ratio" ]]; then
    echoit " > Adding possible values 0, 0.25, 0.5, 0.75, 1.0 for option '${OPTION}' to the final list..."
    echo "$OPTION=0.0" >> $OUTPUT_FILE
    echo "$OPTION=0.25" >> $OUTPUT_FILE
    echo "$OPTION=0.50" >> $OUTPUT_FILE
    echo "$OPTION=0.75" >> $OUTPUT_FILE
    echo "$OPTION=1.0" >> $OUTPUT_FILE
  elif [[ "$COMMAND" == "rocksdb_default_cf_options" ]]; then
    echoit " > Adding possible values for option '${OPTION}' to the final list..."
    echo "$OPTION=write_buffer_size=256m;max_write_buffer_number=1" >> $OUTPUT_FILE
    echo "$OPTION=write_buffer_size=32m;max_write_buffer_number=4" >> $OUTPUT_FILE
    echo "$OPTION=write_buffer_size=128m;max_write_buffer_number=3" >> $OUTPUT_FILE
    echo "$OPTION=target_file_size_base=32m" >> $OUTPUT_FILE
    echo "$OPTION=target_file_size_base=128m" >> $OUTPUT_FILE
    echo "$OPTION=target_file_size_base=256m" >> $OUTPUT_FILE
    echo "$OPTION=max_bytes_for_level_base=32m" >> $OUTPUT_FILE
    echo "$OPTION=max_bytes_for_level_base=128m" >> $OUTPUT_FILE
    echo "$OPTION=max_bytes_for_level_base=256m" >> $OUTPUT_FILE
    echo "$OPTION=max_bytes_for_level_base=512m" >> $OUTPUT_FILE
    echo "$OPTION=max_write_buffer_number=4" >> $OUTPUT_FILE
    echo "$OPTION=max_write_buffer_number=3" >> $OUTPUT_FILE
    echo "$OPTION=max_write_buffer_number=2" >> $OUTPUT_FILE
    echo "$OPTION=max_write_buffer_number=1" >> $OUTPUT_FILE
    echo "$OPTION=level0_file_num_compaction_trigger=4" >> $OUTPUT_FILE
    echo "$OPTION=level0_file_num_compaction_trigger=3" >> $OUTPUT_FILE
    echo "$OPTION=level0_file_num_compaction_trigger=2" >> $OUTPUT_FILE
    echo "$OPTION=level0_slowdown_writes_trigger=20;level0_stop_writes_trigger=10" >> $OUTPUT_FILE
    echo "$OPTION=level0_slowdown_writes_trigger=10;level0_stop_writes_trigger=20" >> $OUTPUT_FILE
    echo "$OPTION=level0_slowdown_writes_trigger=1;level0_stop_writes_trigger=3" >> $OUTPUT_FILE
    echo "$OPTION=compression=kLZ4Compression;bottommost_compression=kLZ4Compression;compression_opts=-14:4:0" >> $OUTPUT_FILE
    echo "$OPTION=compression=kZSTD;bottommost_compression=kZSTD;compression_opts=-14:4:0" >> $OUTPUT_FILE
    echo "$OPTION=compression=kZlibCompression;bottommost_compression=kZlibCompression;compression_opts=-14:4:0" >> $OUTPUT_FILE
    echo "$OPTION=level_compaction_dynamic_level_bytes=true;optimize_filters_for_hits=true;memtable_prefix_bloom_size_ratio=0.05" >> $OUTPUT_FILE
    echo "$OPTION=level_compaction_dynamic_level_bytes=false;optimize_filters_for_hits=true;memtable_prefix_bloom_size_ratio=0.01" >> $OUTPUT_FILE
    echo "$OPTION=level_compaction_dynamic_level_bytes=true;optimize_filters_for_hits=false;memtable_prefix_bloom_size_ratio=0.10" >> $OUTPUT_FILE
    echo "$OPTION=compaction_pri=kMinOverlappingRatio;prefix_extractor=capped:12" >> $OUTPUT_FILE
    echo "$OPTION=block_based_table_factory={cache_index_and_filter_blocks=1;filter_policy=bloomfilter:10:false;whole_key_filtering=0}" >> $OUTPUT_FILE
  elif [[ "$OPTION" == "--loose-rocksdb_read_free_rpl" ]]; then
    echoit " > Adding possible values OFF, PK_SK, PK_ONLY for option '${OPTION}' to the final list..."
    echo "$OPTION=OFF" >> $OUTPUT_FILE
    echo "$OPTION=PK_SK" >> $OUTPUT_FILE
    echo "$OPTION=PK_ONLY" >> $OUTPUT_FILE
  elif [[ "$OPTION" == "--loose-rocksdb_file_checksums" ]]; then
    echoit " > Adding possible values CHECKSUMS_OFF, CHECKSUMS_WRITE_ONLY, CHECKSUMS_WRITE_AND_VERIFY for option '${OPTION}' to the final list..."
    echo "$OPTION=CHECKSUMS_OFF" >> $OUTPUT_FILE
    echo "$OPTION=CHECKSUMS_WRITE_ONLY" >> $OUTPUT_FILE
    echo "$OPTION=CHECKSUMS_WRITE_AND_VERIFY" >> $OUTPUT_FILE
  elif [ "$OPTION" == "--loose-rocksdb_validate_tables" -o "$OPTION" == "--loose-rocksdb_flush_log_at_trx_commit" ]; then
    echoit " > Adding possible values 0, 1, 2 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=2" >> $OUTPUT_FILE
  elif [[ "OPTION" == "--loose-rocksdb_checksums_pct" ]]; then
    echoit " > Adding possible values 0, 1, 10, 50, 99, 100 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=10" >> $OUTPUT_FILE
    echo "$OPTION=50" >> $OUTPUT_FILE
    echo "$OPTION=99" >> $OUTPUT_FILE
    echo "$OPTION=100" >> $OUTPUT_FILE
  elif [[ "$OPTION" == "--loose-rocksdb_block_cache_size" ]]; then
    echoit " > Adding possible values 1024, 2048, 4048, 536870912, 9223372036854775807 for option '${OPTION}' to the final list..."
    echo "$OPTION=1024" >> $OUTPUT_FILE
    echo "$OPTION=2048" >> $OUTPUT_FILE
    echo "$OPTION=4048" >> $OUTPUT_FILE
    echo "$OPTION=536870912" >> $OUTPUT_FILE
    echo "$OPTION=9223372036854775807" >> $OUTPUT_FILE
  elif [[ "$OPTION" == "--loose-rocksdb-base-background-compactions" ]]; then
    echoit " > Adding possible values -1, 0, 1, 5, 10, 30, 63, 64 for option '${OPTION}' to the final list..."
    echo "$OPTION=-1" >> $OUTPUT_FILE
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=5" >> $OUTPUT_FILE
    echo "$OPTION=10" >> $OUTPUT_FILE
    echo "$OPTION=30" >> $OUTPUT_FILE
    echo "$OPTION=63" >> $OUTPUT_FILE
    echo "$OPTION=64" >> $OUTPUT_FILE
  elif [[ "$OPTION" == "--loose-rocksdb_compaction_sequential_deletes" ]]; then
    echoit " > Adding possible values 0, 1, 100, 5000, 1999999, 2000000 for option '${OPTION}' to the final list..."
    echo "$OPTION=0" >> $OUTPUT_FILE
    echo "$OPTION=1" >> $OUTPUT_FILE
    echo "$OPTION=100" >> $OUTPUT_FILE
    echo "$OPTION=5000" >> $OUTPUT_FILE
    echo "$OPTION=1999999" >> $OUTPUT_FILE
    echo "$OPTION=2000000" >> $OUTPUT_FILE
  elif [[ "$COMMAND" == "rocksdb_write_disable_wal" || "$COMMAND" == "rocksdb_allow_mmap_writes" ]]; then
    if [ "$VALUE" == "OFF" ]; then
      echo "$OPTION=ON --loose-rocksdb_flush_log_at_trx_commit=0" >> $OUTPUT_FILE
    else
      echo "$OPTION=OFF --loose-rocksdb_flush_log_at_trx_commit=0" >> $OUTPUT_FILE
    fi
  elif [ "$VALUE" == "ON" -o "$VALUE" == "OFF" ]; then
    echoit " > Adding possible values ON, OFF for option '${OPTION}' to the final list..."
    if [ "$VALUE" == "OFF" ]; then
      echo "$OPTION=ON" >> $OUTPUT_FILE
    else
      echo "$OPTION=OFF" >> $OUTPUT_FILE
    fi
  elif [[ "$VALUE" =~ ^-?[0-9]+$ ]]; then
    echoit "  > Adding int values ( $VALUE, 0, 1, -1, 2, 12, 24, 254, 1023, 2047, 2147483647, 1125899906842624, 18446744073709551615 ) for option '${OPTION}' to the final list..."
    fix_min_value
    echo "$OPTION=$MIN_VALUE" >> $OUTPUT_FILE
    echo "$OPTION=$MAX_VALUE" >> $OUTPUT_FILE
    # uncomment to add default values (if different from MIN_VALUE and MAX_VALUE):
    # DEFAULT_VALUE=$MIN_VALUE; check_range $VALUE
    DEFAULT_VALUE=$VALUE
    check_range 0
    check_range 1
    check_range -1
    check_range 2
    check_range 12
    check_range 24
    check_range 254
    check_range 1023
    check_range 2047
    check_range 2147483647
    check_range 1125899906842624
    check_range 18446744073709551615
  elif [ "${VALUE}" == "" ]; then
    echoit "  > Assert: Option '${OPTION}' is blank by default and not programmed into the script yet, please cover this in the script..."
    exit 1
  else
    echoit "  > ${OPTION} IS NOT COVERED YET, PLEASE ADD!!!"
    exit 1
  fi
done < $TEMP_FILE

#rm -rf $TEMP_FILE
echo "Done! Output file: ${OUTPUT_FILE}"
killall -9 $MYSQLD_BIN
