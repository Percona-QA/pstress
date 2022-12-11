#!/bin/bash
# Created by Roel Van de Paar, Percona LLC
# Modified by Mohit Joshi, Percona LLC

# This script quickly and intelligently generates all available mysqld --option combinations (i.e. including values)

# User variables
OUTPUT_FILE=/tmp/mysqld_options_ms_80.txt

# Internal variables, do not change
TEMP_FILE=/tmp/mysqld_options.tmp

if [ ! -r ./bin/mysqld ]; then
  if [ ! -r ./mysqld ]; then
    echo "This script quickly and intelligently generates all available mysqld --option combinations (i.e. including values)"
    echo "Error: no ./bin/mysqld or ./mysqld found!"
    exit 1
  else
    cd ..
  fi
fi

IS_PXC=0
if ./bin/mysqld --version | grep -q 'Percona XtraDB Cluster' 2>/dev/null ; then 
  IS_PXC=1
fi

echoit(){
  echo "[$(date +'%T')] $1"
  echo "[$(date +'%T')] $1" >> /tmp/generate_mysqld_options.log
}

# Extract all options, their default values, and do some initial cleaning
./bin/mysqld --no-defaults --help --verbose | grep -v "slave" 2>/dev/null | sed '0,/Value (after reading options)/d' | egrep -v "To see what values.*is using|mysqladmin.*instead of|^[ \t]*$|\-\-\-" > ${TEMP_FILE}

# mysqld options excluded from list
# RV/HM 18.07.2017 Temporarily added to EXCLUDED_LIST: --binlog-group-commit-sync-delay due to hang issues seen in 5.7 with startup like --no-defaults --plugin-load=tokudb=ha_tokudb.so --tokudb-check-jemalloc=0 --init-file=/home/hrvoje/percona-qa/plugins_57.sql --binlog-group-commit-sync-delay=2047
# --transaction-write-set-extraction is deprecated so it needs to be removed in next release
EXCLUDED_LIST=( --admin-address --admin-port --admin-ssl-ca --admin-ssl-capath --admin-ssl-cert --admin-ssl-cipher --admin-ssl-crl --admin-ssl-crlpath --admin-ssl-key --admin-tls-ciphersuites --admin-tls-version --basedir --bind-address --binlog-group-commit-sync-delay --buffered-error-log-filename --caching-sha2-password-private-key-path --caching-sha2-password-public-key-path --character-sets-dir --chroot --coredumper --datadir --debug --default-time-zone --disabled-storage-engines --ft-stopword-file --general-log --general-log-file --init-connect --init-file --initialize --initialize-insecure --init-replica --innodb-buffer-pool-filename --innodb-data-file-path --innodb-data-home-dir --innodb-directories --innodb-doublewrite-dir --innodb-ft-aux-table --innodb-ft-server-stopword-table --innodb-ft-user-stopword-table --innodb-interpreter --innodb-interpreter-output --innodb-log-group-home-dir --innodb-page-size --innodb-parallel-doublewrite-path --innodb-tmpdir --innodb-redo-log-archive-dirs --innodb-temp-data-file-path --innodb-temp-tablespaces-dir --innodb-undo-directory --innodb-undo-tablespaces --internal-tmp-mem-storage-engine --keyring-migration-destination --keyring-migration-host --keyring-migration-password --keyring-migration-port --keyring-migration-socket --keyring-migration-source --keyring-migration-user --language --lc-messages-dir --log-bin-index --log-error --log-error-services --log-error-suppression-list --log-error-verbosity --log-isam --log-query-errors --log-slow-filter --log-slow-verbosity --log-tc --log-timestamps --master-info-file --myisam-block-size --myisam-data-pointer-size --myisam-max-sort-file-size --myisam-mmap-size --myisam-recover-options --myisam-repair-threads --myisam-sort-buffer-size --myisam-stats-method --myisam-use-mmap --mysqlx-bind-address --mysqlx-socket --mysqlx-ssl-ca --mysqlx-ssl-capath --mysqlx-ssl-cert --mysqlx-ssl-cipher --mysqlx-ssl-crl --mysqlx-ssl-crlpath --mysqlx-ssl-key --persist-only-admin-x509-subject --pid-file --plugin-dir --plugin-load --port --proxy-protocol-networks --relay-log-index --relay-log-info-file --replica-load-tmpdir --replica-skip-errors --report-host --report-password --report-port --report-user --secure-file-priv --secure-log-path --sha256-password-private-key-path --sha256-password-public-key-path --slow-query-log-always-write-time --slow-query-log-file --slow-query-log-use-global-control  --socket --ssl-ca --ssl-capath --ssl-cert --ssl-cipher --ssl-crl --ssl-crlpath --ssl-key --terminology-use-previous --tls-ciphersuites --tmpdir --transaction-write-set-extraction --utility-user --utility-user-dynamic-privileges --utility-user-password --utility-user-privileges --utility-user-schema-access --version-suffix --authentication-policy --debug-sensitive-session-string)
# Create a file (${OUTPUT_FILE}) with all options/values intelligently handled and included
rm -Rf ${OUTPUT_FILE}
touch ${OUTPUT_FILE}

while read line; do 
  OPTION="--$(echo ${line} | awk '{print $1}')"
  VALUE="$(echo ${line} | awk '{print $2}' | sed 's|^[ \t]*||;s|[ \t]*$||')"
  if [ "${VALUE}" == "(No" ]; then
    echoit "Working on option '${OPTION}' which has no default value..."
  else
    echoit "Working on option '${OPTION}' with default value '${VALUE}'..."
  fi
  # Process options & values
  if [[ " ${EXCLUDED_LIST[@]} " =~ " ${OPTION} " ]]; then 
    echoit "  > Option '${OPTION}' is logically excluded from being handled by this script..."
  elif [ "${OPTION}" == "--enforce-storage-engine" ]; then
    echoit "  > Adding possible values InnoDB for option '${OPTION}' to the final list..."
    echo "${OPTION}=InnoDB" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--binlog-error-action" ]; then
    echoit "  > Adding possible values IGNORE_ERROR, ABORT_SERVER for option '${OPTION}' to the final list..."
    echo "${OPTION}=IGNORE_ERROR" >> ${OUTPUT_FILE}
    echo "${OPTION}=ABORT_SERVER" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--enforce-gtid-consistency" ]; then
    echoit "  > Adding possible values OFF, ON, WARN for option '${OPTION}' to the final list..."
    echo "${OPTION}=OFF" >> ${OUTPUT_FILE}
    echo "${OPTION}=ON" >> ${OUTPUT_FILE}
    echo "${OPTION}=WARN" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--mandatory-roles" ]; then
    echoit "  > Adding possible values '','role1@%,role2,role3,role4@localhost','@%','user1@localhost,testuser@%' for option '${OPTION}' to the final list..."
    echo "${OPTION}=''" >> ${OUTPUT_FILE}
    echo "${OPTION}='role1@%,role2,role3,role4@localhost'" >> ${OUTPUT_FILE}
    echo "${OPTION}='@%'" >> ${OUTPUT_FILE}
    echo "${OPTION}='user1@localhost,testuser@%'" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--binlog-format" ]; then
    echoit "  > Adding possible values ROW, STATEMENT, MIXED for option '${OPTION}' to the final list..."
    echo "${OPTION}=ROW" >> ${OUTPUT_FILE}
    echo "${OPTION}=STATEMENT" >> ${OUTPUT_FILE}
    echo "${OPTION}=MIXED" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--binlog-row-image" ]; then
    echoit "  > Adding possible values full, minimal, noblob for option '${OPTION}' to the final list..."
    echo "${OPTION}=full" >> ${OUTPUT_FILE}
    echo "${OPTION}=minimal" >> ${OUTPUT_FILE}
    echo "${OPTION}=noblob" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--binlog-row-value-options" ]; then
    echoit "  > Adding possible values '',PARTIAL_JSON for option '${OPTION}' to the final list..."
    echo "${OPTION}=''" >> ${OUTPUT_FILE}
    echo "${OPTION}=PARTIAL_JSON" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--character-set-filesystem" -o "${OPTION}" == "--character-set-server" ]; then
    echoit "  > Adding possible values for option '${OPTION}' to the final list..."
    echo "${OPTION}=binary" >> ${OUTPUT_FILE}
    echo "${OPTION}=utf8" >> ${OUTPUT_FILE}
    echo "${OPTION}=big5" >> ${OUTPUT_FILE}
    echo "${OPTION}=dec8" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp850" >> ${OUTPUT_FILE}
    echo "${OPTION}=hp8" >> ${OUTPUT_FILE}
    echo "${OPTION}=koi8r" >> ${OUTPUT_FILE}
    echo "${OPTION}=latin1" >> ${OUTPUT_FILE}
    echo "${OPTION}=latin2" >> ${OUTPUT_FILE}
    echo "${OPTION}=swe7" >> ${OUTPUT_FILE}
    echo "${OPTION}=ascii" >> ${OUTPUT_FILE}
    echo "${OPTION}=ujis" >> ${OUTPUT_FILE}
    echo "${OPTION}=sjis" >> ${OUTPUT_FILE}
    echo "${OPTION}=hebrew" >> ${OUTPUT_FILE}
    echo "${OPTION}=tis620" >> ${OUTPUT_FILE}
    echo "${OPTION}=euckr" >> ${OUTPUT_FILE}
    echo "${OPTION}=koi8u" >> ${OUTPUT_FILE}
    echo "${OPTION}=gb2312" >> ${OUTPUT_FILE}
    echo "${OPTION}=greek" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp1250" >> ${OUTPUT_FILE}
    echo "${OPTION}=gbk" >> ${OUTPUT_FILE}
    echo "${OPTION}=latin5" >> ${OUTPUT_FILE}
    echo "${OPTION}=armscii8" >> ${OUTPUT_FILE}
    echo "${OPTION}=ucs2" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp866" >> ${OUTPUT_FILE}
    echo "${OPTION}=keybcs2" >> ${OUTPUT_FILE}
    echo "${OPTION}=macce" >> ${OUTPUT_FILE}
    echo "${OPTION}=macroman" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp852" >> ${OUTPUT_FILE}
    echo "${OPTION}=latin7" >> ${OUTPUT_FILE}
    echo "${OPTION}=utf8mb4" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp1251" >> ${OUTPUT_FILE}
    echo "${OPTION}=utf16" >> ${OUTPUT_FILE}
    echo "${OPTION}=utf16le" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp1256" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp1257" >> ${OUTPUT_FILE}
    echo "${OPTION}=utf32" >> ${OUTPUT_FILE}
    echo "${OPTION}=geostd8" >> ${OUTPUT_FILE}
    echo "${OPTION}=cp932" >> ${OUTPUT_FILE}
    echo "${OPTION}=eucjpms" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--collation-server" ]; then
    echoit "  > Adding possible value utf8mb4_0900_ai_ci for option '${OPTION}' to the final list..."
    echo "${OPTION}=utf8mb4_0900_ai_ci" >> ${OUTPUT_FILE}	  
  elif [ "${OPTION}" == "--completion-type" ]; then
    echoit "  > Adding possible values 0, 1, 2 for option '${OPTION}' to the final list..."
    echo "${OPTION}=0" >> ${OUTPUT_FILE}
    echo "${OPTION}=1" >> ${OUTPUT_FILE}
    echo "${OPTION}=2" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--concurrent-insert" ]; then
    echoit "  > Adding possible values 0, 1, 2 for option '${OPTION}' to the final list..."
    echo "${OPTION}=0" >> ${OUTPUT_FILE}
    echo "${OPTION}=1" >> ${OUTPUT_FILE}
    echo "${OPTION}=2" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-log-files-in-group" ]; then
    echoit "  > Adding possible values 0,1,2,5,10 for option '${OPTION}' to the final list..."
    echo "${OPTION}=0" >> ${OUTPUT_FILE}
    echo "${OPTION}=1" >> ${OUTPUT_FILE}
    echo "${OPTION}=2" >> ${OUTPUT_FILE}
    echo "${OPTION}=5" >> ${OUTPUT_FILE}
    echo "${OPTION}=10" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-checksum-algorithm" ]; then
    echoit "  > Adding possible values innodb, crc32 for option '${OPTION}' to the final list..."
    echo "${OPTION}=innodb" >> ${OUTPUT_FILE}
    echo "${OPTION}=crc32" >> ${OUTPUT_FILE}
    echo "${OPTION}=none" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-cleaner-lsn-age-factor" ]; then
    echoit "  > Adding possible values legacy, high_checkpoint for option '${OPTION}' to the final list..."
    echo "${OPTION}=legacy" >> ${OUTPUT_FILE}
    echo "${OPTION}=high_checkpoint" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-corrupt-table-action" ]; then
    echoit "  > Adding possible values assert, warn, salvage for option '${OPTION}' to the final list..."
    echo "${OPTION}=assert" >> ${OUTPUT_FILE}
    echo "${OPTION}=warn" >> ${OUTPUT_FILE}
    echo "${OPTION}=salvage" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-empty-free-list-algorithm" ]; then
    echoit "  > Adding possible values legacy, backoff for option '${OPTION}' to the final list..."
    echo "${OPTION}=legacy" >> ${OUTPUT_FILE}
    echo "${OPTION}=backoff" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-buffer-pool-evict" ]; then
    echoit "  > Adding possible value uncompressed for option '${OPTION}' to the final list..."
    echo "${OPTION}=uncompressed" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-flush-method" ]; then
    echoit "  > Adding possible values fsync, O_DSYNC, O_DIRECT, O_DIRECT_NO_FSYNC, littlesync, nosync for option '${OPTION}' to the final list..."
    echo "${OPTION}=fsync" >> ${OUTPUT_FILE}
    echo "${OPTION}=O_DSYNC" >> ${OUTPUT_FILE}
    echo "${OPTION}=O_DIRECT" >> ${OUTPUT_FILE}
    echo "${OPTION}=O_DIRECT_NO_FSYNC" >> ${OUTPUT_FILE}
    echo "${OPTION}=littlesync" >> ${OUTPUT_FILE}
    echo "${OPTION}=nosync" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-checksum-algorithm" ]; then
    echoit "  > Adding possible values none, innodb, crc32 for option '${OPTION}' to the final list..."
    echo "${OPTION}=innodb" >> ${OUTPUT_FILE}
    echo "${OPTION}=crc32" >> ${OUTPUT_FILE}
    echo "${OPTION}=none" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-monitor-disable" -o "${OPTION}" == "--innodb-monitor-enable" -o "${OPTION}" == "--innodb-monitor-reset" -o "${OPTION}" == "--innodb-monitor-reset-all" ]; then
    echoit "  > Adding possible values counter, module, pattern and all for option '${OPTION}' to the final list..."
    echo "${OPTION}=counter" >> ${OUTPUT_FILE}
    echo "${OPTION}=module" >> ${OUTPUT_FILE}
    echo "${OPTION}=pattern" >> ${OUTPUT_FILE}
    echo "${OPTION}=all" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-stats-method" ]; then
    echoit "  > Adding possible values nulls_equal, nulls_unequal, nulls_ignored for option '${OPTION}' to the final list..."
    echo "${OPTION}=nulls_equal" >> ${OUTPUT_FILE}
    echo "${OPTION}=nulls_unequal" >> ${OUTPUT_FILE}
    echo "${OPTION}=nulls_ignored" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--log-bin" ]; then
    echoit "  > Adding possible value binlog for option '${OPTION}' to the final list..."
    echo "${OPTION}=binlog" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--log-slow-rate-type" ]; then
    echoit "  > Adding possible values session, query for option '${OPTION}' to the final list..."  
    echo "${OPTION}=session" >> ${OUTPUT_FILE}
    echo "${OPTION}=query" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--replica-type-conversions" ]; then
    echoit "  > Adding possible values ALL_LOSSY, ALL_NON_LOSSY, ALL_SIGNED, ALL_UNSIGNED for option '${OPTION}' to the final list..."
    echo "${OPTION}=ALL_LOSSY" >> ${OUTPUT_FILE}
    echo "${OPTION}=ALL_NON_LOSSY" >> ${OUTPUT_FILE}
    echo "${OPTION}=ALL_SIGNED" >> ${OUTPUT_FILE}
    echo "${OPTION}=ALL_UNSIGNED" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--performance-schema-accounts-size" -o "${OPTION}" == "--performance-schema-hosts-size" -o "${OPTION}" == "--performance-schema-max-thread-instances" -o "${OPTION}" == "--performance-schema-users-size" ]; then  
    echoit "  > Adding possible values 0, 1, 2, 12, 24, 254, 1023, 2047, 1048576 for option '${OPTION}' to the final list..."
    echo "${OPTION}=0" >> ${OUTPUT_FILE}
    echo "${OPTION}=1" >> ${OUTPUT_FILE}
    echo "${OPTION}=2" >> ${OUTPUT_FILE}
    echo "${OPTION}=12" >> ${OUTPUT_FILE}
    echo "${OPTION}=24" >> ${OUTPUT_FILE}
    echo "${OPTION}=254" >> ${OUTPUT_FILE}
    echo "${OPTION}=1023" >> ${OUTPUT_FILE}
    echo "${OPTION}=2047" >> ${OUTPUT_FILE}
    echo "${OPTION}=1048576" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--relay-log" ]; then  
    echoit "  > Adding possible values relay-bin for option '${OPTION}' to the final list..."
    echo "${OPTION}=relay-bin" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--sql-mode" ]; then
    echoit "  > Adding possible values for option '${OPTION}' to the final list..."
    echo "${OPTION}=ALLOW_INVALID_DATES" >> ${OUTPUT_FILE}
    echo "${OPTION}=ANSI_QUOTES" >> ${OUTPUT_FILE}
    echo "${OPTION}=ERROR_FOR_DIVISION_BY_ZERO" >> ${OUTPUT_FILE}
    echo "${OPTION}=HIGH_NOT_PRECEDENCE" >> ${OUTPUT_FILE}
    echo "${OPTION}=IGNORE_SPACE" >> ${OUTPUT_FILE}
    echo "${OPTION}=NO_AUTO_VALUE_ON_ZERO" >> ${OUTPUT_FILE}
    echo "${OPTION}=NO_BACKSLASH_ESCAPES" >> ${OUTPUT_FILE}
    echo "${OPTION}=NO_DIR_IN_CREATE" >> ${OUTPUT_FILE}
    echo "${OPTION}=NO_ENGINE_SUBSTITUTION" >> ${OUTPUT_FILE}
    echo "${OPTION}=NO_UNSIGNED_SUBTRACTION" >> ${OUTPUT_FILE}
    echo "${OPTION}=NO_ZERO_DATE" >> ${OUTPUT_FILE}
    echo "${OPTION}=NO_ZERO_IN_DATE" >> ${OUTPUT_FILE}
    echo "${OPTION}=ONLY_FULL_GROUP_BY" >> ${OUTPUT_FILE}
    echo "${OPTION}=PAD_CHAR_TO_FULL_LENGTH" >> ${OUTPUT_FILE}
    echo "${OPTION}=PIPES_AS_CONCAT" >> ${OUTPUT_FILE}
    echo "${OPTION}=REAL_AS_FLOAT" >> ${OUTPUT_FILE}
    echo "${OPTION}=STRICT_ALL_TABLES" >> ${OUTPUT_FILE}
    echo "${OPTION}=STRICT_TRANS_TABLES" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--thread-handling" ]; then
    echoit "  > Adding possible values no-threads, one-thread-per-connection, dynamically-loaded, pool-of-threads for option '${OPTION}' to the final list..."
    echo "${OPTION}=no-threads" >> ${OUTPUT_FILE}
    echo "${OPTION}=one-thread-per-connection" >> ${OUTPUT_FILE}
    echo "${OPTION}=dynamically-loaded" >> ${OUTPUT_FILE}
    echo "${OPTION}=pool-of-threads" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--thread-pool-high-prio-mode" ]; then
    echoit "  > Adding possible values transactions, statements, none for option '${OPTION}' to the final list..."
    echo "${OPTION}=transactions" >> ${OUTPUT_FILE}
    echo "${OPTION}=statements" >> ${OUTPUT_FILE}
    echo "${OPTION}=none" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--transaction-isolation" ]; then
    echoit "  > Adding possible values READ-UNCOMMITTED, READ-COMMITTED, REPEATABLE-READ, SERIALIZABLE for option '${OPTION}' to the final list..."
    echo "${OPTION}=READ-UNCOMMITTED" >> ${OUTPUT_FILE}
    echo "${OPTION}=READ-COMMITTED" >> ${OUTPUT_FILE}
    echo "${OPTION}=REPEATABLE-READ" >> ${OUTPUT_FILE}
    echo "${OPTION}=SERIALIZABLE" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-doublewrite-files" ]; then
    echoit "  > Adding possible values 2, 16, 126, 256 for option '${OPTION}' to the final list..."
    echo "${OPTION}=2" >> ${OUTPUT_FILE}
    echo "${OPTION}=16" >> ${OUTPUT_FILE}
    echo "${OPTION}=126" >> ${OUTPUT_FILE}
    echo "${OPTION}=256" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--optimizer-trace" ]; then
    echoit "  > Adding possible values enabled and one_line for option '${OPTION}' to the final list..."
    echo "${OPTION}=enabled" >> ${OUTPUT_FILE}
    echo "${OPTION}=one_line" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--performance-schema-instrument" ]; then
    echoit "  > Adding possible values for option '${OPTION}' to the final list..."
    echo "${OPTION}='wait/synch/cond/%=COUNTED'" >> ${OUTPUT_FILE}
    echo "${OPTION}='wait/synch/cond/%=0'" >> ${OUTPUT_FILE}
    echo "${OPTION}='wait/synch/cond/%=1'" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--block-encryption-mode" ]; then
    echoit "  > Adding possible values aes-128-ecb, aes-128-cbc, aes-128-cfb1, aes-192-ecb, aes-192-cbc, aes-192-ofb, aes-256-ecb, aes-256-cbc, aes-256-cfb128 for option '${OPTION}' to the final list..."
    echo "${OPTION}=aes-128-ecb" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-128-cbc" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-128-cfb1" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-192-ecb" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-192-cbc" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-192-ofb" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-256-ecb" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-256-cbc" >> ${OUTPUT_FILE}
    echo "${OPTION}=aes-256-cfb128" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--default-authentication-plugin" ]; then
    echoit "  > Adding possible values mysql_native_password, sha256_password, caching_sha2_password for option '${OPTION}' to the final list..."
    echo "${OPTION}=mysql_native_password" >> ${OUTPUT_FILE}
    echo "${OPTION}=sha256_password" >> ${OUTPUT_FILE}
    echo "${OPTION}=caching_sha2_password" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-change-buffering" ]; then
    echoit "  > Adding possible values all, none, inserts, deletes, changes, purges for option '${OPTION}' to the final list..."
    echo "${OPTION}=all" >> ${OUTPUT_FILE}
    echo "${OPTION}=none" >> ${OUTPUT_FILE}
    echo "${OPTION}=inserts" >> ${OUTPUT_FILE}
    echo "${OPTION}=deletes" >> ${OUTPUT_FILE}
    echo "${OPTION}=changes" >> ${OUTPUT_FILE}
    echo "${OPTION}=purges" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-default-row-format" ]; then
    echoit "  > Adding possible values dynamic, compact, redundant for option '${OPTION}' to the final list..."
    echo "${OPTION}=dynamic" >> ${OUTPUT_FILE}
    echo "${OPTION}=compact" >> ${OUTPUT_FILE}
    echo "${OPTION}=redundant" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--log-output" ]; then
    echoit "  > Adding possible values FILE, TABLE, NONE for option '${OPTION}' to the final list..."
    echo "${OPTION}=FILE" >> ${OUTPUT_FILE}
    echo "${OPTION}=TABLE" >> ${OUTPUT_FILE}
    echo "${OPTION}=NONE" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--master-info-repository" ]; then
    echoit "  > Adding possible values FILE, TABLE for option '${OPTION}' to the final list..."
    echo "${OPTION}=FILE" >> ${OUTPUT_FILE}
    echo "${OPTION}=TABLE" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--relay-log-info-repository" ]; then
    echoit "  > Adding possible values FILE, TABLE for option '${OPTION}' to the final list..."
    echo "${OPTION}=FILE" >> ${OUTPUT_FILE}
    echo "${OPTION}=TABLE" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--windowing-use-high-precision" ]; then
    echoit "  > Adding possible values 0, 1 for option '${OPTION}' to the final list..."
    echo "${OPTION}=0" >> ${OUTPUT_FILE}
    echo "${OPTION}=1" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--default-storage-engine" ]; then
    echoit "  > Adding possible value InnoDB for option '${OPTION}' to the final list..."
    echo "${OPTION}=InnoDB" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--binlog-row-metadata" ]; then
    echoit "  > Adding possible value full and minimal for option '${OPTION}' to the final list..."
    echo "${OPTION}=full" >> ${OUTPUT_FILE}
    echo "${OPTION}=minimal" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--binlog-transaction-dependency-tracking" ]; then
    echoit "  > Adding possible value COMMIT_ORDER , WRITESET and WRITESET_SESSION for option '${OPTION}' to the final list..."
    echo "${OPTION}=COMMIT_ORDER" >> ${OUTPUT_FILE}
    echo "${OPTION}=WRITESET" >> ${OUTPUT_FILE}
    echo "${OPTION}=WRITESET_SESSION" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--default-tmp-storage-engine" ]; then
    echoit "  > Adding possible value InnoDB  for option '${OPTION}' to the final list..."
    echo "${OPTION}=InnoDB" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--ft-boolean-syntax" ]; then
    echoit "  > Adding possible values for option '${OPTION}' to the final list..."
    echo "${OPTION}='+ -><()~*:""&|'" >> ${OUTPUT_FILE}
    echo "${OPTION}='+ -><(~*:)""&|'" >> ${OUTPUT_FILE}
    echo "${OPTION}=' +-><(~*:)""&|'" >> ${OUTPUT_FILE}
    echo "${OPTION}=' +-><(:~*)""&|'" >> ${OUTPUT_FILE}
    echo "${OPTION}='| -><(:~*)""&+'" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--group-replication-consistency" ]; then
    echoit "  > Adding possible values for option '${OPTION}' to the final list..."
    echo "${OPTION}=BEFORE" >> ${OUTPUT_FILE}
    echo "${OPTION}=EVENTUAL" >> ${OUTPUT_FILE}
    echo "${OPTION}=BEFORE_ON_PRIMARY_FAILOVER" >> ${OUTPUT_FILE}
    echo "${OPTION}=AFTER" >> ${OUTPUT_FILE}
    echo "${OPTION}=BEFORE_AND_AFTER" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-compress-debug" ]; then
    echoit "  > Adding possible values none, zlib, lz4, lz4hc for option '${OPTION}' to the final list..."
    echo "${OPTION}=none" >> ${OUTPUT_FILE}
    echo "${OPTION}=zlib" >> ${OUTPUT_FILE}
    echo "${OPTION}=lz4" >> ${OUTPUT_FILE}
    echo "${OPTION}=lz4hc" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--innodb-segment-reserve-factor" ]; then
    echoit "  > Adding possible values 0.03, 12.5, 20, 30, 39, 40 for option '${OPTION}' to the final list..."
    echo "${OPTION}=0.03" >> ${OUTPUT_FILE}
    echo "${OPTION}=12.5" >> ${OUTPUT_FILE}
    echo "${OPTION}=20" >> ${OUTPUT_FILE}
    echo "${OPTION}=30" >> ${OUTPUT_FILE}
    echo "${OPTION}=39" >> ${OUTPUT_FILE}
    echo "${OPTION}=40" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--lc-messages" ]; then
    echoit "  > Adding possible values en_US for option '${OPTION}' to the final list..."
    echo "${OPTION}=en_US" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--lc-time-names" ]; then
    echoit "  > Adding possible values en_US for option '${OPTION}' to the final list..."
    echo "${OPTION}=en_US" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--mysqlx-compression-algorithms" ]; then
    echoit "  > Adding possible values DEFLATE_STREAM, LZ4_MESSAGE, ZSTD_STREAM for option '${OPTION}' to the final list..."
    echo "${OPTION}=DEFLATE_STREAM" >> ${OUTPUT_FILE}
    echo "${OPTION}=LZ4_MESSAGE" >> ${OUTPUT_FILE}
    echo "${OPTION}=ZSTD_STREAM" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--optimizer-switch" ]; then
    echoit "  > Adding possible values for option '${OPTION}' to the final list..."
    echo "${OPTION}=index_merge=on,index_merge_union=on,index_merge_sort_union=on,index_merge_intersection=on,engine_condition_pushdown=on,index_condition_pushdown=on,mrr=on,mrr_cost_based=on,block_nested_loop=on,batched_key_access=off,materialization=on,semijoin=on,loosescan=on,firstmatch=on,duplicateweedout=on,subquery_materialization_cost_based=on,use_index_extensions=on,condition_fanout_filter=on,derived_merge=on,use_invisible_indexes=off,skip_scan=on,hash_join=on,subquery_to_derived=off,prefer_ordering_index=on,hypergraph_optimizer=off,derived_condition_pushdown=on,favor_range_scan=off" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--optimizer-trace-features" ]; then
    echoit "  > Adding possible values greedy_search, range_optimizer, dynamic_range, repeated_subselect for option '${OPTION}' to the final list..."
    echo "${OPTION}=greedy_search=on,range_optimizer=on,dynamic_range=on,repeated_subselect=on" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--protocol-compression-algorithms" ]; then
    echoit "  > Adding possible values zlib, zstd, uncompressed for option '${OPTION}' to the final list..."
    echo "${OPTION}=zlib" >> ${OUTPUT_FILE}
    echo "${OPTION}=zstd" >> ${OUTPUT_FILE}
    echo "${OPTION}=uncompressed" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--replica-exec-mode" ]; then
    echoit "  > Adding possible value STRICT and IDEMPOTENT for option '${OPTION}' to the final list..."
    echo "${OPTION}=STRICT" >> ${OUTPUT_FILE}
    echo "${OPTION}=IDEMPOTENT" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--replica-parallel-type" ]; then
    echoit "  > Adding possible value DATABASE and LOGICAL_CLOCK for option '${OPTION}' to the final list..."
    echo "${OPTION}=DATABASE" >> ${OUTPUT_FILE}
    echo "${OPTION}=LOGICAL_CLOCK" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--session-track-system-variables" ]; then
    echoit "  > Adding possible values time_zone, autocommit, character_set_client, character_set_results, character_set_connection for option '${OPTION}' to the final list..."
    echo "${OPTION}=time_zone" >> ${OUTPUT_FILE}
    echo "${OPTION}=autocommit" >> ${OUTPUT_FILE}
    echo "${OPTION}=character_set_client" >> ${OUTPUT_FILE}
    echo "${OPTION}=character_set_results" >> ${OUTPUT_FILE}
    echo "${OPTION}=character_set_connection" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--tls-version" ]; then
    echoit "  > Adding possible value TLSv1, TLSv1.1, TLSv1.2, TLSv1.3 for option '${OPTION}' to the final list..."
    echo "${OPTION}=TLSv1" >> ${OUTPUT_FILE}
    echo "${OPTION}=TLSv1.1" >> ${OUTPUT_FILE}
    echo "${OPTION}=TLSv1.2" >> ${OUTPUT_FILE}
    echo "${OPTION}=TLSv1.3" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--upgrade" ]; then
    echoit "  > Adding possible value NONE, MINIMAL, AUTO, FORCE for option '${OPTION}' to the final list..."
    echo "${OPTION}=NONE" >> ${OUTPUT_FILE}
    echo "${OPTION}=MINIMAL" >> ${OUTPUT_FILE}
    echo "${OPTION}=AUTO" >> ${OUTPUT_FILE}
    echo "${OPTION}=FORCE" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--binlog-checksum" ]; then
    echoit "  > Adding possible value NONE, CRC32 for option '${OPTION}' to the final list..."
    echo "${OPTION}=NONE" >> ${OUTPUT_FILE}
    echo "${OPTION}=CRC32" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--event-scheduler" ]; then
    echoit "  > Adding possible value ON, OFF, DISABLED for option '${OPTION}' to the final list..."
    echo "${OPTION}=ON" >> ${OUTPUT_FILE}
    echo "${OPTION}=OFF" >> ${OUTPUT_FILE}
    echo "${OPTION}=DISABLED" >> ${OUTPUT_FILE}
  elif [ "${OPTION}" == "--gtid-mode" ]; then
    echoit "  > Adding possible value ON, OFF, OFF_PERMISSIVE, ON_PERMISSIVE for option '${OPTION}' to the final list..."
    echo "${OPTION}=ON --enforce-gtid-consistency=ON " >> ${OUTPUT_FILE}
    echo "${OPTION}=OFF" >> ${OUTPUT_FILE}
    echo "${OPTION}=OFF_PERMISSIVE" >> ${OUTPUT_FILE}
    echo "${OPTION}=ON_PERMISSIVE" >> ${OUTPUT_FILE}  
  elif [ "${VALUE}" == "TRUE" -o "${VALUE}" == "FALSE" -o "${VALUE}" == "ON" -o "${VALUE}" == "OFF" -o "${VALUE}" == "YES" -o "${VALUE}" == "NO" ]; then
    echoit "  > Adding possible values TRUE/ON/YES/1 and FALSE/OFF/NO/0 (as a universal 1 and 0) for option '${OPTION}' to the final list..."
    echo "${OPTION}=1" >> ${OUTPUT_FILE}
    echo "${OPTION}=0" >> ${OUTPUT_FILE}
  elif [[ "$(echo ${VALUE} | tr -d ' ')" =~ ^-?[0-9]+$ ]]; then
    if [ "${VALUE}" != "0" ]; then 
      echoit "  > Adding int values (${VALUE}, -1, 0, 1, 2, 12, 24, 254, 1023, 2047, -1125899906842624, 1125899906842624) for option '${OPTION}' to the final list..."
      echo "${OPTION}=${VALUE}" >> ${OUTPUT_FILE}
    else
      echoit "  > Adding int values (-1, 0, 1, 2, 12, 24, 254, 1023, 2047, -1125899906842624, 1125899906842624) for option '${OPTION}' to the final list..."
    fi
    echo "${OPTION}=0" >> ${OUTPUT_FILE}
    echo "${OPTION}=1" >> ${OUTPUT_FILE}
    echo "${OPTION}=2" >> ${OUTPUT_FILE}
    echo "${OPTION}=12" >> ${OUTPUT_FILE}
    echo "${OPTION}=24" >> ${OUTPUT_FILE}
    echo "${OPTION}=254" >> ${OUTPUT_FILE}
    echo "${OPTION}=1023" >> ${OUTPUT_FILE}
    echo "${OPTION}=2047" >> ${OUTPUT_FILE}
    echo "${OPTION}=-1125899906842624" >> ${OUTPUT_FILE}
    echo "${OPTION}=1125899906842624" >> ${OUTPUT_FILE}
  elif [ "${VALUE}" == "" -o "${VALUE}" == "(No" ]; then
    echoit "  > Assert: Option '${OPTION}' is blank by default and not programmed into the script yet, please cover this in the script..."
    exit 1
  else
    echoit "  > ${OPTION} IS NOT COVERED YET, PLEASE ADD!!!"
    exit 1
  fi
done < ${TEMP_FILE}

rm -Rf ${TEMP_FILE}
echo "Done! Output file: ${OUTPUT_FILE}"
