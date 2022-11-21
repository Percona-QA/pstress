# What is pstress ?

pstress is a probability-based open-source database testing tool designed to run in concurrency and to test if the database can recover when something goes wrong. It generates random transactions based on options provided by the user. With the right set of options, users can test features, regression, and crash recovery. It can create concurrent load on a cluster or on a single server.

pstress is extended using the existing framework of pquery and uses a driver script to perform concurrency and crash recovery. pstress primarily has 2 modules:

Workload ( multi-threaded program written in C++ that generates random metadata load and SQLs to execute )
Driver script (written in BASH which integrates the workload to perform concurrency and crash recovery testing )

# What is new in pstress ?

- pstress can be run stand-alone against a running server OR run with the driver shell script and a configuration file. In the later case, the user does not need to start the server manually. The driver shell script starts the server and executes pstress binaries with the set of options passed from the configuration file. After a certain time interval, it stops the server, saves the data directory and restarts the server by varying some server variables and then continues the load. This set of actions is called a step.
- During each step, the server is bombarded with combinations of SQLs initiated by multiple threads in different SQL connections.
- After each step, crash recovery is performed using the data directory from the previous step.
- pstress can generate different varieties of load based on the options provided by the user. For example, the tool can generate a single-threaded test having only inserts in one table or it can generate complex transactions in multiple threads.
- Kill or shutdown the running server or a node and let it recover and check 
- In case any issue is identified, it is reported in the pstress run logs. By default, pstress saves each trial directory which has the data directory, server error logs, pstress-thread logs. It also saves the mysqld binaries and the configuration file used for running the test.

# How to build pstress ?

1. Install cmake >= 2.6 and C++ compiler >= 4.7 (gcc-c++ for RedHat-based, g++ for Debian-based), the development files for your MySQL version/fork, and potentially OpenSSL and AIO development files and/or other deps if needed.
2. Change dir to pstress
3. Run cmake with the required options, which are:
   *  *-DPERCONASERVER*=**ON** - build pstress with Percona Server
   *  *-DMYSQL*=**ON** - build pstress with MySQL Server
   *  *-DPERCONACLUSTER*=**ON** - build pstress with Percona XtraDB Cluster
4. If you have MySQL | Percona Server | Percona XtraDB Cluster installed to some custom location you may consider setting the additional flags to cmake: *MYSQL_INCLUDE_DIR* and *MYSQL_LIBRARY*. OR, you can set *BASEDIR* variable if you have binary tarball extracted to some custom place for fully automatic library detection (recommended).
5. The resulting binary will automatically receive an appropriate flavor suffix:
  * *pstress-ms* for MySQL
  * *pstress-ps* for Percona Server
  * *pstress-pxc* for Percona XtraDB Cluster

# Can you give an easy build example using an extracted Percona Server tarball?
```
$ cd pstress
$ git clean -fd; rm CMakeCache.txt;
$ cmake . -DPERCONASERVER=ON -DBASEDIR=$HOME/mysql-8.0/bld/install
$ sudo make install # If you want pstress to be installed on the system, otherwise the binary can be found in ./src
$ ... build your other MySQL flavors/forks here in the same way, modifying the basedir and the servertype (both -D options) ...
```
# What options does pstress accept?

First, take a quick look at ``` ./pstress-ps --help, ./pstress-ps --help --verbose ``` to see available modes and options.

# Command line options example:

Option | Function | Example | Default
--- | --- | --- | ---
--add-column | alter table add some random column | --add-column=10 | default#: 1 
--add-drop-partition | randomly add drop new partition | | default#: 3
--add-index | alter table add random index | | default#: 1
--address | IP address to connect to | | default: 
--alt-db-enc | Alter Database Encryption mode to Y/N | | default#: 1
--alt-discard-tbs | ALTER TABLE table_name DISCARD TABLESPACE | --alt-discard-tbs=50 | default#: 1
--alter-algorith | algorithm used in alter table. INPLACE/COPY/DEFAULT/ALL | --alter-algorith=INPLACE | default: all
--alter-lock | lock mechanism used in alter table. | --alter-lock=NONE | default: all
--alter-redo-log | Alter instance enable/disable redo log | | default#: 0
--alter-table-compress | Alter table compression | --alter-table-compress=50 | default#: 10
--alter-table-encrypt | Alter table set Encryption | --alter-table-encrypt=50 | default#: 10
--alt-tbs-enc | Alter tablespace set Encryption | --alt-tbs-enc=50 | default#: 1
--alt-tbs-rename | Alter tablespace rename | --alt-tbs-rename=50 | default#: 1
--analyze | Analyze table, for partition table randomly analyze either partition or full table | --analyze=10 | default#: 1
--check | check table, for partition table randomly check either partition or full table | | default#: 5
--columns | maximum columns in a table, default depends on page-size, branch. for 8.0 it is 7 for 5.7 it 10 | --columns=10 | default#: 10
--commit-rollback-ratio |  ratio of commit to rollback. e.g. if 5, then 5 transactions will be committed and 1 will be rollback. if 0 then all transactions will be rollback | | default#: 5
--config-file | Config file to use for test | | default: 
--database | The database to connect to | | default: test
--delete-all-row | delete all rows of a table | --delete-all-row=5 | default#: 1
--delete-with-cond | delete row with where condition | --delete-with-cond=300 | default#: 200
--drop-column | alter table drop some random column | --drop-column=10 | default#: 1
--drop-index | alter table drop random index | | default#: 1
--encryption-type | all ==> keyring/Y/N | --encryption-type=keyring | default: Y/N
--engine | Engine used | --engine=InnoDB | default: INNODB
--exact-initial-records | When passed with --records (N) option inserts exact number of  N records in tables | | default: 0
--help | user asked for help | | default: 1
--index-columns | maximum columns in an index of a table, default depends on page-size as well | --index-columns=5 | default#: 10
--indexes | maximum indexes in a table,default depends on page-size as well | --indexes=2 | default#: 7
--infile | The SQL input file | | default: pquery.sql
--insert-row | insert random row | --insert-row=500 | default#: 600
--jlddl | load DDL and exit | --jlddl | default: 0
--log-all-queries | Log all queries (succeeded and failed) | | default: 1
--log-client-output | Log query output to separate file | | default: 0
--logdir | Log directory | | default: /tmp
--log-failed-queries | Log all failed queries | | default: 0
--log-query-duration | Log query duration in milliseconds | | default: 0
--log-query-numbers | write query # to logs | | default: 0
--log-query-statistics | extended output of query result | | default: 0
--log-succeeded-queries | Log succeeded queries | | default: 0
--max-partitions | maximum number of partitions in table | choose between 1 and 8192 | default#: 25
--metadata-path | path of metadata file | | default: 
--modify-column | Alter table column modify | | default#: 10
--mso | mysqld server options variables which are set during the load, see --set-variable. n:option=v1=v2 where n is probability of picking option, v1 and v2 different value that are supported. | --mso=innodb_temp_tablespace_encrypt=on=off | default: 
--no-auto-inc | Disable auto inc columns in table, including pkey | | default: 0
--no-blob | Disable blob columns | | default: 0
--no-column-compression | Disable column compression. It is percona style compression | | default: 0
--no-ddl | do not use ddl in workload | --no-ddl | default: 0
--no-delete | do not execute any type of delete on tables | | default: 0
--no-desc-index | Disable index with desc on tables | | default: 0
--no-encryption | Disable All type of encryption | --no-encryption | default: 0
--no-insert | do not execute insert into tables | --alt-db-enc=30 | default: 0
--no-partition-tables | do not work on partition tables | | default: 0
--no-select | do not execute any type select on tables | | default: 0
--no-shuffle | execute SQL sequentially/ randomly | | default: 0
--no-table-compression | Disable table compression | | default: 0
--no-tbs | disable all type of tablespace including the general tablespace | | default: 0
--no-temp-tables | do not work on temporary tables | | default: 0
--no-update | do not execute any type of update on tables | | default: 0
--no-virtual | Disable virtual columns | | default: 0
--only-cl-ddl | only run command line ddl. other ddl will be disabled | --only-cl-ddl | default: 0
--only-cl-sql | only run command line sql. other sql will be disable | --only-cl-sql | default: 0
--only-partition-tables | Work only on Partition tables | | default: 0
--only-temp-tables | Work only on temporary tables | | default: 0
--optimize | optimize table | --optimize=10 | default#: 3
--partition-types  | total partition supported | all for LIST, HASH, KEY, RANGE or to provide or pass for example --partition-types LIST,HASH,RANGE without space | default: all
--password | The MySQL user's password | | default:
--port: Port to use | | default#: 3306
--pquery | run pstress as pquery 2.0. sqls will be executed from --infine in some order based on shuffle. basically it will run in pquery mode you can also use -k | | default: 0
--prepare | create new random tables and insert initial records |  | default: 0
--primary-key-probability | Probability of adding primary key in a table | --primary-key-probability=40 | default#: 50
--queries-per-thread | The number of queries per thread | | default#: 1
--ratio-normal-temp | ratio of normal to temporary tables. for e.g. if ratio to normal table to temporary is 10 . --tables 40. them only 4 temporary table will be created per session  | --ratio-normal-temp=4 | default#: 10
--records | Number of initial records in table | --records=100 | default#: 1000
--recreate-table | drop and recreate table | --recreate-table=5 | default#: 1
--rename-column | alter table rename column | --rename-column=10 | default#: 1
--rename-index | alter table rename index | | default#: 1
--rotate-encryption-key | Alter instance rotate innodb system key X | | default#: 1
--rotate-master-key | Alter instance rotate innodb master key | --rotate-master-key=50 | default#: 1
--rotate-redo-log-key | Rotate redo log key | --rotate-redo-log-key=50 | default#: 1
--row-format | create table row format. it is the row format of table. a table can have compressed, dynamic, redundant row format. | --row-format=compressed | default: all
--savepoint-prb-k | probability of using savepoint in a transaction.Also 25% such transaction will be rollback to some savepoint | | default#: 50
--seconds | Number of seconds to execute workload | --seconds=100 | default#: 1000
--seed | Initial seed used for the test | --seed=1001 | Random value
--select-all-row | select all data probability | --select-all-row=10 | default#: 8
--select-single-row | Select table using single row | --select-single-row=20 | default#: 800
--set-variable | set mysqld variable during the load.(session|global) | --set-variable=autocommit=OFF | default#: 3
--socket | Socket file to use | | default: /tmp/socket.sock
--sof | server options file, MySQL server options file, picks some of the mysqld options, and try to set them during the load , using set global and set session | --sof=innodb_temp_tablespace_encrypt=on=off | default:
--special-sql | special sql | | default#: 10
--sql-file | file to be used  for special sql T1_INT_1, T1_INT_2 will be replaced with int columns of some table in database T1_VARCHAR_1, T1_VARCHAR_2 will be replaced with varchar columns of some table in database | | default: grammar.sql
--step | current step in pstress script | | default#: 1
--tables | Number of initial tables | --tables=10 | default#: 10
--tbs-count | random number of different general tablespaces | --tbs-count=3 | default#: 1
--test-connection | Test connection to server and exit | | default: 0
--threads | The number of threads to use | | default#: 1
--truncate | truncate table | --truncate=5 | default#: 1
--trx-prb-k | probability(out of 1000) of combining sql as single trx | | default#: 10
--trx-size | average size of each trx | | default#: 100
--undo-tbs-count | Number of default undo tablespaces | --undo-tbs-count=3 | default#: 3
--undo-tbs-sql | Assign probability of running create/alter/drop undo tablespace | --undo-tbs-sql=50 | default#: 1
--update-with-cond | Update row using where clause | --update-with-cond=500 | default#: 200
--user | The MySQL userID to be used | | default: root
--verbose | verbose | | default: 1


# How to do a sample pstress run

pstress must be run from the directory where the executable binaries are located. Commonly, the binaries are located inside the src directory.
cd pstress/src

```bash
./pstress-ps --tables 30 --logdir=$PWD/log --records 200 --threads 10 --seconds 100 --socket $SOCKET --insert-row 100 --update-with-cond 50 --no-delete --log-failed-queries --log-all-queries --no-encryption
```

# How to do a sample pstress run through Driver Script

It can be run with the driver shell script(pstress-run.sh) and a configuration file(pstress-run.conf) located in pstress directory.
cd pstress/pstress
```bash
nohup ./pstress-run.sh pstress-run.conf 2>&1 &
```
Check run logs through tail -f nohup.out


# Contributors
* Alexey Bychko - C++ code, cmake extensions
* Roel Van de Paar - invention, scripted framework
* Rahul Malik - pstress developer
* Mohit Joshi - pstress developer
* For the full list of contributors, please see [CONTRIBUTORS](https://github.com/Percona-QA/pstress/blob/master/doc/CONTRIBUTORS)
