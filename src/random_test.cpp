/*
 =========================================================
 #       Created by Rahul Malik, Percona LLC             #
 =========================================================
*/
#include "random_test.hpp"
#include "common.hpp"
#include "node.hpp"
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <libgen.h>

#define CR_SERVER_GONE_ERROR 2006
#define CR_SERVER_LOST 2013
using namespace rapidjson;
std::mt19937 rng;

const std::string partition_string = "_p";
const int version = 2;
/* range for random number int, integers, floats and double.
 more the value, less randomness.
 for example if it is 1. then there is very high chance of executing
 successful DML.
todo allow this option to be configured by user */
const int g_integer_range = 100;

static bool encrypted_temp_tables = false;
static bool encrypted_sys_tablelspaces = false;
static bool keyring_comp_status = false;
static std::vector<Table *> *all_tables = new std::vector<Table *>;
static std::vector<std::string> g_undo_tablespace;
static std::vector<std::string> g_encryption;
static std::vector<std::string> g_compression = {"none", "zlib", "lz4"};
static std::vector<std::string> g_row_format;
static std::vector<std::string> g_tablespace;
static std::vector<std::string> locks;
static std::vector<std::string> algorithms;
static std::vector<int> g_key_block_size;
static int g_max_columns_length = 30;
static int g_innodb_page_size;
static int sum_of_all_opts = 0; // sum of all probablility
std::mutex ddl_logs_write;
static std::chrono::system_clock::time_point start_time =
    std::chrono::system_clock::now();

std::atomic<size_t> table_started(0);
std::atomic<size_t> check_failures(0);
std::atomic<size_t> table_completed(0);
std::atomic_flag lock_stream = ATOMIC_FLAG_INIT;
std::atomic<bool> run_query_failed(false);
/* partition type supported by system */
std::vector<Partition::PART_TYPE> Partition::supported;
const int maximum_records_in_each_parititon_list = 100;

static MYSQL_ROW mysql_fetch_row_safe(Thd1 *thd) {
  if (!thd->result) {
    thd->thread_log << "mysql_fetch_row called with nullptr arg!";
    return nullptr;
  }
  return mysql_fetch_row(thd->result.get());
}

static bool mysql_num_fields_safe(Thd1 *thd, unsigned int req) {
  if (!thd->result) {
    thd->thread_log << "mysql_num_fields called with nullptr arg!";
    return 0;
  }
  auto num_fields = mysql_num_fields(thd->result.get());
  auto ret = req <= num_fields;
  if (!ret) {
    thd->thread_log << "Expected at least " << req << " fields but only "
                    << num_fields << " exist";
  }
  return ret;
}

/* run check table */
static bool get_check_result(const std::string &sql, Thd1 *thd) {

  execute_sql(sql, thd);
  auto row = mysql_fetch_row_safe(thd);
  if (row && mysql_num_fields_safe(thd, 4) && strcmp(row[3], "OK") != 0) {
    thd->thread_log << "Error: " << row[0] << " " << row[1] << " " << row[2]
                    << " " << row[3] << std::endl;
    return false;
  }

  return true;
}

static std::string mysql_read_single_value(const std::string &sql, Thd1 *thd) {
  std::string query_result = "";

  execute_sql(sql, thd);
  auto row = mysql_fetch_row_safe(thd);
  if (row && mysql_num_fields_safe(thd, 1))
    query_result = row[0];

  return query_result;
}

/* return server version in number format
 Example 8.0.26 -> 80026
 Example 5.7.35 -> 50735
*/
static int get_server_version() {
  std::string ps_base = mysql_get_client_info();
  unsigned long major = 0, minor = 0, version = 0;
  std::size_t major_p = ps_base.find(".");
  if (major_p != std::string::npos)
    major = stoi(ps_base.substr(0, major_p));

  std::size_t minor_p = ps_base.find(".", major_p + 1);
  if (minor_p != std::string::npos)
    minor = stoi(ps_base.substr(major_p + 1, minor_p - major_p));

  std::size_t version_p = ps_base.find(".", minor_p + 1);
  if (version_p != std::string::npos)
    version = stoi(ps_base.substr(minor_p + 1, version_p - minor_p));
  else
    version = stoi(ps_base.substr(minor_p + 1));
  auto server_version = major * 10000 + minor * 100 + version;
  return server_version;
}

/* return server version in number format
 Example 8.0.26 -> 80026
 Example 5.7.35 -> 50735
*/
static int server_version() {
  static int sv = get_server_version();
  return sv;
}

/* return probabality of all options and disable some feature based on user
 * request/ branch/ fork */
int sum_of_all_options(Thd1 *thd) {

  /* find out innodb page_size */
  if (options->at(Option::ENGINE)->getString().compare("INNODB") == 0) {
    g_innodb_page_size =
        std::stoi(mysql_read_single_value("select @@innodb_page_size", thd));
    assert(g_innodb_page_size % 1024 == 0);
    g_innodb_page_size /= 1024;
  }

  /*check which all partition type supported */
  auto part_supp = opt_string(PARTITION_SUPPORTED);
  if (part_supp.compare("all") == 0) {
    Partition::supported.push_back(Partition::KEY);
    Partition::supported.push_back(Partition::LIST);
    Partition::supported.push_back(Partition::HASH);
    Partition::supported.push_back(Partition::RANGE);
  } else {
    std::transform(part_supp.begin(), part_supp.end(), part_supp.begin(),
                   ::toupper);
    if (part_supp.find("HASH") != std::string::npos)
      Partition::supported.push_back(Partition::HASH);
    if (part_supp.find("KEY") != std::string::npos)
      Partition::supported.push_back(Partition::KEY);
    if (part_supp.find("LIST") != std::string::npos)
      Partition::supported.push_back(Partition::LIST);
    if (part_supp.find("RANGE") != std::string::npos)
      Partition::supported.push_back(Partition::RANGE);
  }

  if (options->at(Option::MAX_PARTITIONS)->getInt() < 1 ||
      options->at(Option::MAX_PARTITIONS)->getInt() > 8192)
    throw std::runtime_error(
        "invalid range for --max-partition. Choose between 1 and 8192");
  ;

  /* for 5.7 disable some features */
  if (server_version() < 80000) {
    opt_int_set(ALTER_TABLESPACE_RENAME, 0);
    opt_int_set(RENAME_COLUMN, 0);
    opt_int_set(UNDO_SQL, 0);
    opt_int_set(ALTER_REDO_LOGGING, 0);
  }

  /* check if keyring component is installed */
  if (mysql_read_single_value("SELECT status_value FROM performance_schema.keyring_component_status WHERE \
      status_key='component_status'", thd) == "Active")
    keyring_comp_status = true;

  auto lock = opt_string(LOCK);
  if (lock.compare("all") == 0) {
    locks.push_back("DEFAULT");
    locks.push_back("EXCLUSIVE");
    locks.push_back("SHARED");
    locks.push_back("NONE");
  } else {
    std::transform(lock.begin(), lock.end(), lock.begin(), ::toupper);
    if (lock.find("EXCLUSIVE") != std::string::npos)
      locks.push_back("EXCLUSIVE");
    if (lock.find("SHARED") != std::string::npos)
      locks.push_back("SHARED");
    if (lock.find("NONE") != std::string::npos)
      locks.push_back("NONE");
    if (lock.find("DEFAULT") != std::string::npos)
      locks.push_back("DEFAULT");
  }
  auto algorithm = opt_string(ALGORITHM);
  if (algorithm.compare("all") == 0) {
    algorithms.push_back("INPLACE");
    algorithms.push_back("COPY");
    algorithms.push_back("INSTANT");
    algorithms.push_back("DEFAULT");
  } else {
    std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
                   ::toupper);
    if (algorithm.find("INPLACE") != std::string::npos)
      algorithms.push_back("INPLACE");
    if (algorithm.find("COPY") != std::string::npos)
      algorithms.push_back("COPY");
    if (algorithm.find("INSTANT") != std::string::npos)
      algorithms.push_back("INSTANT");
    if (algorithm.find("DEFAULT") != std::string::npos)
      algorithms.push_back("DEFAULT");
  }

  /* Disabling alter discard tablespace until 8.0.30
   * Bug: https://jira.percona.com/browse/PS-7865 is fixed by upstream in
   * MySQL 8.0.31 */
  if (server_version() >= 80000 && server_version() <= 80030) {
    opt_int_set(ALTER_DISCARD_TABLESPACE, 0);
  }

  auto enc_type = options->at(Option::ENCRYPTION_TYPE)->getString();

  /* for percona-server we have additional encryption type keyring */
  if (enc_type.compare("all") == 0) {
    g_encryption = {"Y", "N"};
    if (strcmp(FORK, "Percona-Server") == 0) {
      g_encryption.push_back("KEYRING");
    }
  } else if (enc_type.compare("oracle") == 0) {
    g_encryption = {"Y", "N"};
    options->at(Option::ALTER_ENCRYPTION_KEY)->setInt(0);
  } else
    g_encryption = {enc_type};

  /* feature not supported by oracle */
  if (strcmp(FORK, "MySQL") == 0) {
    options->at(Option::ALTER_DATABASE_ENCRYPTION)->setInt(0);
    options->at(Option::NO_COLUMN_COMPRESSION)->setBool("true");
    options->at(Option::ALTER_ENCRYPTION_KEY)->setInt(0);
  }

  if (server_version() >= 80000) {
    /* for 8.0 default columns set default columns */
    if (!options->at(Option::COLUMNS)->cl)
      options->at(Option::COLUMNS)->setInt(7);
  }

  if (options->at(Option::ONLY_PARTITION)->getBool() &&
      options->at(Option::ONLY_TEMPORARY)->getBool())
    throw std::runtime_error("choose either only partition or only temporary ");

  if (options->at(Option::ONLY_PARTITION)->getBool() &&
      options->at(Option::NO_PARTITION)->getBool())
    throw std::runtime_error("choose either only partition or no partition");

  if (options->at(Option::ONLY_PARTITION)->getBool())
    options->at(Option::NO_TEMPORARY)->setBool("true");

  /* if select is set as zero, disable all type of selects */
  if (options->at(Option::NO_SELECT)->getBool()) {
    options->at(Option::SELECT_ALL_ROW)->setInt(0);
    options->at(Option::SELECT_ROW_USING_PKEY)->setInt(0);
  }
  /* if delete is set as zero, disable all type of deletes */
  if (options->at(Option::NO_DELETE)->getBool()) {
    options->at(Option::DELETE_ALL_ROW)->setInt(0);
    options->at(Option::DELETE_ROW_USING_PKEY)->setInt(0);
  }
  /* If update is disable, set all update probability to zero */
  if (options->at(Option::NO_UPDATE)->getBool()) {
    options->at(Option::UPDATE_ROW_USING_PKEY)->setInt(0);
  }
  /* if insert is disable, set all insert probability to zero */
  if (options->at(Option::NO_INSERT)->getBool()) {
    opt_int_set(INSERT_RANDOM_ROW, 0);
  }
  /* if no-tbs, do not execute tablespace related sql */
  if (options->at(Option::NO_TABLESPACE)->getBool()) {
    opt_int_set(ALTER_TABLESPACE_RENAME, 0);
    opt_int_set(ALTER_TABLESPACE_ENCRYPTION, 0);
  }

  /* options to disable if engine is not INNODB */
  std::string engine = options->at(Option::ENGINE)->getString();
  std::transform(engine.begin(), engine.end(), engine.begin(), ::toupper);
  if (engine.compare("ROCKSDB") == 0) {
    options->at(Option::NO_TEMPORARY)->setBool("true");
    options->at(Option::NO_COLUMN_COMPRESSION)->setBool("true");
    options->at(Option::NO_ENCRYPTION)->setBool(true);
    options->at(Option::NO_DESC_INDEX)->setBool(true);
    options->at(Option::NO_TABLE_COMPRESSION)->setBool(true);
  }

  /* If no-encryption is set, disable all encryption options */
  if (options->at(Option::NO_ENCRYPTION)->getBool()) {
    opt_int_set(ALTER_TABLE_ENCRYPTION, 0);
    opt_int_set(ALTER_TABLESPACE_ENCRYPTION, 0);
    opt_int_set(ALTER_MASTER_KEY, 0);
    opt_int_set(ALTER_ENCRYPTION_KEY, 0);
    opt_int_set(ALTER_GCACHE_MASTER_KEY, 0);
    opt_int_set(ROTATE_REDO_LOG_KEY, 0);
    opt_int_set(ALTER_DATABASE_ENCRYPTION, 0);
    opt_int_set(ALTER_INSTANCE_RELOAD_KEYRING, 0);
  }

  if (mysql_read_single_value("select @@innodb_temp_tablespace_encrypt", thd) ==
      "1")
    encrypted_temp_tables = true;

  if (strcmp(FORK, "Percona-Server") == 0 &&
      mysql_read_single_value("select @@innodb_sys_tablespace_encrypt", thd) ==
          "1")
    encrypted_sys_tablelspaces = true;

  /* Disable GCache encryption for MS or PS, only supported in PXC-8.0 */
  if (strcmp(FORK, "Percona-XtraDB-Cluster") != 0 ||
      (strcmp(FORK, "Percona-XtraDB-Cluster") == 0 && server_version() < 80000))
    opt_int_set(ALTER_GCACHE_MASTER_KEY, 0);

  /* If OS is Mac, disable table compression as hole punching is not supported
   * on OSX */
  if (strcmp(PLATFORM_ID, "Darwin") == 0)
    options->at(Option::NO_TABLE_COMPRESSION)->setBool(true);

  /* If no-table-compression is set, disable all compression */
  if (options->at(Option::NO_TABLE_COMPRESSION)->getBool()) {
    opt_int_set(ALTER_TABLE_COMPRESSION, 0);
    g_compression.clear();
  }

  /* if no dynamic variables is passed set-global to zero */
  if (server_options->empty())
    opt_int_set(SET_GLOBAL_VARIABLE, 0);

  auto only_cl_ddl = opt_bool(ONLY_CL_DDL);
  auto only_cl_sql = opt_bool(ONLY_CL_SQL);
  auto no_ddl = opt_bool(NO_DDL);

  /* if set, then disable all other SQL*/
  if (only_cl_sql) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->sql && !opt->cl)
        opt->setInt(0);
    }
  }

  /* only-cl-ddl, if set then disable all other DDL */
  if (only_cl_ddl) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->ddl && !opt->cl)
        opt->setInt(0);
    }
  }

  if (only_cl_ddl && no_ddl)
    throw std::runtime_error("noddl && only-cl-ddl can't be passed together");

  /* if no ddl is set disable all ddl */
  if (no_ddl) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->sql && opt->ddl)
        opt->setInt(0);
    }
  }

  int total = 0;
  for (auto &opt : *options) {
    if (opt == nullptr)
      continue;
    if (opt->getType() == Option::INT)
      thd->thread_log << opt->getName() << "=>" << opt->getInt() << std::endl;
    else if (opt->getType() == Option::BOOL)
      thd->thread_log << opt->getName() << "=>" << opt->getBool() << std::endl;
    if (!opt->sql)
      continue;
    total += opt->getInt();
  }

  if (total == 0)
    throw std::runtime_error("no option selected");
  return total;
}

/* return some options */
Option::Opt pick_some_option() {
  int rd = rand_int(sum_of_all_opts, 1);
  for (auto &opt : *options) {
    if (opt == nullptr || !opt->sql)
      continue;
    if (rd <= opt->getInt())
      return opt->getOption();
    else
      rd -= opt->getInt();
  }
  return Option::MAX;
}

int sum_of_all_server_options() {
  int total = 0;
  for (auto &opt : *server_options) {
    total += opt->prob;
  }
  return total;
}

/* pick some algorithm. and if caller pass value of algo & lock set it */
inline static std::string
pick_algorithm_lock(std::string *const algo = nullptr,
                    std::string *const lock = nullptr) {

  std::string current_lock;
  std::string current_algo;

  current_algo = algorithms[rand_int(algorithms.size() - 1)];

/*
  Support Matrix	LOCK=DEFAULT	LOCK=EXCLUSIVE	 LOCK=NONE      LOCK=SHARED
  ALGORITHM=INPLACE	Supported	Supported	 Supported      Supported
  ALGORITHM=COPY	Supported	Supported	 Not Supported  Supported
  ALGORITHM=INSTANT	Supported	Not Supported	 Not Supported  Not Supported
  ALGORITHM=DEFAULT	Supported	Supported        Supported      Supported
*/

  /* If current_algo=INSTANT, we can set current_lock=DEFAULT directly as it is
   * the only supported option */
  if (current_algo == "INSTANT")
    current_lock = "DEFAULT";
  /* If current_algo=COPY; MySQL supported LOCK values are
   * DEFAULT,EXCLUSIVE,SHARED. At this point, it may pick LOCK=NONE as well, but
   * we will handle it later in the code. If current_algo=INPLACE|DEFAULT;
   * randomly pick any value, since all lock types are supported.*/
  else
    current_lock = locks[rand_int(locks.size() - 1)];

  /* Handling the incompatible combination at the end.
   * A user may see a deviation if he has opted for --alter-lock to NOT
   * run with DEFAULT. But this is an exceptional case.
   */
  if (current_algo == "COPY" && current_lock == "NONE")
    current_lock = "DEFAULT";

  if (algo != nullptr)
    *algo = current_algo;
  if (lock != nullptr)
    *lock = current_lock;

  return " LOCK=" + current_lock + ", ALGORITHM=" + current_algo;
}

/* set seed of current thread */
int set_seed(Thd1 *thd) {

  auto initial_seed = opt_int(INITIAL_SEED);
  initial_seed += options->at(Option::STEP)->getInt();

  rng = std::mt19937(initial_seed);
  thd->thread_log << "Initial seed " << initial_seed << std::endl;
  for (int i = 0; i < thd->thread_id; i++)
    rand_int(MAX_SEED_SIZE, MIN_SEED_SIZE);
  thd->seed = rand_int(MAX_SEED_SIZE, MIN_SEED_SIZE);
  thd->thread_log << "CURRENT SEED IS " << thd->seed << std::endl;
  return thd->seed;
}

/* generate random strings of size N_STR */
std::vector<std::string> *random_strs_generator(unsigned long int seed) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "0123456789";

  static const size_t N_STRS = 10000;

  std::default_random_engine rng(seed);
  std::uniform_int_distribution<> dist(0, sizeof(alphabet) / sizeof(*alphabet) -
                                              2);

  std::vector<std::string> *strs = new std::vector<std::string>;
  strs->reserve(N_STRS);
  std::generate_n(std::back_inserter(*strs), strs->capacity(), [&] {
    std::string str;
    str.reserve(MAX_RANDOM_STRING_SIZE);
    std::generate_n(std::back_inserter(str), MAX_RANDOM_STRING_SIZE,
                    [&]() { return alphabet[dist(rng)]; });

    return str;
  });
  return strs;
}

std::vector<std::string> *random_strs;

int rand_int(int upper, int lower) {
  assert(upper >= lower);
  std::uniform_int_distribution<std::mt19937::result_type> dist(
      lower, upper); // distribution in range [lower, upper]
  return dist(rng);
}

/* return random float number in the range of upper and lower */
std::string rand_float(float upper, float lower) {
  assert(upper >= lower);
  static std::uniform_real_distribution<> dis(lower, upper);
  std::ostringstream out;
  out << std::fixed;
  out << std::setprecision(2) << (float)(dis(rng));
  return out.str();
}

std::string rand_double(double upper, double lower) {
  assert(upper >= lower);
  static std::uniform_real_distribution<> dis(lower, upper);
  std::ostringstream out;
  out << std::fixed;
  out << std::setprecision(5) << (double)(dis(rng));
  return out.str();
}

/* return random string in range of upper and lower */
std::string rand_string(int upper, int lower) {
  std::string rs = ""; /*random_string*/
  auto size = rand_int(upper, lower);

  while (size > 0) {
    auto str = random_strs->at(rand_int(random_strs->size() - 1));
    if (size > MAX_RANDOM_STRING_SIZE)
      rs += str;
    else
      rs += str.substr(0, size);
    size -= MAX_RANDOM_STRING_SIZE;
  }
  return rs;
}

/* return column type from a string */
Column::COLUMN_TYPES Column::col_type(std::string type) {
  if (type.compare("INTEGER") == 0)
    return INTEGER;
  else if (type.compare("INT") == 0)
    return INT;
  else if (type.compare("CHAR") == 0)
    return CHAR;
  else if (type.compare("VARCHAR") == 0)
    return VARCHAR;
  else if (type.compare("BOOL") == 0)
    return BOOL;
  else if (type.compare("GENERATED") == 0)
    return GENERATED;
  else if (type.compare("BLOB") == 0)
    return BLOB;
  else if (type.compare("FLOAT") == 0)
    return FLOAT;
  else if (type.compare("DOUBLE") == 0)
    return DOUBLE;
  else
    throw std::runtime_error("unhandled " + col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
}

/* return string from a column type */
const std::string Column::col_type_to_string(COLUMN_TYPES type) {
  switch (type) {
  case INTEGER:
    return "INTEGER";
  case INT:
    return "INT";
  case CHAR:
    return "CHAR";
  case DOUBLE:
    return "DOUBLE";
  case FLOAT:
    return "FLOAT";
  case VARCHAR:
    return "VARCHAR";
  case BOOL:
    return "BOOL";
  case BLOB:
    return "BLOB";
  case GENERATED:
    return "GENERATED";
  case COLUMN_MAX:
    break;
  }
  return "FAIL";
}

/* integer range */

static std::string rand_value_universal(Column::COLUMN_TYPES type_,
                                        int length) {
  int rand_length;
  switch (type_) {
  case (Column::COLUMN_TYPES::INTEGER):
    return std::to_string(
        rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));
    break;
  case (Column::COLUMN_TYPES::INT):
    return std::to_string(
        rand_int(g_integer_range *
                 options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));
    break;
  case (Column::COLUMN_TYPES::FLOAT): {
    return rand_float(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt());
    break;
  }
  case (Column::COLUMN_TYPES::DOUBLE): {
    return rand_double(1.0 / g_integer_range *
                       options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt());
    break;
  }
  case Column::COLUMN_TYPES::CHAR:
  case Column::COLUMN_TYPES::VARCHAR:
    return "\'" + rand_string(length) + "\'";
    break;
  case Column::COLUMN_TYPES::BOOL:
    return (rand_int(1) == 1 ? "true" : "false");
    break;
  case Column::COLUMN_TYPES::BLOB:
    rand_length = rand_int(length);
    if (rand_int(10) != 10)
      rand_length /= 10;
    return "\'" + rand_string(rand_length) + "\'";
    break;
  case Column::COLUMN_TYPES::GENERATED:
  case Column::COLUMN_TYPES::COLUMN_MAX:
    throw std::runtime_error("unhandled " + Column::col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
  }
  return "";
}

/* return random value of any string */
std::string Column::rand_value() { return rand_value_universal(type_, length); }

/* return random value of sub string */
std::string Generated_Column::rand_value() {
  return rand_value_universal(g_type, length);
}

/* prepare single quoted string for LIKE clause */
std::string &Table::prepare_like_string(std::string &&str) {
  /* Check if the incoming string is empty */
  if (str.at(0) == '\'' && str.at(1) == '\'')
    str = str.insert(1, 1, '%');
  /* Processing the single quoted values that are returned by 'rand_string' */
  else if (str.at(0) == '\'') {
    str = str.substr(0, 2);
    str = str.insert(2, 1, '\'');
    str = str.insert(1, 1, '%');
    str = str.insert(3, 1, '%');
  } else /*Return non-string number with single quotes */ {
    str = "\'%" + str + "%\'";
  }
  return str;
}

/* return table definition */
std::string Column::definition() {
  std::string def = name_ + " " + clause();
  if (null)
    def += " NOT NULL";
  if (auto_increment)
    def += " AUTO_INCREMENT";
  if (compressed) {
    def += " COLUMN_FORMAT COMPRESSED";
  }
  return def;
}

/* add new column, part of create table or Alter table */
Column::Column(std::string name, Table *table, COLUMN_TYPES type)
    : table_(table) {
  type_ = type;
  switch (type) {
  case CHAR:
    name_ = "c" + name;
    length = rand_int(g_max_columns_length, 10);
    break;
  case VARCHAR:
    name_ = "v" + name;
    length = rand_int(g_max_columns_length, 10);
    break;
  case INT:
  case INTEGER:
    name_ = "i" + name;
    if (rand_int(10) == 1)
      length = rand_int(100, 20);
    break;
  case FLOAT:
    name_ = "f" + name;
    break;
  case DOUBLE:
    name_ = "d" + name;
    break;
  case BOOL:
    name_ = "t" + name;
    break;
  default:
    throw std::runtime_error("unhandled " + col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
  }
}

/* add new blob column, part of create table or Alter table */
Blob_Column::Blob_Column(std::string name, Table *table)
    : Column(table, Column::BLOB) {

  if (options->at(Option::NO_COLUMN_COMPRESSION)->getBool() == false &&
      rand_int(1) == 1)
    compressed = true;
  switch (rand_int(5, 1)) {
  case 1:
    sub_type = "MEDIUMTEXT";
    name_ = "mt" + name;
    break;
  case 2:
    sub_type = "TEXT";
    name_ = "t" + name;
    break;
  case 3:
    sub_type = "LONGTEXT";
    name_ = "lt" + name;
    break;
  case 4:
    sub_type = "BLOB";
    name_ = "b" + name;
    break;
  case 5:
    sub_type = "LONGBLOB";
    name_ = "lb" + name;
    break;
  }
}

Blob_Column::Blob_Column(std::string name, Table *table, std::string sub_type_)
    : Column(table, Column::BLOB) {
  name_ = name;
  sub_type = sub_type_;
}

/* Constructor used for load metadata */
Generated_Column::Generated_Column(std::string name, Table *table,
                                   std::string clause, std::string sub_type)
    : Column(table, Column::GENERATED) {
  name_ = name;
  str = clause;
  g_type = Column::col_type(sub_type);
}

/* Generated column constructor. lock table before calling */
Generated_Column::Generated_Column(std::string name, Table *table)
    : Column(table, Column::GENERATED) {
  name_ = "g" + name;
  auto blob_supported = !options->at(Option::NO_BLOB)->getBool();
  g_type = COLUMN_MAX;
  /* Generated columns are 2:2:2:2 (INT:VARCHAR:CHAR:BLOB) */
  while (g_type == COLUMN_MAX) {
    auto x = rand_int(4, 1);
    if (x <= 1)
      g_type = INT;
    else if (x <= 2)
      g_type = VARCHAR;
    else if (x <= 3)
      g_type = CHAR;
    else if (blob_supported && x <= 4) {
      g_type = BLOB;
    }
  }

  if (options->at(Option::NO_COLUMN_COMPRESSION)->getBool() == false &&
      rand_int(1) == 1 && g_type == BLOB)
    compressed = true;

  /*number of columns in generated columns */
  size_t columns = rand_int(.6 * table->columns_->size()) + 1;

  std::vector<size_t> col_pos; // position of columns
  while (col_pos.size() < columns) {
    size_t col = rand_int(table->columns_->size() - 1);
    if (!table->columns_->at(col)->auto_increment &&
        table->columns_->at(col)->type_ != GENERATED)
      col_pos.push_back(col);
  }

  if (g_type == INT || g_type == INTEGER) {
    str = " " + col_type_to_string(g_type) + " GENERATED ALWAYS AS (";
    for (auto pos : col_pos) {
      auto col = table->columns_->at(pos);
      if (col->type_ == VARCHAR || col->type_ == CHAR || col->type_ == BLOB)
        str += " LENGTH(" + col->name_ + ")+";
      else if (col->type_ == INT || col->type_ == INTEGER ||
               col->type_ == BOOL || col->type_ == FLOAT ||
               col->type_ == DOUBLE)
        str += " " + col->name_ + "+";
      else
        throw std::runtime_error("unhandled " + col_type_to_string(col->type_) +
                                 " at line " + std::to_string(__LINE__));
    }
    str.pop_back();
  } else if (g_type == VARCHAR || g_type == CHAR || g_type == BLOB) {
    auto size = rand_int(g_max_columns_length, col_pos.size());
    int actual_size = 0;
    std::string gen_sql;
    for (auto pos : col_pos) {
      auto col = table->columns_->at(pos);
      auto current_size = rand_int((int)size / col_pos.size() * 2, 1);
      int column_size = 0;
      /* base column */
      switch (col->type_) {
      case INT:
      case INTEGER:
        column_size = 10; // interger max string size is 10
        break;
      case FLOAT:
      case DOUBLE:
        column_size = 10;
        break;
      case BOOL:
        column_size = 1;
        break;
      case VARCHAR:
      case CHAR:
        column_size = col->length;
        break;
      case BLOB:
        column_size = 5000; // todo set it different subtype
        break;
      case COLUMN_MAX:
      case GENERATED:
        throw std::runtime_error("unhandled " + col_type_to_string(col->type_) +
                                 " at line " + std::to_string(__LINE__));
      }
      if (column_size > current_size) {
        actual_size += current_size;
        gen_sql += "SUBSTRING(" + col->name_ + ",1," +
                   std::to_string(current_size) + "),";
      } else {
        actual_size += column_size;
        gen_sql += col->name_ + ",";
      }
    }
    gen_sql.pop_back();
    str = " " + col_type_to_string(g_type);
    if (g_type == VARCHAR || g_type == CHAR)
      str += "(" + std::to_string(actual_size) + ")";
    str += " GENERATED ALWAYS AS (CONCAT(";
    str += gen_sql;
    str += ")";
    length = actual_size;
  } else {
    throw std::runtime_error("unhandled " + col_type_to_string(g_type) +
                             " at line " + std::to_string(__LINE__));
  }
  str += ")";

  if (rand_int(2) == 1 || compressed)
    str += " STORED";
}

template <typename Writer> void Column::Serialize(Writer &writer) const {
  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String("type");
  std::string typ = col_type_to_string(type_);
  writer.String(typ.c_str(), static_cast<SizeType>(typ.length()));
  writer.String("null");
  writer.Bool(null);
  writer.String("primary_key");
  writer.Bool(primary_key);
  writer.String("compressed");
  writer.Bool(compressed);
  writer.String("auto_increment");
  writer.Bool(auto_increment);
  writer.String("lenght");
  writer.Int(length);
}

/* add sub_type metadata */
template <typename Writer> void Blob_Column::Serialize(Writer &writer) const {
  writer.String("sub_type");
  writer.String(sub_type.c_str(), static_cast<SizeType>(sub_type.length()));
}

/* add sub_type and clause in metadata */
template <typename Writer>
void Generated_Column::Serialize(Writer &writer) const {
  writer.String("sub_type");
  auto type = col_type_to_string(g_type);
  writer.String(type.c_str(), static_cast<SizeType>(type.length()));
  writer.String("clause");
  writer.String(str.c_str(), static_cast<SizeType>(str.length()));
}

template <typename Writer> void Ind_col::Serialize(Writer &writer) const {
  writer.StartObject();
  writer.String("name");
  auto &name = column->name_;
  writer.String(name.c_str(), static_cast<SizeType>(name.length()));
  writer.String("desc");
  writer.Bool(desc);
  writer.String("length");
  writer.Uint(length);
  writer.EndObject();
}

template <typename Writer> void Index::Serialize(Writer &writer) const {
  writer.StartObject();
  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String(("index_columns"));
  writer.StartArray();
  for (auto ic : *columns_)
    ic->Serialize(writer);
  writer.EndArray();
  writer.EndObject();
}

Index::~Index() {
  for (auto id_col : *columns_) {
    delete id_col;
  }
  delete columns_;
}

template <typename Writer> void Table::Serialize(Writer &writer) const {
  writer.StartObject();

  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String("type");
  writer.String(get_type().c_str(), static_cast<SizeType>(get_type().length()));

  if (type == PARTITION) {
    auto part_table = static_cast<const Partition *>(this);
    writer.String("part_type");
    std::string part_type = part_table->get_part_type();
    writer.String(part_type.c_str(), static_cast<SizeType>(part_type.length()));
    writer.String("number_of_part");
    writer.Int(part_table->number_of_part);
    if (part_table->part_type == Partition::RANGE) {
      writer.String("part_range");
      writer.StartArray();
      for (auto par : part_table->positions) {
        writer.StartArray();
        writer.String(par.name.c_str(),
                      static_cast<SizeType>(par.name.length()));
        writer.Int(par.range);
        writer.EndArray();
      }
      writer.EndArray();
    } else if (part_table->part_type == Partition::LIST) {

      writer.String("part_list");
      writer.StartArray();
      for (auto list : part_table->lists) {
        writer.StartArray();
        writer.String(list.name.c_str(),
                      static_cast<SizeType>(list.name.length()));
        writer.StartArray();
        for (auto i : list.list)
          writer.Int(i);
        writer.EndArray();
        writer.EndArray();
      };
      writer.EndArray();
    }
  }

  writer.String("engine");
  if (!engine.empty())
    writer.String(engine.c_str(), static_cast<SizeType>(engine.length()));
  else
    writer.String("default");

  writer.String("row_format");
  if (!row_format.empty())
    writer.String(row_format.c_str(),
                  static_cast<SizeType>(row_format.length()));
  else
    writer.String("default");

  writer.String("tablespace");
  if (!tablespace.empty())
    writer.String(tablespace.c_str(),
                  static_cast<SizeType>(tablespace.length()));
  else
    writer.String("file_per_table");

  writer.String("encryption");
  writer.String(encryption.c_str(), static_cast<SizeType>(encryption.length()));

  writer.String("compression");
  writer.String(compression.c_str(),
                static_cast<SizeType>(compression.length()));

  writer.String("key_block_size");
  writer.Int(key_block_size);

  writer.String(("columns"));
  writer.StartArray();

  /* write all colummns */
  for (auto &col : *columns_) {
    writer.StartObject();
    col->Serialize(writer);
    if (col->type_ == Column::GENERATED) {
      static_cast<Generated_Column *>(col)->Serialize(writer);
    } else if (col->type_ == Column::BLOB) {
      static_cast<Blob_Column *>(col)->Serialize(writer);
    }
    writer.EndObject();
  }

  writer.EndArray();

  writer.String(("indexes"));
  writer.StartArray();
  for (auto *ind : *indexes_)
    ind->Serialize(writer);
  writer.EndArray();
  writer.EndObject();
}

Ind_col::Ind_col(Column *c, bool d) : column(c), desc(d) {}

Index::Index(std::string n) : name_(n), columns_() {
  columns_ = new std::vector<Ind_col *>;
}

void Index::AddInternalColumn(Ind_col *column) { columns_->push_back(column); }

/* index definition */
std::string Index::definition() {
  std::string def;
  def += "INDEX " + name_ + "(";
  for (auto idc : *columns_) {
    def += idc->column->name_;

    /* blob columns should have prefix length */
    if (idc->column->type_ == Column::BLOB ||
        (idc->column->type_ == Column::GENERATED &&
         static_cast<Generated_Column *>(idc->column)->generate_type() ==
             Column::BLOB))
      def += "(" + std::to_string(rand_int(g_max_columns_length, 1)) + ")";

    def += (idc->desc ? " DESC" : (rand_int(3) ? "" : " ASC"));
    def += ", ";
  }
  def.erase(def.length() - 2);
  def += ") ";
  return def;
}

bool Table::load(Thd1 *thd) {
  thd->ddl_query = true;
  if (!execute_sql(definition(false), thd)) {
    thd->thread_log << "Failed to create table " << name_ << std::endl;
    run_query_failed = true;
    return false;
  }

  /* load default data in table */
  if (!options->at(Option::JUST_LOAD_DDL)->getBool()) {
    thd->ddl_query = false;
    size_t number_of_records =
        options->at(Option::EXACT_INITIAL_RECORDS)->getBool()
            ? options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()
            : rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt());

    if (!InsertBulkRecord(thd, number_of_records))
      return false;
  }

  thd->ddl_query = true;
  if (!load_secondary_indexes(thd)) {
    return false;
  }

  if (run_query_failed) {
    thd->thread_log << "some other thread failed, Exiting. Please check logs "
                    << std::endl;
    return false;
  }

  return true;
}

Table::Table(std::string n) : name_(n), indexes_() {
  columns_ = new std::vector<Column *>;
  indexes_ = new std::vector<Index *>;
}

bool Table::load_secondary_indexes(Thd1 *thd) {

  if (indexes_->size() == 0)
    return true;

  for (auto id : *indexes_) {
    if (id == indexes_->at(auto_inc_index))
      continue;
    std::string sql = "ALTER TABLE " + name_ + " ADD " + id->definition();
    if (!execute_sql(sql, thd)) {
      thd->thread_log << "Failed to add index " << id->name_ << " on " << name_
                      << std::endl;
      run_query_failed = true;
      return false;
    }
  }

  return true;
}

/* Constructor used by load_metadata */
Partition::Partition(std::string n, std::string part_type_, int number_of_part_)
    : Table(n), number_of_part(number_of_part_) {
  set_part_type(part_type_);
}

/* Constructor used by new Partiton table */
Partition::Partition(std::string n) : Table(n) {

  part_type = supported[rand_int(supported.size() - 1)];

  number_of_part = rand_int(options->at(Option::MAX_PARTITIONS)->getInt(), 2);

  /* randomly pick ranges for partition */
  if (part_type == RANGE) {
    auto number_of_records =
        options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt();
    for (int i = 0; i < number_of_part; i++) {
      positions.emplace_back("p",
                             rand_int(g_integer_range * number_of_records));
    }
    std::sort(positions.begin(), positions.end(), Partition::compareRange);
    for (int i = 0; i < number_of_part; i++) {
      positions.at(i).name = "p" + std::to_string(i);
    }
    // adjust the range so we don't have overlapping ranges
    for (int i = 1; i < number_of_part; i++) {
      if (positions.at(i).range == positions.at(i - 1).range)
        for (int j = i; j < number_of_part; j++)
          positions.at(j).range++;
    }

  } else if (part_type == LIST) {
    auto number_of_records =
        rand_int(maximum_records_in_each_parititon_list * number_of_part,
                 number_of_part);

    /* temporary vector to store all number_of_records */
    for (int i = 0; i < number_of_records; i++)
      total_left_list.push_back(i);

    for (int i = 0; i < number_of_part; i++) {
      lists.emplace_back("p" + std::to_string(i));
      auto number_of_records_in_partition =
          rand_int(number_of_records) / number_of_part;

      if (number_of_records_in_partition == 0)
        number_of_records_in_partition = 1;

      for (int j = 0; j < number_of_records_in_partition; j++) {
        auto curr = rand_int(total_left_list.size() - 1);
        lists.at(i).list.push_back(total_left_list.at(curr));
        total_left_list.erase(total_left_list.begin() + curr);
      }
    }
  }
}

void Table::DropCreate(Thd1 *thd) {
  execute_sql("DROP TABLE " + name_, thd);
  std::string def = definition();
  if (!execute_sql(def, thd) && tablespace.size() > 0) {
    std::string tbs = " TABLESPACE=" + tablespace + "_rename";

    auto no_encryption = opt_bool(NO_ENCRYPTION);

    std::string encrypt_sql = " ENCRYPTION = " + encryption;

    /* If tablespace is rename or encrypted, or tablespace rename/encrypted */
    if (!execute_sql(def + tbs, thd))
      if (!no_encryption && (execute_sql(def + encrypt_sql, thd) ||
                             execute_sql(def + encrypt_sql + tbs, thd))) {
        table_mutex.lock();
        if (encryption.compare("Y") == 0)
          encryption = 'N';
        else if (encryption.compare("N") == 0)
          encryption = 'Y';
        table_mutex.unlock();
      }
  }
}

void Table::Optimize(Thd1 *thd) {
  if (type == PARTITION && rand_int(4) == 1) {
    table_mutex.lock();
    int partition =
        rand_int(static_cast<Partition *>(this)->number_of_part - 1);
    table_mutex.unlock();
    execute_sql("ALTER TABLE " + name_ + " OPTIMIZE PARTITION p" +
                    std::to_string(partition),
                thd);
  } else
    execute_sql("OPTIMIZE TABLE " + name_, thd);
}

void Table::Check(Thd1 *thd) {
  if (type == PARTITION && rand_int(4) == 1) {
    table_mutex.lock();
    int partition =
        rand_int(static_cast<Partition *>(this)->number_of_part - 1);
    table_mutex.unlock();
    get_check_result("ALTER TABLE " + name_ + " CHECK PARTITION p" +
                         std::to_string(partition),
                     thd);
  } else
    get_check_result("CHECK TABLE " + name_, thd);
}

void Table::Analyze(Thd1 *thd) {
  if (type == PARTITION && rand_int(4) == 1) {
    table_mutex.lock();
    int partition =
        rand_int(static_cast<Partition *>(this)->number_of_part - 1);
    table_mutex.unlock();
    execute_sql("ALTER TABLE " + name_ + " ANALYZE PARTITION p" +
                    std::to_string(partition),
                thd);
  } else
    execute_sql("ANALYZE TABLE " + name_, thd);
}

void Table::Truncate(Thd1 *thd) {
  /* 99% truncate the some partition */
  if (type == PARTITION && rand_int(100) > 1) {
    table_mutex.lock();
    std::string part_name;
    auto part_table = static_cast<Partition *>(this);
    assert(part_table->number_of_part > 0);
    if (part_table->part_type == Partition::HASH ||
        part_table->part_type == Partition::KEY) {
      part_name = std::to_string(rand_int(part_table->number_of_part - 1));
    } else if (part_table->part_type == Partition::RANGE) {
      part_name =
          part_table->positions.at(rand_int(part_table->positions.size() - 1))
              .name;
    } else if (part_table->part_type == Partition::LIST) {
      part_name =
          part_table->lists.at(rand_int(part_table->lists.size() - 1)).name;
    }
    table_mutex.unlock();
    execute_sql("ALTER TABLE " + name_ + pick_algorithm_lock() +
                    ", TRUNCATE PARTITION " + part_name,
                thd);
  } else {
    execute_sql("TRUNCATE TABLE " + name_, thd);
  }
}

/* add or drop average 10% of max partitions */
void Partition::AddDrop(Thd1 *thd) {
  if (part_type == KEY || part_type == HASH) {
    int new_partition =
        rand_int(options->at(Option::MAX_PARTITIONS)->getInt()) / 10;
    if (new_partition == 0)
      new_partition = 1;

    if (rand_int(1) == 0) {
      if (execute_sql("ALTER TABLE " + name_ + " ADD PARTITION PARTITIONS " +
                          std::to_string(new_partition),
                      thd)) {
        table_mutex.lock();
        number_of_part += new_partition;
        table_mutex.unlock();
      }
    } else {
      if (execute_sql("ALTER TABLE " + name_ + pick_algorithm_lock() +
                          ", COALESCE PARTITION " +
                          std::to_string(new_partition),
                      thd)) {
        table_mutex.lock();
        number_of_part -= new_partition;
        table_mutex.unlock();
      }
    }
  } else if (part_type == RANGE) {
    /* drop partition, else add partition */
    if (rand_int(1) == 1) {
      table_mutex.lock();
      if (positions.size()) {
        auto par = positions.at(rand_int(positions.size() - 1));
        auto part_name = par.name;
        table_mutex.unlock();
        if (execute_sql("ALTER TABLE " + name_ + pick_algorithm_lock() +
                            ", DROP PARTITION " + part_name,
                        thd)) {
          table_mutex.lock();
          number_of_part--;
          for (auto i = positions.begin(); i != positions.end(); i++) {
            if (i->name.compare(part_name) == 0) {
              positions.erase(i);
              break;
            }
          }
          table_mutex.unlock();
        }
      } else
        table_mutex.unlock();
    } else {
      /* add partition */
      table_mutex.lock();
      int first;
      int second;
      std::string par_name;
      if (positions.size()) {
        if (positions.size() > 1) {
          size_t pst = rand_int(positions.size() - 1, 1);

          if (positions.at(pst).range - positions.at(pst - 1).range <= 2) {
            table_mutex.unlock();
            return;
          }
          auto par = positions.at(pst);
          auto prev_par = positions.at(pst - 1);
          first = rand_int(par.range, prev_par.range);
          second = par.range;
          par_name = par.name;
        } else {
          auto par = positions.at(0);
          first = rand_int(par.range);
          second = par.range;
          par_name = par.name;
        }

        std::string sql = "ALTER TABLE " + name_ + " REORGANIZE PARTITION " +
                          par_name + " INTO ( PARTITION " + par_name +
                          "a VALUES LESS THAN " + "(" + std::to_string(first) +
                          "), PARTITION " + par_name + "b VALUES LESS THAN (" +
                          std::to_string(second) + "))";
        table_mutex.unlock();

        if (execute_sql(sql, thd)) {
          table_mutex.lock();
          for (auto i = positions.begin(); i != positions.end(); i++) {
            if (i->name.compare(par_name) == 0) {
              positions.erase(i);
              break;
            }
          }
          positions.emplace_back(par_name + "a", first);
          positions.emplace_back(par_name + "b", second);
          std::sort(positions.begin(), positions.end(),
                    Partition::compareRange);
          number_of_part++;
          table_mutex.unlock();
        }
      } else
        table_mutex.unlock();
    }
  } else if (part_type == LIST) {

    /* drop partition or add partition */
    if (rand_int(1) == 0) {
      table_mutex.lock();
      assert(lists.size() > 0);
      auto par = lists.at(rand_int(lists.size() - 1));
      auto part_name = par.name;
      table_mutex.unlock();
      if (execute_sql("ALTER TABLE " + name_ + pick_algorithm_lock() +
                          ", DROP PARTITION " + part_name,
                      thd)) {
        table_mutex.lock();
        number_of_part--;
        for (auto i = lists.begin(); i != lists.end(); i++) {
          if (i->name.compare(part_name) == 0) {
            for (auto j : i->list)
              total_left_list.push_back(j);
            lists.erase(i);
            break;
          }
        }
        table_mutex.unlock();
      }

    } else {
      /* add partition */
      size_t number_of_records_in_partition =
          rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()) /
          rand_int(options->at(Option::MAX_PARTITIONS)->getInt(), 1);

      if (number_of_records_in_partition == 0)
        number_of_records_in_partition = 1;
      table_mutex.lock();
      if (number_of_records_in_partition > total_left_list.size()) {
        table_mutex.unlock();
        return;
      } else {
        std::vector<int> temp_list;
        while (temp_list.size() != number_of_records_in_partition) {
          auto curr = rand_int(total_left_list.size() - 1);
          int flag = false;
          for (auto l : temp_list) {
            if (l == curr)
              flag = true;
          }
          if (flag == false)
            temp_list.push_back(curr);
        }
        table_mutex.unlock();
        std::string new_part_name = "p" + std::to_string(rand_int(1000, 100));
        std::string sql = "ALTER TABLE " + name_ +
                          " ADD PARTITION (PARTITION " + new_part_name +
                          " VALUES IN (";
        for (size_t i = 0; i < temp_list.size(); i++) {
          sql += " " + std::to_string(temp_list.at(i));
          if (i != temp_list.size() - 1)
            sql += ",";
        }
        sql += "))";
        if (execute_sql(sql, thd)) {
          table_mutex.lock();
          number_of_part++;
          lists.emplace_back(new_part_name);
          for (auto l : temp_list) {
            lists.at(lists.size() - 1).list.push_back(l);
            total_left_list.erase(
                std::remove(total_left_list.begin(), total_left_list.end(), l),
                total_left_list.end());
          }
          table_mutex.unlock();
        }
      }
    }
  }
}

Table::~Table() {
  for (auto ind : *indexes_)
    delete ind;
  for (auto col : *columns_) {
    col->mutex.lock();
    delete col;
  }
  delete columns_;
  delete indexes_;
}

/* create default column */
void Table::CreateDefaultColumn() {
  auto no_auto_inc = opt_bool(NO_AUTO_INC);
  bool has_auto_increment = false;

  /* if table is partition add new column */
  if (type == PARTITION) {
    std::string name = "p_col";
    Column::COLUMN_TYPES type;
    if (static_cast<Partition *>(this)->part_type == Partition::LIST)
      type = Column::INTEGER;
    else
      type = Column::INT;
    auto col = new Column{name, this, type};
    AddInternalColumn(col);
  }

  /* create normal column */
  static auto max_col = opt_int(COLUMNS);

  auto max_columns = rand_int(max_col, 1);

  for (int i = 0; i < max_columns; i++) {
    std::string name;
    Column::COLUMN_TYPES type;
    Column *col;
    /*  if we need to create primary column */

    /* First column can be primary */
    if (i == 0 && rand_int(100) <= options->at(Option::PRIMARY_KEY)->getInt()) {
      type = Column::INT;
      name = "pkey";
      col = new Column{name, this, type};
      col->primary_key = true;
      if (!no_auto_inc && rand_int(3) < 3) {
        /* 75% of primary key tables are autoinc */
        if (this->type == PARTITION && rand_int(3) == 1)
          columns_->at(0)->auto_increment = true;
        else
          col->auto_increment = true;
        has_auto_increment = true;
      }
    } else {
      name = std::to_string(i);
      Column::COLUMN_TYPES col_type = Column::COLUMN_MAX;
      static auto no_virtual_col = opt_bool(NO_VIRTUAL_COLUMNS);
      static auto no_blob_col = opt_bool(NO_BLOB);

      /* loop untill we select some column */
      while (col_type == Column::COLUMN_MAX) {

        /* columns are 6:2:2:4:2:2:1 INT:FLOAT:DOUBLE:VARCHAR:CHAR:BLOB:BOOL */
        auto prob = rand_int(19);

        /* intial columns can't be generated columns. also 50% of tables last
         * columns are virtuals */
        if (!no_virtual_col && i >= .8 * max_columns && rand_int(1) == 1)
          col_type = Column::GENERATED;
        else if (prob < 5)
          col_type = Column::INT;
        else if (prob < 6)
          col_type = Column::INTEGER;
        else if (prob < 8)
          col_type = Column::FLOAT;
        else if (prob < 10)
          col_type = Column::DOUBLE;
        else if (prob < 14)
          col_type = Column::VARCHAR;
        else if (prob < 16)
          col_type = Column::CHAR;
        else if (!no_blob_col && prob < 18)
          col_type = Column::BLOB;
        else if (prob == 19)
          col_type = Column::BOOL;
      }

      if (col_type == Column::GENERATED)
        col = new Generated_Column(name, this);
      else if (col_type == Column::BLOB)
        col = new Blob_Column(name, this);
      else
        col = new Column(name, this, col_type);

      /* 25% column can have auto_inc */
      if (col->type_ == Column::INT && !no_auto_inc &&
          has_auto_increment == false && rand_int(100) > 25) {
        col->auto_increment = true;
        has_auto_increment = true;
      }
    }
    AddInternalColumn(col);
  }
}

/* create default indexes */
void Table::CreateDefaultIndex() {

  int auto_inc_pos = -1; // auto_inc_column_position

  static size_t max_indexes = opt_int(INDEXES);

  if (max_indexes == 0)
    return;

  /* if table have few column, decrease number of indexes */
  size_t indexes = rand_int(
      columns_->size() < max_indexes ? columns_->size() : max_indexes, 1);

  /* for auto-inc columns handling, we need to add auto_inc as first column */
  for (size_t i = 0; i < columns_->size(); i++) {
    if (columns_->at(i)->auto_increment) {
      auto_inc_pos = i;
    }
  }

  /*which column will have auto_inc */
  auto_inc_index = rand_int(indexes - 1, 0);

  for (size_t i = 0; i < indexes; i++) {
    Index *id = new Index(name_ + "i" + std::to_string(i));

    static size_t max_columns = opt_int(INDEX_COLUMNS);

    int number_of_compressed = 0;

    for (auto column : *columns_)
      if (column->compressed)
        number_of_compressed++;

    size_t number_of_columns = columns_->size() - number_of_compressed;

    /* only compressed columns */
    if (number_of_columns == 0)
      return;

    number_of_columns = rand_int(
        (max_columns < number_of_columns ? max_columns : number_of_columns), 1);

    std::vector<int> col_pos; // position of columns

    /* pick some columns */
    while (col_pos.size() < number_of_columns) {
      int current = rand_int(columns_->size() - 1);
      if (columns_->at(current)->compressed)
        continue;
      /* auto-inc column should be first column in auto_inc_index */
      if (auto_inc_pos != -1 && i == auto_inc_index && col_pos.size() == 0)
        col_pos.push_back(auto_inc_pos);
      else {
        bool already_added = false;
        for (auto id : col_pos) {
          if (id == current)
            already_added = true;
        }
        if (!already_added)
          col_pos.push_back(current);
      }
    } // while

    for (auto pos : col_pos) {
      auto col = columns_->at(pos);
      static bool no_desc_support = opt_bool(NO_DESC_INDEX);
      bool column_desc = false;
      if (!no_desc_support) {
        column_desc = rand_int(100) < DESC_INDEXES_IN_COLUMN
                          ? true
                          : false; // 33 % are desc //
      }
      id->AddInternalColumn(
          new Ind_col(col, column_desc)); // desc is set as true
    }
    AddInternalIndex(id);
  }
}

/* Create new table and pick some attributes */
Table *Table::table_id(TABLE_TYPES type, int id) {
  Table *table;
  std::string name = "tt_" + std::to_string(id);
  switch (type) {
  case PARTITION:
    table = new Partition(name + partition_string);
    break;
  case NORMAL:
    table = new Table(name);
    break;
  case TEMPORARY:
    table = new Temporary_table(name + "_t");
    break;
  default:
    throw std::runtime_error("Unhandle Table type");
    break;
  }

  table->type = type;

  static auto no_encryption = opt_bool(NO_ENCRYPTION);

  /* temporary table on 8.0 can't have key block size */
  if (!(server_version() >= 80000 && type == TEMPORARY)) {
    if (g_key_block_size.size() > 0)
      table->key_block_size =
          g_key_block_size[rand_int(g_key_block_size.size() - 1)];

    if (table->key_block_size > 0 && rand_int(2) == 0) {
      table->row_format = "COMPRESSED";
    }

    if (table->key_block_size == 0 && g_row_format.size() > 0)
      table->row_format = g_row_format[rand_int(g_row_format.size() - 1)];
  }

  /* with more number of tablespace there are more chances to have table in
   * tablespaces */
  static int tbs_count = opt_int(NUMBER_OF_GENERAL_TABLESPACE);

  /* partition and temporary tables don't have tablespaces */
  if (table->type == PARTITION && !no_encryption) {
    table->encryption = g_encryption[rand_int(g_encryption.size() - 1)];
  } else if (table->type != TEMPORARY && !no_encryption) {
    int rand_index = rand_int(g_encryption.size() - 1);
    if (g_encryption.at(rand_index) == "Y" ||
        g_encryption.at(rand_index) == "N") {
      if (g_tablespace.size() > 0 && rand_int(tbs_count) != 0) {
        table->tablespace = g_tablespace[rand_int(g_tablespace.size() - 1)];
        if (table->tablespace.substr(table->tablespace.size() - 2, 2)
                .compare("_e") == 0)
          table->encryption = "Y";
        table->row_format.clear();
        if (g_innodb_page_size > INNODB_16K_PAGE_SIZE ||
            table->tablespace.compare("innodb_system") == 0 ||
            stoi(table->tablespace.substr(3, 2)) == g_innodb_page_size)
          table->key_block_size = 0;
        else
          table->key_block_size = std::stoi(table->tablespace.substr(3, 2));
      }
    } else
      table->encryption = g_encryption.at(rand_index);
  }

  if (encrypted_temp_tables && table->type == TEMPORARY)
    table->encryption = 'Y';

  if (encrypted_sys_tablelspaces &&
      table->tablespace.compare("innodb_system") == 0) {
    table->encryption = 'Y';
  }

  /* 25 % tables are compress */
  if (table->type != TEMPORARY && table->tablespace.empty() and
      rand_int(3) == 1 && g_compression.size() > 0) {
    table->compression = g_compression[rand_int(g_compression.size() - 1)];
    table->row_format.clear();
    table->key_block_size = 0;
  }

  static auto engine = options->at(Option::ENGINE)->getString();
  table->engine = engine;

  /* If indexes are disabled, also disable auto_inc */
  if (!options->at(Option::INDEXES)->getInt())
    options->at(Option::NO_AUTO_INC)->setBool(true);

  table->CreateDefaultColumn();
  table->CreateDefaultIndex();

  return table;
}

/* prepare table definition */
std::string Table::definition(bool with_index) {
  std::string def = "CREATE";
  if (type == TEMPORARY)
    def += " TEMPORARY";
  def += " TABLE " + name_ + " (";

  if (columns_->size() == 0)
    throw std::runtime_error("no column in table " + name_);

  /* add columns */
  for (auto col : *columns_) {
    def += col->definition() + ", ";
  }

  /* if column has primary key */
  for (auto col : *columns_) {
    if (col->primary_key) {
      def += " PRIMARY KEY(";
      if (type == PARTITION) {
        if (rand_int(1) == 0)
          def += col->name_ + ", ip_col";
        else
          def += "ip_col, " + col->name_;
      } else
        def += col->name_;
      def += +"), ";
    }
  }

  if (with_index) {
    if (indexes_->size() > 0) {
      for (auto id : *indexes_) {
        def += id->definition() + ", ";
      }
    }
  } else {
    /* only load autoinc */
    if (indexes_->size() > 0) {
      def += indexes_->at(auto_inc_index)->definition() + ", ";
    }
  }

  def.erase(def.length() - 2);

  def += " )";
  static auto no_encryption = opt_bool(NO_ENCRYPTION);
  bool keyring_key_encrypt_flag = 0;

  if (!no_encryption && type != TEMPORARY) {
    if (encryption == "Y" || encryption == "N")
      def += " ENCRYPTION='" + encryption + "'";
    else if (encryption == "KEYRING") {
      keyring_key_encrypt_flag = 1;
      switch (rand_int(2)) {
      case 0:
        def += "ENCRYPTION='KEYRING'";
        break;
      case 1:
        def += " ENCRYPTION_KEY_ID=" + std::to_string(rand_int(9));
        break;
      case 2:
        def += " ENCRYPTION='KEYRING' ENCRYPTION_KEY_ID=" +
               std::to_string(rand_int(9));
        break;
      }
    }
  }

  if (!compression.empty())
    def += " COMPRESSION='" + compression + "'";

  if (!tablespace.empty() && !keyring_key_encrypt_flag)
    def += " TABLESPACE=" + tablespace;

  if (key_block_size > 1)
    def += " KEY_BLOCK_SIZE=" + std::to_string(key_block_size);

  if (row_format.size() > 0)
    def += " ROW_FORMAT=" + row_format;

  if (!engine.empty())
    def += " ENGINE=" + engine;

  if (type == PARTITION) {
    auto par = static_cast<Partition *>(this);
    def += " PARTITION BY " + par->get_part_type() + " (ip_col)";
    switch (par->part_type) {
    case Partition::HASH:
    case Partition::KEY:
      def += " PARTITIONS " + std::to_string(par->number_of_part);
      break;
    case Partition::RANGE:
      def += "(";
      for (size_t i = 0; i < par->positions.size(); i++) {
        std::string range;
        if (i == par->positions.size() - 1)
          range = "MAXVALUE";
        else
          range = std::to_string(par->positions[i].range);

        def += " PARTITION p" + std::to_string(i) + " VALUES LESS THAN (" +
               range + ")";

        if (i == par->positions.size() - 1)
          def += ")";
        else
          def += ",";
      }
      break;
    case Partition::LIST:
      def += "(";
      for (size_t i = 0; i < par->lists.size(); i++) {
        def += " PARTITION " + par->lists.at(i).name + " VALUES IN (";
        auto list = par->lists.at(i).list;
        for (size_t j = 0; j < list.size(); j++) {
          def += std::to_string(list.at(j));
          if (j == list.size() - 1)
            def += ")";
          else
            def += ",";
        }
        if (i == par->lists.size() - 1)
          def += ")";
        else
          def += ",";
      }
      break;
    }
  }
  return def;
}

/* create default table includes all tables*/
void generate_metadata_for_tables() {
  auto tables = opt_int(TABLES);

  auto only_temporary_tables = opt_bool(ONLY_TEMPORARY);

  if (!only_temporary_tables) {
    for (int i = 1; i <= tables; i++) {
      if (!options->at(Option::ONLY_PARTITION)->getBool())
        all_tables->push_back(Table::table_id(Table::NORMAL, i));
      if (!options->at(Option::NO_PARTITION)->getBool())
        all_tables->push_back(Table::table_id(Table::PARTITION, i));
    }
  }
}

bool execute_sql(const std::string &sql, Thd1 *thd) {
  auto query = sql.c_str();
  static auto log_all = opt_bool(LOG_ALL_QUERIES);
  static auto log_failed = opt_bool(LOG_FAILED_QUERIES);
  static auto log_success = opt_bool(LOG_SUCCEDED_QUERIES);
  static auto log_query_duration = opt_bool(LOG_QUERY_DURATION);
  static auto log_client_output = opt_bool(LOG_CLIENT_OUTPUT);
  static auto log_query_numbers = opt_bool(LOG_QUERY_NUMBERS);
  std::chrono::system_clock::time_point begin, end;

  if (log_query_duration) {
    begin = std::chrono::system_clock::now();
  }

  auto res = mysql_real_query(thd->conn, query, strlen(query));

  if (log_query_duration) {
    end = std::chrono::system_clock::now();

    /* elpased time in micro-seconds */
    auto te_start = std::chrono::duration_cast<std::chrono::microseconds>(
        begin - start_time);
    auto te_query =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
    auto in_time_t = std::chrono::system_clock::to_time_t(begin);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%X");

    thd->thread_log << ss.str() << " " << te_start.count() << "=>"
                    << te_query.count() << "ms ";
  }
  thd->performed_queries_total++;

  if (res == 1) { // query failed
    thd->failed_queries_total++;
    thd->max_con_fail_count++;
    if (log_all || log_failed) {
      thd->thread_log << " F " << sql << std::endl;
      thd->thread_log << "Error " << mysql_error(thd->conn) << std::endl;
    }
    if (mysql_errno(thd->conn) == CR_SERVER_GONE_ERROR ||
        mysql_errno(thd->conn) == CR_SERVER_LOST) {
      thd->thread_log << "server gone, while processing " + sql;
      run_query_failed = true;
    }
  } else {
    thd->max_con_fail_count = 0;
    thd->success = true;
    auto result = mysql_store_result(thd->conn);
    thd->result = std::shared_ptr<MYSQL_RES>(result, [](MYSQL_RES *r) {
      if (r)
        mysql_free_result(r);
    });

    if (log_client_output) {
      if (thd->result != nullptr) {
        unsigned int i, num_fields;

        num_fields = mysql_num_fields(thd->result.get());
        while (auto row = mysql_fetch_row_safe(thd)) {
          for (i = 0; i < num_fields; i++) {
            if (row[i]) {
              if (strlen(row[i]) == 0) {
                thd->client_log << "EMPTY"
                                << "#";
              } else {
                thd->client_log << row[i] << "#";
              }
            } else {
              thd->client_log << "#NO DATA"
                              << "#";
            }
          }
          if (log_query_numbers) {
            thd->client_log << ++thd->query_number;
          }
          thd->client_log << '\n';
        }
      }
    }

    /* log successful query */
    if (log_all || log_success) {
      thd->thread_log << " S " << sql;
      int number;
      if (thd->result == nullptr)
        number = mysql_affected_rows(thd->conn);
      else
        number = mysql_num_rows(thd->result.get());
      thd->thread_log << " rows:" << number << std::endl;
    }
  }

  if (thd->ddl_query) {
    ddl_logs_write.lock();
    thd->ddl_logs << thd->thread_id << " " << sql << " "
                  << mysql_error(thd->conn) << std::endl;
    ddl_logs_write.unlock();
  }

  return (res == 0 ? 1 : 0);
}

void Table::SetEncryption(Thd1 *thd) {
  std::string sql = "ALTER TABLE " + name_ + " ENCRYPTION = '";
  std::string enc = g_encryption[rand_int(g_encryption.size() - 1)];
  sql += enc + "'";
  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    encryption = enc;
    table_mutex.unlock();
  }
}

// todo pick relevant table //
void Table::SetTableCompression(Thd1 *thd) {
  std::string sql = "ALTER TABLE " + name_ + " COMPRESSION= '";
  std::string comp = g_compression[rand_int(g_compression.size() - 1)];
  sql += comp + "'";
  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    compression = comp;
    table_mutex.unlock();
  }
}

// todo pick relevent table//
void Table::ModifyColumn(Thd1 *thd) {
  std::string sql = "ALTER TABLE " + name_ + " MODIFY COLUMN ";
  Column *col = nullptr;
  /* store old value */
  int length = 0;
  std::string default_value;
  bool auto_increment = false;
  bool compressed = false; // percona type compressed

  // try maximum 50 times to get a valid column
  int i = 0;
  while (i < 50 && col == nullptr) {
    auto col1 = columns_->at(rand_int(columns_->size() - 1));
    switch (col1->type_) {
    case Column::BLOB:
    case Column::GENERATED:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::INT:
    case Column::INTEGER:
      col = col1;
      length = col->length;
      auto_increment = col->auto_increment;
      compressed = col->compressed;
      col->mutex.lock(); // lock column so no one can modify it //
      break;
      /* todo no support for BOOL INT so far */
    case Column::BOOL:
    case Column::COLUMN_MAX:
      break;
    }
    i++;
  }

  /* could not find a valid column to process */
  if (col == nullptr)
    return;

  if (col->length != 0)
    col->length = rand_int(g_max_columns_length, 0);

  if (col->auto_increment == true and rand_int(5) == 0)
    col->auto_increment = false;

  if (col->compressed == true and rand_int(4) == 0)
    col->compressed = false;
  else if (options->at(Option::NO_COLUMN_COMPRESSION)->getBool() == false &&
           (col->type_ == Column::BLOB || col->type_ == Column::GENERATED ||
            col->type_ == Column::VARCHAR))
    col->compressed = true;

  sql += " " + col->definition() + "," + pick_algorithm_lock();

  /* if not successful rollback */
  if (!execute_sql(sql, thd)) {
    col->length = length;
    col->auto_increment = auto_increment;
    col->compressed = compressed;
  }

  col->mutex.unlock();
}

/* alter table drop column */
void Table::DropColumn(Thd1 *thd) {
  table_mutex.lock();

  /* do not drop last column */
  if (columns_->size() == 1) {
    table_mutex.unlock();
    return;
  }
  auto ps = rand_int(columns_->size() - 1); // position

  auto name = columns_->at(ps)->name_;

  if (rand_int(100, 1) <= options->at(Option::PRIMARY_KEY)->getInt() &&
      name.find("pkey") != std::string::npos) {
    table_mutex.unlock();
    return;
  }

  std::string sql = "ALTER TABLE " + name_ + " DROP COLUMN " + name + ",";

  sql += pick_algorithm_lock();
  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();

    std::vector<int> indexes_to_drop;
    for (auto id = indexes_->begin(); id != indexes_->end(); id++) {
      auto index = *id;

      for (auto id_col = index->columns_->begin();
           id_col != index->columns_->end(); id_col++) {
        auto ic = *id_col;
        if (ic->column->name_.compare(name) == 0) {
          if (index->columns_->size() == 1) {
            delete index;
            indexes_to_drop.push_back(id - indexes_->begin());
          } else {
            delete ic;
            index->columns_->erase(id_col);
          }
          break;
        }
      }
    }
    std::sort(indexes_to_drop.begin(), indexes_to_drop.end(),
              std::greater<int>());

    for (auto &i : indexes_to_drop) {
      indexes_->at(i) = indexes_->back();
      indexes_->pop_back();
    }
    // table->indexes_->erase(id);

    for (auto pos = columns_->begin(); pos != columns_->end(); pos++) {
      auto col = *pos;
      if (col->name_.compare(name) == 0) {
        col->mutex.lock();
        delete col;
        columns_->erase(pos);
        break;
      }
    }
    table_mutex.unlock();
  }
}

/* alter table add random column */
void Table::AddColumn(Thd1 *thd) {

  static auto no_use_virtual = opt_bool(NO_VIRTUAL_COLUMNS);
  static auto use_blob = !options->at(Option::NO_BLOB)->getBool();

  std::string sql = "ALTER TABLE " + name_ + " ADD COLUMN ";

  Column::COLUMN_TYPES col_type = Column::COLUMN_MAX;

  auto use_virtual = true;

  // lock table to create definition
  table_mutex.lock();

  if (no_use_virtual ||
      (columns_->size() == 1 && columns_->at(0)->auto_increment == true))
    use_virtual = false;

  while (col_type == Column::COLUMN_MAX) {
    /* new columns are in ratio of 3:2:1:1:1:1
     * INT:VARCHAR:CHAR:BLOB:BOOL:GENERATD */
    auto prob = rand_int(8);
    if (prob < 1)
      col_type = Column::INTEGER;
    else if (prob < 3)
      col_type = Column::INT;
    else if (prob < 5)
      col_type = Column::VARCHAR;
    else if (prob < 6)
      col_type = Column::CHAR;
    else if (prob < 7 && use_virtual)
      col_type = Column::GENERATED;
    else if (prob < 8)
      col_type = Column::BOOL;
    else if (prob < 9 && use_blob)
      col_type = Column::BLOB;
  }

  Column *tc;

  std::string name = "N" + std::to_string(rand_int(300));

  if (col_type == Column::GENERATED)
    tc = new Generated_Column(name, this);
  else if (col_type == Column::BLOB)
    tc = new Blob_Column(name, this);
  else
    tc = new Column(name, this, col_type);

  sql += tc->definition();

  std::string algo;
  std::string algorithm_lock = pick_algorithm_lock(&algo);

  bool has_virtual_column = false;
  /* if a table has virtual column, We can not add AFTER */
  for (auto col : *columns_) {
    if (col->type_ == Column::GENERATED) {
      has_virtual_column = true;
      break;
    }
  }
  if (col_type == Column::GENERATED)
    has_virtual_column = true;

  if ((((algo == "INSTANT" || algo == "INPLACE") &&
        has_virtual_column == false && key_block_size == 1) ||
       (algo != "INSTANT" && algo != "INPLACE")) &&
      rand_int(10, 1) <= 7) {
    sql += " AFTER " + columns_->at(rand_int(columns_->size() - 1))->name_;
  }

  sql += ",";

  sql += algorithm_lock;

  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    auto add_new_column =
        true; // check if there is already a column with this name
    for (auto col : *columns_) {
      if (col->name_.compare(tc->name_) == 0)
        add_new_column = false;
    }

    if (add_new_column)
      AddInternalColumn(tc);
    else
      delete tc;

    table_mutex.unlock();
  } else
    delete tc;
}

/* randomly drop some index of table */
void Table::DropIndex(Thd1 *thd) {
  table_mutex.lock();
  if (indexes_ != nullptr && indexes_->size() > 0) {
    auto index = indexes_->at(rand_int(indexes_->size() - 1));
    auto name = index->name_;
    std::string sql = "ALTER TABLE " + name_ + " DROP INDEX " + name + ",";
    sql += pick_algorithm_lock();
    table_mutex.unlock();
    if (execute_sql(sql, thd)) {
      table_mutex.lock();
      for (size_t i = 0; i < indexes_->size(); i++) {
        auto ix = indexes_->at(i);
        if (ix->name_.compare(name) == 0) {
          delete ix;
          indexes_->at(i) = indexes_->back();
          indexes_->pop_back();
          break;
        }
      }
      table_mutex.unlock();
    }
  } else {
    table_mutex.unlock();
    thd->thread_log << "no index to drop " + name_ << std::endl;
  }
}

/*randomly add some index on the table */
void Table::AddIndex(Thd1 *thd) {
  auto i = rand_int(1000);
  Index *id = new Index(name_ + std::to_string(i));

  static size_t max_columns = opt_int(INDEX_COLUMNS);
  table_mutex.lock();

  /* number of columns to be added */
  int no_of_columns = rand_int(
      (max_columns < columns_->size() ? max_columns : columns_->size()), 1);

  std::vector<int> col_pos; // position of columns

  /* pick some columns */
  while (col_pos.size() < (size_t)no_of_columns) {
    int current = rand_int(columns_->size() - 1);
    /* auto-inc column should be first column in auto_inc_index */
    bool already_added = false;
    for (auto id : col_pos) {
      if (id == current)
        already_added = true;
    }
    if (!already_added)
      col_pos.push_back(current);
  } // while

  for (auto pos : col_pos) {
    auto col = columns_->at(pos);
    static bool no_desc_support = opt_bool(NO_DESC_INDEX);
    bool column_desc = false;
    if (!no_desc_support) {
      column_desc = rand_int(100) < DESC_INDEXES_IN_COLUMN
                        ? true
                        : false; // 33 % are desc //
    }
    id->AddInternalColumn(new Ind_col(col, column_desc)); // desc is set as true
  }

  std::string sql = "ALTER TABLE " + name_ + " ADD " + id->definition() + ",";
  sql += pick_algorithm_lock();
  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    auto do_not_add = false; // check if there is already a index with this name
    for (auto ind : *indexes_) {
      if (ind->name_.compare(id->name_) == 0)
        do_not_add = true;
    }
    if (!do_not_add)
      AddInternalIndex(id);
    else
      delete id;

    table_mutex.unlock();
  } else {
    delete id;
  }
}

void Table::DeleteAllRows(Thd1 *thd) {
  std::string sql = "DELETE FROM " + name_;
  if (type == PARTITION && rand_int(100) < 98) {
    sql += " PARTITION (";
    auto part = static_cast<Partition *>(this);
    assert(part->number_of_part > 0);
    table_mutex.lock();
    if (part->part_type == Partition::RANGE) {
      sql += part->positions.at(rand_int(part->positions.size() - 1)).name;
      /* below randomness is added intentionally */
      for (int i = 0; i < rand_int(part->positions.size()); i++) {
        if (rand_int(5) == 1)
          sql += "," +
                 part->positions.at(rand_int(part->positions.size() - 1)).name;
      }
    } else if (part->part_type == Partition::KEY ||
               part->part_type == Partition::HASH) {
      sql += "p" + std::to_string(rand_int(part->number_of_part - 1));
      for (int i = 0; i < rand_int(part->number_of_part); i++) {
        if (rand_int(2) == 1)
          sql += ", p" + std::to_string(rand_int(part->number_of_part - 1));
      }
    } else if (part->part_type == Partition::LIST) {
      sql += part->lists.at(rand_int(part->lists.size() - 1)).name;
      /* below randomness is added intentionally */
      for (int i = 0; i < rand_int(part->lists.size()); i++) {
        if (rand_int(5) == 1)
          sql += "," + part->lists.at(rand_int(part->lists.size() - 1)).name;
      }
    }
    table_mutex.unlock();
    sql += ")";
  }
  execute_sql(sql, thd);
}

void Table::SelectAllRow(Thd1 *thd) {
  std::string sql = "SELECT * FROM " + name_;
  if (type == PARTITION && rand_int(100) < 98) {
    sql += " PARTITION (";
    auto part = static_cast<Partition *>(this);
    assert(part->number_of_part > 0);
    table_mutex.lock();
    if (part->part_type == Partition::RANGE) {
      sql += part->positions.at(rand_int(part->positions.size() - 1)).name;
      for (int i = 0; i < rand_int(part->positions.size()); i++) {
        if (rand_int(2) == 1)
          sql += "," +
                 part->positions.at(rand_int(part->positions.size() - 1)).name;
      }
    } else if (part->part_type == Partition::KEY ||
               part->part_type == Partition::HASH) {
      sql += "p" + std::to_string(rand_int(part->number_of_part - 1));
      for (int i = 0; i < rand_int(part->number_of_part); i++) {
        if (rand_int(2) == 1)
          sql += ", p" + std::to_string(rand_int(part->number_of_part - 1));
      }
    } else if (part->part_type == Partition::RANGE) {
      sql += part->lists.at(rand_int(part->lists.size() - 1)).name;
      for (int i = 0; i < rand_int(part->lists.size()); i++) {
        if (rand_int(2) == 1)
          sql += "," + part->lists.at(rand_int(part->lists.size() - 1)).name;
      }
    }
    sql += ")";
    table_mutex.unlock();
  }
  execute_sql(sql, thd);
}

void Table::IndexRename(Thd1 *thd) {
  table_mutex.lock();
  if (indexes_->size() == 0)
    table_mutex.unlock();
  else {
    auto ps = rand_int(indexes_->size() - 1);
    auto name = indexes_->at(ps)->name_;
    /* ALTER index to _rename or back to orignal_name */
    std::string new_name = "_rename";
    static auto s = new_name.size();
    if (name.size() > s &&
        name.substr(name.length() - s).compare("_rename") == 0)
      new_name = name.substr(0, name.length() - s);
    else
      new_name = name + new_name;
    std::string sql = "ALTER TABLE " + name_ + " RENAME INDEX " + name +
                      " To " + new_name + ",";
    sql += pick_algorithm_lock();
    table_mutex.unlock();
    if (execute_sql(sql, thd)) {
      table_mutex.lock();
      for (auto &ind : *indexes_) {
        if (ind->name_.compare(name) == 0)
          ind->name_ = new_name;
      }
      table_mutex.unlock();
    }
  }
}

void Table::ColumnRename(Thd1 *thd) {
  table_mutex.lock();
  auto ps = rand_int(columns_->size() - 1);
  auto name = columns_->at(ps)->name_;
  /* ALTER column to _rename or back to orignal_name */
  std::string new_name = "_rename";
  static auto s = new_name.size();
  if (name.size() > s && name.substr(name.length() - s).compare("_rename") == 0)
    new_name = name.substr(0, name.length() - s);
  else
    new_name = name + new_name;
  std::string sql = "ALTER TABLE " + name_ + " RENAME COLUMN " + name + " To " +
                    new_name + ",";
  sql += pick_algorithm_lock();
  table_mutex.unlock();
  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    for (auto &col : *columns_) {
      if (col->name_.compare(name) == 0)
        col->name_ = new_name;
    }
    table_mutex.unlock();
  }
}

void Table::DeleteRandomRow(Thd1 *thd) {
  table_mutex.lock();
  auto where = -1;
  bool only_bool = true;
  int pk_pos = -1;

  for (size_t i = 0; i < columns_->size(); i++) {
    auto col = columns_->at(i);
    if (col->type_ != Column::BOOL)
      only_bool = false;
    if (col->primary_key)
      pk_pos = i;
  }

  /* 50% time we use primary key column */
  if (pk_pos != -1 && rand_int(100) > 50)
    where = pk_pos;
  else {
    /* iterate over and over to find a valid column */
    while (where < 0) {
      auto col_pos = rand_int(columns_->size() - 1);
      switch (columns_->at(col_pos)->type_) {
      case Column::BOOL:
        if (only_bool || rand_int(1000) == 0)
          where = col_pos;
        break;
      case Column::INT:
      case Column::FLOAT:
      case Column::DOUBLE:
      case Column::VARCHAR:
      case Column::CHAR:
      case Column::BLOB:
      case Column::GENERATED:
        where = col_pos;
        break;
      case Column::INTEGER:
        if (rand_int(1000) < 10)
          where = col_pos;
        break;
      case Column::COLUMN_MAX:
        break;
      }
    }
  }
  std::string sql = "DELETE FROM " + name_;

  if (type == PARTITION && rand_int(10) < 2) {
    sql += " PARTITION (";
    auto part = static_cast<Partition *>(this);
    assert(part->number_of_part > 0);
    if (part->part_type == Partition::RANGE) {
      sql += part->positions.at(rand_int(part->positions.size() - 1)).name;
    } else if (part->part_type == Partition::KEY ||
               part->part_type == Partition::HASH) {
      sql += "p" + std::to_string(rand_int(part->number_of_part - 1));
    } else if (part->part_type == Partition::LIST) {
      sql += part->lists.at(rand_int(part->lists.size() - 1)).name;
    }

    sql += ")";
  }
  sql += " WHERE " + columns_->at(where)->name_;

  auto prob = rand_int(100);
  if (prob <= 90)
    sql += " = " + columns_->at(where)->rand_value();
  else if (prob <= 92)
    sql += " >= " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->name_ +
           " <= " + columns_->at(where)->rand_value();
  else if (prob <= 96)
    sql += " IN (" + columns_->at(where)->rand_value() + "," +
           columns_->at(where)->rand_value() + ")";
  else if (prob <= 99)
    sql += " BETWEEN " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();
  else
    sql += " LIKE " + prepare_like_string(columns_->at(where)->rand_value());

  table_mutex.unlock();
  execute_sql(sql, thd);
}

void Table::SelectRandomRow(Thd1 *thd) {
  table_mutex.lock();
  auto where = -1;
  while (where < 0) {
    auto col_pos = rand_int(columns_->size() - 1);
    switch (columns_->at(col_pos)->type_) {
    case Column::BOOL:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::INT:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::BLOB:
    case Column::GENERATED:
      where = col_pos;
      break;
    case Column::INTEGER:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::COLUMN_MAX:
      break;
    }
  }
  std::string sql = "SELECT * FROM " + name_;

  /* if it partition table randomly pick some partition */
  if (type == PARTITION && rand_int(10) < 2) {
    sql += " PARTITION (";
    auto part = static_cast<Partition *>(this);
    assert(part->number_of_part > 0);
    if (part->part_type == Partition::RANGE) {
      sql += part->positions.at(rand_int(part->positions.size() - 1)).name;
    } else if (part->part_type == Partition::KEY ||
               part->part_type == Partition::HASH) {
      sql += "p" + std::to_string(rand_int(part->number_of_part - 1));
    } else if (part->part_type == Partition::LIST) {
      sql += part->lists.at(rand_int(part->lists.size() - 1)).name;
    }

    sql += ")";
  }

  sql += " WHERE " + columns_->at(where)->name_;
  auto prob = rand_int(100);
  if (rand_int(1000) < 2)
    sql += " NOT BETWEEN " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();
  else if (prob <= 90)
    sql += " = " + columns_->at(where)->rand_value();
  else if (prob <= 92)
    sql += " >= " + columns_->at(where)->rand_value();
  else if (prob <= 94)
    sql += " >= " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->name_ +
           " <= " + columns_->at(where)->rand_value();
  else if (prob <= 96)
    sql += " IN (" + columns_->at(where)->rand_value() + ", " +
           columns_->at(where)->rand_value() + ")";
  else if (prob <= 98)
    sql += " LIKE " + prepare_like_string(columns_->at(where)->rand_value());
  else
    sql += " BETWEEN " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();

  table_mutex.unlock();
  execute_sql(sql, thd);
}

/* update random row */
void Table::UpdateRandomROW(Thd1 *thd) {
  table_mutex.lock();
  int set;
  while (true) {
    set = rand_int(columns_->size() - 1);
    if (columns_->at(set)->type_ != Column::GENERATED)
      break;
  }

  auto where = -1;
  while (where < 0) {
    auto col_pos = rand_int(columns_->size() - 1);
    switch (columns_->at(col_pos)->type_) {
    case Column::BOOL:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::INT:
    case Column::FLOAT:
    case Column::DOUBLE:
    case Column::VARCHAR:
    case Column::CHAR:
    case Column::BLOB:
    case Column::GENERATED:
      where = col_pos;
      break;
    case Column::INTEGER:
      if (rand_int(1000) < 10)
        where = col_pos;
      break;
    case Column::COLUMN_MAX:
      break;
    }
  }
  std::string sql = "UPDATE " + name_;

  if (type == PARTITION && rand_int(10) < 2) {
    sql += " PARTITION (";
    auto part = static_cast<Partition *>(this);
    assert(part->number_of_part > 0);
    if (part->part_type == Partition::RANGE) {
      sql += part->positions.at(rand_int(part->positions.size() - 1)).name;
    } else if (part->part_type == Partition::KEY ||
               part->part_type == Partition::HASH) {
      sql += "p" + std::to_string(rand_int(part->number_of_part - 1));
    }
    if (part->part_type == Partition::LIST) {
      sql += part->lists.at(rand_int(part->lists.size() - 1)).name;
    }

    sql += ")";
  }

  sql += " SET " + columns_->at(set)->name_ + " = " +
         columns_->at(set)->rand_value() + " WHERE ";

  /* if tables has pkey try to use that in where clause for 50% cases */
  for (size_t i = 0; i < columns_->size(); i++) {
    if (columns_->at(i)->primary_key && rand_int(100) <= 50) {
      where = i;
      break;
    }
  }
  auto prob = rand_int(100);
  if (prob <= 90)
    sql +=
        columns_->at(where)->name_ + " = " + columns_->at(where)->rand_value();
  else if (prob <= 92)
    sql += columns_->at(where)->name_ +
           " >= " + columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->name_ +
           " >= " + columns_->at(where)->rand_value();
  else if (prob <= 94)
    sql += columns_->at(where)->name_ + " IN (" +
           columns_->at(where)->rand_value() + "," +
           columns_->at(where)->rand_value() + ")";
  else if (prob <= 98)
    sql += columns_->at(where)->name_ + " BETWEEN " +
           columns_->at(where)->rand_value() + " AND " +
           columns_->at(where)->rand_value();
  else
    sql += columns_->at(where)->name_ + " LIKE " +
           prepare_like_string(columns_->at(where)->rand_value());

  table_mutex.unlock();
  execute_sql(sql, thd);
}

bool Table::InsertBulkRecord(Thd1 *thd, size_t number_of_records) {
  std::vector<int> unique_keys;
  bool is_list_partition = false;

  if (number_of_records == 0)
    return true;

  std::string prepare_sql = "INSERT ";

  /* ignore error in the case parition list  */
  if (type == PARTITION &&
      static_cast<Partition *>(this)->part_type == Partition::LIST) {
    is_list_partition = true;
  }

  if (is_list_partition)
    prepare_sql += "IGNORE ";

  prepare_sql += "INTO " + name_ + " (";

  assert(number_of_records <=
         static_cast<size_t>(
             g_integer_range *
             options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));

  for (const auto &column : *columns_) {
    prepare_sql += column->name_ + ", ";

    /* if a table has primary key generated random keys */
    if (column->primary_key) {
      std::unordered_set<int> unique_keys_sets(number_of_records);
      size_t record_generated = 0;
      while (record_generated < number_of_records) {
        /* 0 in primary key generates */
        auto tmp_key = rand_int(
            g_integer_range *
                options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt(),
            1);

        if (unique_keys_sets.count(tmp_key) == 0) {
          unique_keys_sets.insert(tmp_key);
          record_generated++;
        }
      }
      unique_keys.assign(unique_keys_sets.begin(), unique_keys_sets.end());
    }
  }

  prepare_sql.erase(prepare_sql.length() - 2);
  prepare_sql += ")";

  std::string values = " VALUES";

  while (number_of_records-- > 0) {
    std::string value = "(";
    for (const auto &column : *columns_) {
      if (column->type_ == Column::COLUMN_TYPES::GENERATED)
        value += "DEFAULT";
      else if (column->primary_key)
        value += std::to_string(unique_keys.at(number_of_records));
      else if (column->auto_increment == true)
        value += "NULL";
      else if (is_list_partition && column->name_.compare("ip_col") == 0) {
        /* for list partition we insert only maximum possible value
         * todo modify rand_value to return list parititon range */
        value += std::to_string(
            rand_int(maximum_records_in_each_parititon_list *
                     options->at(Option::MAX_PARTITIONS)->getInt()));
      } else
        value += column->rand_value();

      value += ", ";
    }
    value.erase(value.size() - 2);
    value += ")";
    values += value;
    if (values.size() > 1024 * 1024 || number_of_records == 0) {
      if (!execute_sql(prepare_sql + values, thd)) {
        ddl_logs_write.lock();
        thd->ddl_logs << "Bulk insert failed for table  " << name_ << std::endl;
        ddl_logs_write.unlock();
        run_query_failed = true;
        return false;
      }
      values = " VALUES";
    } else {
      values += ", ";
    }
  }

  return true;
}

void Table::InsertRandomRow(Thd1 *thd) {
  table_mutex.lock();
  std::string vals = "";
  std::string type = "INSERT";

  type = rand_int(3) == 0 ? "INSERT" : "REPLACE";

  std::string sql = type + " INTO " + name_ + "  ( ";
  for (auto &column : *columns_) {
    sql += column->name_ + " ,";
    std::string val;
    if (column->type_ == Column::COLUMN_TYPES::GENERATED)
      val = "default";
    else
      val = column->rand_value();
    if (column->auto_increment == true && rand_int(100) < 10)
      val = "NULL";
    vals += " " + val + ",";
  }

  if (vals.size() > 0) {
    vals.pop_back();
    sql.pop_back();
  }
  sql += ") VALUES(" + vals;
  sql += " )";
  table_mutex.unlock();
  execute_sql(sql, thd);
}

/* set mysqld_variable */
void set_mysqld_variable(Thd1 *thd) {
  static int total_probablity = sum_of_all_server_options();
  int rd = rand_int(total_probablity);
  for (auto &opt : *server_options) {
    if (rd <= opt->prob) {
      std::string sql = "SET ";
      sql += rand_int(3) == 0 ? " SESSION " : " GLOBAL ";
      sql += opt->name + "=" + opt->values.at(rand_int(opt->values.size() - 1));
      execute_sql(sql, thd);
    }
  }
}

/* alter tablespace set encryption */
void alter_tablespace_encryption(Thd1 *thd) {
  std::string tablespace;

  if ((rand_int(10) < 2 && server_version() >= 80000) ||
      g_tablespace.size() == 0) {
    tablespace = "mysql";
  } else if (g_tablespace.size() > 0) {
    tablespace = g_tablespace[rand_int(g_tablespace.size() - 1)];
  }

  if (tablespace.size() > 0) {
    std::string sql = "ALTER TABLESPACE " + tablespace + " ENCRYPTION ";
    sql += (rand_int(1) == 0 ? "'Y'" : "'N'");
    execute_sql(sql, thd);
  }
}

/* alter table discard tablespace */
void Table::alter_discard_tablespace(Thd1 *thd) {
  std::string sql = "ALTER TABLE " + name_ + " DISCARD TABLESPACE";
  execute_sql(sql, thd);
  /* Discarding the tablespace makes the table unusable, hence recreate the
   * table */
  DropCreate(thd);
}

/* alter instance enable disable redo logging */
static void alter_redo_logging(Thd1 *thd) {
  std::string sql = "ALTER INSTANCE ";
  sql += (rand_int(1) == 0 ? "DISABLE" : "ENABLE");
  sql += " INNODB REDO_LOG";
  execute_sql(sql, thd);
}

/* alter database set encryption */
void alter_database_encryption(Thd1 *thd) {
  std::string sql = "ALTER DATABASE test ENCRYPTION ";
  sql += (rand_int(1) == 0 ? "'Y'" : "'N'");
  execute_sql(sql, thd);
}

/* create,alter,drop undo tablespace */
static void create_alter_drop_undo(Thd1 *thd) {
  auto x = rand_int(100);
  if (x < 20) {
    std::string name =
        g_undo_tablespace[rand_int(g_undo_tablespace.size() - 1)];
    std::string sql =
        "CREATE UNDO TABLESPACE " + name + " ADD DATAFILE '" + name + ".ibu'";
    execute_sql(sql, thd);
  }
  if (x < 40) {
    std::string sql = "DROP UNDO TABLESPACE " +
                      g_undo_tablespace[rand_int(g_undo_tablespace.size() - 1)];
    execute_sql(sql, thd);
  } else {
    std::string sql =
        "ALTER UNDO TABLESPACE " +
        g_undo_tablespace[rand_int(g_undo_tablespace.size() - 1)] + " SET ";
    sql += (rand_int(1) == 0 ? "ACTIVE" : "INACTIVE");
    execute_sql(sql, thd);
  }
}

/* alter tablespace rename */
void alter_tablespace_rename(Thd1 *thd) {
  if (g_tablespace.size() > 0) {
    auto tablespace = g_tablespace[rand_int(g_tablespace.size() - 1),
                                   1]; // don't pick innodb_system;
    std::string sql = "ALTER TABLESPACE " + tablespace;
    if (rand_int(1) == 0)
      sql += "_rename RENAME TO " + tablespace;
    else
      sql += " RENAME TO " + tablespace + "_rename";
    execute_sql(sql, thd);
  }
}

/* load special sql from a file */
static std::vector<std::string> load_grammar_sql_from() {
  std::vector<std::string> array;
  auto grammar_file = opt_string(GRAMMAR_FILE);
  std::string sql, file;
  if (grammar_file == "grammar.sql")
    file = std::string(binary_fullpath) + "/" + std::string(grammar_file);
  else
    file = grammar_file;

  std::ifstream myfile(file);
  if (myfile.is_open()) {
    while (!myfile.eof()) {
      getline(myfile, sql);
      /* do not process any blank lines */
      if (sql.find_first_not_of("\t\n ") != std::string::npos)
        array.push_back(sql);
    }
    myfile.close();
  } else
    throw std::runtime_error("unable to open file " + file);
  return array;
}

/* return preformatted sql */
static void grammar_sql(std::vector<Table *> *all_tables, Thd1 *thd) {

  static std::vector<std::string> all_sql = load_grammar_sql_from();
  enum sql_col_types { INT, VARCHAR };

  if (all_sql.size() == 0)
    return;

  struct table {
    table(std::string n, std::vector<std::string> i, std::vector<std::string> v)
        : name(n), int_col(i), varchar_col(v){};
    std::string name;
    std::vector<std::string> int_col;
    std::vector<std::string> varchar_col;
  };

  auto sql = all_sql[rand_int(all_sql.size() - 1)];

  /* parse SQL in table */
  std::vector<std::vector<int>> sql_tables;

  int tab_sql = 1; // number of tables in sql
  bool table_found;

  do { // search for table
    std::smatch match;
    std::string tab_p = "T" + std::to_string(tab_sql); // table pattern

    if (regex_search(sql, match, std::regex(tab_p))) {
      table_found = true;
      sql_tables.push_back({0, 0});

      int col_sql = 1;
      bool column_found;

      do { // search of int column
        std::string col_p = tab_p + "_INT_" + std::to_string(col_sql);
        if (regex_search(sql, match, std::regex(col_p))) {
          column_found = true;
          sql_tables.at(tab_sql - 1).at(INT)++;
          col_sql++;
        } else
          column_found = false;
      } while (column_found);

      col_sql = 1;
      do {
        std::string col_p = tab_p + "_VARCHAR_" + std::to_string(col_sql);
        if (regex_search(sql, match, std::regex(col_p))) {
          column_found = true;
          sql_tables.at(tab_sql - 1).at(VARCHAR)++;
          col_sql++;
        } else
          column_found = false;
      } while (column_found);
    } else
      table_found = false;
    tab_sql++;
  } while (table_found);

  std::vector<table> final_tables;

  /* try at max 100 times */
  int table_check = 100;

  while (sql_tables.size() > 0 && table_check-- > 0) {

    auto int_columns = sql_tables.back().at(INT);
    auto varchar_columns = sql_tables.back().at(VARCHAR);
    std::vector<std::string> int_cols_str, var_cols_str;
    int column_check = 20;
    auto table = all_tables->at(rand_int(all_tables->size() - 1));
    table->table_mutex.lock();
    auto columns = table->columns_;

    // find columns in table //
    do {
      auto col = columns->at(rand_int(columns->size() - 1));

      if (int_columns > 0 && col->type_ == Column::INT) {
        int_cols_str.push_back(col->name_);
        int_columns--;
      }
      if (varchar_columns > 0 && col->type_ == Column::VARCHAR) {
        var_cols_str.push_back(col->name_);
        varchar_columns--;
      }

      if (int_columns == 0 && varchar_columns == 0) {
        final_tables.emplace_back(table->name_, int_cols_str, var_cols_str);
        sql_tables.pop_back();
      }
    } while (!(int_columns == 0 && varchar_columns == 0) && column_check-- > 0);

    table->table_mutex.unlock();
  }

  if (sql_tables.size() == 0) {

    for (size_t i = 0; i < final_tables.size(); i++) {
      auto table = final_tables.at(i);
      auto table_name = "T" + std::to_string(i + 1);

      /* replace int column */
      for (size_t j = 0; j < table.int_col.size(); j++)
        sql = std::regex_replace(
            sql, std::regex(table_name + "_INT_" + std::to_string(j + 1)),
            table_name + "." + table.int_col.at(j));

      /* replace varchar column */
      for (size_t j = 0; j < table.varchar_col.size(); j++)
        sql = std::regex_replace(
            sql, std::regex(table_name + "_VARCHAR_" + std::to_string(j + 1)),
            table_name + "." + table.varchar_col.at(j));

      /* replace table "T1 " => tt_N T1 */
      sql = std::regex_replace(sql, std::regex(table_name + " "),
                               table.name + " " + table_name + " ");
      /* replace table "T1$" => tt_N T1*/
      sql = std::regex_replace(sql, std::regex(table_name + "$"),
                               table.name + " " + table_name + "");
    }

    execute_sql(sql, thd);
  } else
    std::cout << "NOT ABLE TO FIND any SQL in special SQL" << std::endl;
}

/* save metadata to a file */
void save_metadata_to_file() {
  std::string path = opt_string(METADATA_PATH);
  if (path.size() == 0)
    path = opt_string(LOGDIR);
  auto file = path + "/step_" +
              std::to_string(options->at(Option::STEP)->getInt()) + ".dll";
  std::cout << "Saving metadata to file " << file << std::endl;

  StringBuffer sb;
  PrettyWriter<StringBuffer> writer(sb);
  writer.StartObject();
  writer.String("version");
  writer.Uint(version);
  writer.String(("tables"));
  writer.StartArray();
  for (auto j = all_tables->begin(); j != all_tables->end(); j++) {
    auto table = *j;
    table->Serialize(writer);
  }
  writer.EndArray();
  writer.EndObject();
  std::ofstream of(file);
  of << sb.GetString();

  if (!of.good())
    throw std::runtime_error("can't write the JSON string to the file!");
}

/* create in memory data about tablespaces, row_format, key_block size and undo
 * tablespaces */
void create_in_memory_data() {

  /* Adjust the tablespaces */
  if (!options->at(Option::NO_TABLESPACE)->getBool()) {
    g_tablespace = {"tab02k", "tab04k"};
    g_tablespace.push_back("innodb_system");
    if (g_innodb_page_size >= INNODB_8K_PAGE_SIZE) {
      g_tablespace.push_back("tab08k");
    }
    if (g_innodb_page_size >= INNODB_16K_PAGE_SIZE) {
      g_tablespace.push_back("tab16k");
    }
    if (g_innodb_page_size >= INNODB_32K_PAGE_SIZE) {
      g_tablespace.push_back("tab32k");
    }
    if (g_innodb_page_size >= INNODB_64K_PAGE_SIZE) {
      g_tablespace.push_back("tab64k");
    }

    /* add addtional tablespace */
    auto tbs_count = opt_int(NUMBER_OF_GENERAL_TABLESPACE);
    if (tbs_count > 1) {
      auto current_size = g_tablespace.size();
      for (size_t i = 0; i < current_size; i++) {
        for (int j = 1; j <= tbs_count; j++)
          if (g_tablespace[i].compare("innodb_system") == 0)
            continue;
          else
            g_tablespace.push_back(g_tablespace[i] + std::to_string(j));
      }
    }
  }

  /* set some of tablespace encrypt */
  if (!options->at(Option::NO_ENCRYPTION)->getBool() &&
      !(strcmp(FORK, "MySQL") == 0 && server_version() < 80000)) {
    int i = 0;
    for (auto &tablespace : g_tablespace) {
      if (i++ % 2 == 0 &&
          tablespace.compare("innodb_system") != 0) // alternate tbs are encrypt
        tablespace += "_e";
    }
  }

  std::string row_format = opt_string(ROW_FORMAT);

  if (row_format.compare("all") == 0 &&
      options->at(Option::NO_TABLE_COMPRESSION)->getInt() == true)
    row_format = "uncompressed";

  if (row_format.compare("uncompressed") == 0) {
    g_row_format = {"DYNAMIC", "REDUNDANT"};
  } else if (row_format.compare("all") == 0) {
    g_row_format = {"DYNAMIC", "REDUNDANT", "COMPRESSED"};
    g_key_block_size = {0, 0, 1, 2, 4};
  } else if (row_format.compare("none") == 0) {
    g_key_block_size.clear();
  } else {
    g_row_format.push_back(row_format);
  }

  if (g_innodb_page_size > INNODB_16K_PAGE_SIZE) {
    g_row_format.clear();
    g_key_block_size.clear();
  }

  int undo_tbs_count = opt_int(NUMBER_OF_UNDO_TABLESPACE);
  if (undo_tbs_count > 0) {
    for (int i = 1; i <= undo_tbs_count; i++) {
      g_undo_tablespace.push_back("undo_00" + std::to_string(i));
    }
  }
}

/*load objects from a file */
static std::string load_metadata_from_file() {
  auto previous_step = options->at(Option::STEP)->getInt() - 1;
  auto path = opt_string(METADATA_PATH);
  if (path.size() == 0)
    path = opt_string(LOGDIR);
  auto file = path + "/step_" + std::to_string(previous_step) + ".dll";
  FILE *fp = fopen(file.c_str(), "r");

  if (fp == nullptr)
    throw std::runtime_error("unable to open file " + file);

  char readBuffer[65536];
  FileReadStream is(fp, readBuffer, sizeof(readBuffer));
  Document d;
  d.ParseStream(is);
  auto v = d["version"].GetInt();

  if (d["version"].GetInt() != version)
    throw std::runtime_error("version mismatch between " + file +
                             " and codebase " + " file::version is " +
                             std::to_string(v) + " code::version is " +
                             std::to_string(version));

  for (auto &tab : d["tables"].GetArray()) {
    Table *table;
    std::string name = tab["name"].GetString();
    std::string table_type = tab["type"].GetString();

    if (table_type.compare("PARTITION") == 0) {
      std::string part_type = tab["part_type"].GetString();
      table = new Partition(name, part_type, tab["number_of_part"].GetInt());

      if (part_type.compare("RANGE") == 0) {
        for (auto &par_range : tab["part_range"].GetArray()) {
          static_cast<Partition *>(table)->positions.emplace_back(
              par_range[0].GetString(), par_range[1].GetInt());
        }
      } else if (part_type.compare("LIST") == 0) {
        int curr_index_of_list = 0;
        for (auto &par_list : tab["part_list"].GetArray()) {
          static_cast<Partition *>(table)->lists.emplace_back(
              par_list[0].GetString());
          for (auto &list_value : par_list[1].GetArray())
            static_cast<Partition *>(table)
                ->lists.at(curr_index_of_list)
                .list.push_back(list_value.GetInt());
          curr_index_of_list++;
        }
      }
    } else if (table_type.compare("NORMAL") == 0) {
      table = new Table(name);
    } else
      throw std::runtime_error("Unhandle Table type " + table_type);

    table->set_type(table_type);

    std::string engine = tab["engine"].GetString();
    if (engine.compare("default") != 0) {
      table->engine = engine;
    }

    std::string row_format = tab["row_format"].GetString();
    if (row_format.compare("default") != 0) {
      table->row_format = row_format;
    }

    std::string tablespace = tab["tablespace"].GetString();
    if (tablespace.compare("file_per_table") != 0) {
      table->tablespace = tablespace;
    }

    table->encryption = tab["encryption"].GetString();
    table->compression = tab["compression"].GetString();

    table->key_block_size = tab["key_block_size"].GetInt();

    /* save columns */
    for (auto &col : tab["columns"].GetArray()) {
      Column *a;
      std::string type = col["type"].GetString();

      if (type.compare("INT") == 0 || type.compare("CHAR") == 0 ||
          type.compare("VARCHAR") == 0 || type.compare("BOOL") == 0 ||
          type.compare("FLOAT") == 0 || type.compare("DOUBLE") == 0 ||
          type.compare("INTEGER") == 0) {
        a = new Column(col["name"].GetString(), type, table);
      } else if (type.compare("GENERATED") == 0) {
        auto name = col["name"].GetString();
        auto clause = col["clause"].GetString();
        auto sub_type = col["sub_type"].GetString();
        a = new Generated_Column(name, table, clause, sub_type);
      } else if (type.compare("BLOB") == 0) {
        auto sub_type = col["sub_type"].GetString();
        a = new Blob_Column(col["name"].GetString(), table, sub_type);
      } else
        throw std::runtime_error("unhandled column type");

      a->null = col["null"].GetBool();
      a->auto_increment = col["auto_increment"].GetBool();
      a->length = col["lenght"].GetInt(),
      a->primary_key = col["primary_key"].GetBool();
      a->compressed = col["compressed"].GetBool();
      table->AddInternalColumn(a);
    }

    for (auto &ind : tab["indexes"].GetArray()) {
      Index *index = new Index(ind["name"].GetString());

      for (auto &ind_col : ind["index_columns"].GetArray()) {
        std::string index_base_column = ind_col["name"].GetString();

        for (auto &column : *table->columns_) {
          if (index_base_column.compare(column->name_) == 0) {
            index->AddInternalColumn(
                new Ind_col(column, ind_col["desc"].GetBool()));
            break;
          }
        }
      }
      table->AddInternalIndex(index);
    }

    all_tables->push_back(table);
    options->at(Option::TABLES)->setInt(all_tables->size());
  }
  fclose(fp);
  return file;
}

/* clean tables from memory,random_strs */
void clean_up_at_end() {
  for (auto &table : *all_tables)
    delete table;
  delete all_tables;
  delete random_strs;
}

/* create new database and tablespace */
void create_database_tablespace(Thd1 *thd) {

  /* drop database test*/
  execute_sql("DROP DATABASE IF EXISTS test", thd);
  execute_sql("CREATE DATABASE test", thd); // todo encrypt database/schema

  for (auto &tab : g_tablespace) {
    if (tab.compare("innodb_system") == 0)
      continue;

    std::string sql =
        "CREATE TABLESPACE " + tab + " ADD DATAFILE '" + tab + ".ibd' ";

    if (g_innodb_page_size <= INNODB_16K_PAGE_SIZE) {
      sql += " FILE_BLOCK_SIZE " + tab.substr(3, 3);
    }

    /* encrypt tablespace */
    if (!options->at(Option::NO_ENCRYPTION)->getBool()) {
      if (tab.substr(tab.size() - 2, 2).compare("_e") == 0)
        sql += " ENCRYPTION='Y'";
      else if (server_version() >= 80000)
        sql += " ENCRYPTION='N'";
    }

    /* first try to rename tablespace back */
    if (server_version() >= 80000)
      execute_sql("ALTER TABLESPACE " + tab + "_rename rename to " + tab, thd);

    execute_sql("DROP TABLESPACE " + tab, thd);

    if (!execute_sql(sql, thd))
      throw std::runtime_error("error in " + sql);
  }

  if (server_version() >= 80000) {
    for (auto &name : g_undo_tablespace) {
      std::string sql =
          "CREATE UNDO TABLESPACE " + name + " ADD DATAFILE '" + name + ".ibu'";
      execute_sql(sql, thd);
    }
  }
}

/* check all tables and partition in the starting and if any check table false
 * return false */
static bool check_tables_partitions_preload(Table *table, Thd1 *thd) {
  size_t failures = 0;
  if (table->type == Table::PARTITION) {
    int partition_count;
    switch (static_cast<Partition *>(table)->part_type) {
    case Partition::LIST:
      partition_count = static_cast<Partition *>(table)->lists.size();
      for (int i = 0; i < partition_count; i++) {
        get_check_result("ALTER TABLE " + table->name_ + " CHECK PARTITION " +
                             static_cast<Partition *>(table)->lists[i].name,
                         thd) ||
            failures++;
      }
      break;
    case Partition::RANGE:
      partition_count = static_cast<Partition *>(table)->positions.size();
      for (int i = 0; i < partition_count; i++) {
        get_check_result("ALTER TABLE " + table->name_ + " CHECK PARTITION " +
                             static_cast<Partition *>(table)->positions[i].name,
                         thd) ||
            failures++;
      }
      break;
    case Partition::HASH:
    case Partition::KEY:
      partition_count = static_cast<Partition *>(table)->number_of_part;
      for (int i = 0; i < partition_count; i++) {
        get_check_result("ALTER TABLE " + table->name_ + " CHECK PARTITION p" +
                             std::to_string(i),
                         thd) ||
            failures++;
      }
      break;
    }
  } else {
    get_check_result("CHECK TABLE " + table->name_, thd) || failures++;
  }
  return failures == 0 ? true : false;
}

/* load metadata */
bool Thd1::load_metadata() {
  sum_of_all_opts = sum_of_all_options(this);

  auto seed = opt_int(INITIAL_SEED);
  seed += options->at(Option::STEP)->getInt();
  random_strs = random_strs_generator(seed);

  /*set seed for current step*/
  auto initial_seed = opt_int(INITIAL_SEED);
  initial_seed += options->at(Option::STEP)->getInt();
  rng = std::mt19937(initial_seed);

  /* create in-memory data for general tablespaces */
  create_in_memory_data();

  if (options->at(Option::STEP)->getInt() > 1 &&
      !options->at(Option::PREPARE)->getBool()) {
    auto file = load_metadata_from_file();
    std::cout << "metadata loaded from " << file << std::endl;
  } else {
    create_database_tablespace(this);
    generate_metadata_for_tables();
    std::cout << "metadata created randomly" << std::endl;
  }

  if (options->at(Option::TABLES)->getInt() <= 0)
    throw std::runtime_error("no table to work on \n");

  return 1;
}

/* return true if successful or error out in case of fail */
bool Thd1::run_some_query() {
  execute_sql("USE " + options->at(Option::DATABASE)->getString(), this);

  /* first create temporary tables metadata if requried */
  int temp_tables;
  if (options->at(Option::ONLY_TEMPORARY)->getBool())
    temp_tables = options->at(Option::TABLES)->getInt();
  else if (options->at(Option::NO_TEMPORARY)->getBool())
    temp_tables = 0;
  else
    temp_tables = options->at(Option::TABLES)->getInt() /
                  options->at(Option::TEMPORARY_TO_NORMAL_RATIO)->getInt();

  /* create temporary table */
  std::vector<Table *> *all_session_tables = new std::vector<Table *>;
  for (int i = 0; i < temp_tables; i++) {

    Table *table = Table::table_id(Table::TEMPORARY, i);
    if (!table->load(this))
      return false;
    all_session_tables->push_back(table);
  }

  /* prepare is passed, create all tables */
  if (options->at(Option::PREPARE)->getBool() ||
      options->at(Option::STEP)->getInt() == 1) {
    auto current = table_started++;

    while (current < all_tables->size()) {
      auto table = all_tables->at(current);
      if (!table->load(this))
        return false;
      table_completed++;
      current = table_started++;
    }

    // wait for all tables to finish loading
    while (table_completed < all_tables->size()) {
      thread_log << "Waiting for all threds to finish initial load "
                 << std::endl;
      std::chrono::seconds dura(1);
      if (run_query_failed) {
        thread_log << "Some other thread failed, Exiting. Please check logs "
                   << std::endl;
        return false;
      }
      std::this_thread::sleep_for(dura);
    }
  } else if (options->at(Option::CHECK_TABLE_PRELOAD)->getBool()) {
    auto current = table_started++;

    while (current < all_tables->size()) {
      auto table = all_tables->at(current);
      check_tables_partitions_preload(table, this) || check_failures++;
      table_completed++;
      current = table_started++;
    }
    // wait for all tables to finish check table
    while (table_completed < all_tables->size()) {
      thread_log << "Waiting for all threds to finish check tables "
                 << std::endl;
      std::chrono::seconds dura(1);
      std::this_thread::sleep_for(dura);
    }
  }

  if (options->at(Option::JUST_LOAD_DDL)->getBool() ||
      options->at(Option::PREPARE)->getBool())
    return true;

  /*Print once on screen and in general logs */
  if (!lock_stream.test_and_set()) {
    std::stringstream s;
    if (check_failures > 0) {
      s << "Check table failed for " << check_failures << " "
        << (check_failures == 1 ? "table" : " tables")
        << ". Check thread logs for details \n ";
    }
    s << "Starting random load in " << options->at(Option::THREADS)->getInt()
      << " threads.\n";
    std::cout << s.str();
    this->ddl_logs << s.str();
  }

  auto sec = opt_int(NUMBER_OF_SECONDS_WORKLOAD);
  auto begin = std::chrono::system_clock::now();
  auto end =
      std::chrono::system_clock::time_point(begin + std::chrono::seconds(sec));

  /* set seed for current thread */
  rng = std::mt19937(set_seed(this));
  thread_log << " value of rand_int(100) " << rand_int(100) << std::endl;

  /* combine session tables with all tables */
  all_session_tables->insert(all_session_tables->end(), all_tables->begin(),
                             all_tables->end());

  /* freqency of all options per thread */
  int opt_feq[Option::MAX][2] = {{0, 0}};

  /* variables related to trxs */
  static auto trx_prob = options->at(Option::TRANSATION_PRB_K)->getInt();
  static auto trx_max_size = options->at(Option::TRANSACTIONS_SIZE)->getInt();
  static auto commit_to_rollback =
      options->at(Option::COMMMIT_TO_ROLLBACK_RATIO)->getInt();
  static auto savepoint_prob = options->at(Option::SAVEPOINT_PRB_K)->getInt();

  int trx_left = -1; // -1 for single trx
  int current_save_point = 0;
  while (std::chrono::system_clock::now() < end) {

    /* check if we need to make sql as part of existing or new trx */
    if (trx_prob > 0) {
      if (trx_left == 0)
        execute_sql((rand_int(commit_to_rollback) == 0 ? "ROLLBACK" : "COMMIT"),
                    this);

      /*  check if sql are chossen as part of statement or single trx */
      if (trx_left <= 0 && trx_max_size > 0 && rand_int(1000) < trx_prob) {
        current_save_point = 0;
        if (rand_int(1000) == 1)
          trx_left = trx_max_size * 100;
        else
          trx_left = rand_int(trx_max_size);
        execute_sql("START TRANSACTION", this);
      }

      /* use savepoint or rollback to savepoint */
      if (trx_left > 0 && savepoint_prob > 0) {
        if (rand_int(1000) < savepoint_prob)
          execute_sql("SAVEPOINT SAVE" + std::to_string(++current_save_point),
                      this);

        /* 1/4 chances of rollbacking to savepoint */
        if (current_save_point > 0 && rand_int(1000 * 4) < savepoint_prob) {
          auto sv = rand_int(current_save_point, 1);
          execute_sql("ROLLBACK TO SAVEPOINT SAVE" + std::to_string(sv), this);
          current_save_point = sv - 1;
        }

        trx_left--;
      }
    }

    auto table =
        all_session_tables->at(rand_int(all_session_tables->size() - 1));
    auto option = pick_some_option();
    ddl_query = options->at(option)->ddl == true ? true : false;

    switch (option) {
    case Option::DROP_INDEX:
      table->DropIndex(this);
      break;
    case Option::ADD_INDEX:
      table->AddIndex(this);
      break;
    case Option::DROP_COLUMN:
      table->DropColumn(this);
      break;
    case Option::ADD_COLUMN:
      table->AddColumn(this);
      break;
    case Option::TRUNCATE:
      table->Truncate(this);
      break;
    case Option::DROP_CREATE:
      table->DropCreate(this);
      break;
    case Option::ALTER_TABLE_ENCRYPTION:
      table->SetEncryption(this);
      break;
    case Option::ALTER_TABLE_COMPRESSION:
      table->SetTableCompression(this);
      break;
    case Option::ALTER_COLUMN_MODIFY:
      table->ModifyColumn(this);
      break;
    case Option::SET_GLOBAL_VARIABLE:
      set_mysqld_variable(this);
      break;
    case Option::ALTER_TABLESPACE_ENCRYPTION:
      alter_tablespace_encryption(this);
      break;
    case Option::ALTER_DISCARD_TABLESPACE:
      table->alter_discard_tablespace(this);
      break;
    case Option::ALTER_TABLESPACE_RENAME:
      alter_tablespace_rename(this);
      break;
    case Option::SELECT_ALL_ROW:
      table->SelectAllRow(this);
      break;
    case Option::SELECT_ROW_USING_PKEY:
      table->SelectRandomRow(this);
      break;
    case Option::INSERT_RANDOM_ROW:
      table->InsertRandomRow(this);
      break;
    case Option::DELETE_ALL_ROW:
      table->DeleteAllRows(this);
      break;
    case Option::DELETE_ROW_USING_PKEY:
      table->DeleteRandomRow(this);
      break;
    case Option::UPDATE_ROW_USING_PKEY:
      table->UpdateRandomROW(this);
      break;
    case Option::OPTIMIZE:
      table->Optimize(this);
      break;
    case Option::CHECK_TABLE:
      table->Check(this);
      break;
    case Option::ADD_DROP_PARTITION:
      if (table->type == Table::PARTITION)
        static_cast<Partition *>(table)->AddDrop(this);
      break;
    case Option::ANALYZE:
      table->Analyze(this);
      break;
    case Option::RENAME_COLUMN:
      table->ColumnRename(this);
      break;
    case Option::RENAME_INDEX:
      table->IndexRename(this);
      break;
    case Option::ALTER_MASTER_KEY:
      execute_sql("ALTER INSTANCE ROTATE INNODB MASTER KEY", this);
      break;
    case Option::ALTER_ENCRYPTION_KEY:
      execute_sql("ALTER INSTANCE ROTATE INNODB SYSTEM KEY " +
                      std::to_string(rand_int(9)),
                  this);
      break;
    case Option::ALTER_GCACHE_MASTER_KEY:
      execute_sql("ALTER INSTANCE ROTATE GCACHE MASTER KEY", this);
      break;
    case Option::ALTER_INSTANCE_RELOAD_KEYRING:
      if (keyring_comp_status)
        execute_sql("ALTER INSTANCE RELOAD KEYRING", this);
      break;
    case Option::ROTATE_REDO_LOG_KEY:
      execute_sql("SELECT rotate_system_key(\"percona_redo\")", this);
      break;
    case Option::ALTER_REDO_LOGGING:
      alter_redo_logging(this);
      break;
    case Option::ALTER_DATABASE_ENCRYPTION:
      alter_database_encryption(this);
      break;
    case Option::UNDO_SQL:
      create_alter_drop_undo(this);
      break;
    case Option::GRAMMAR_SQL:
      grammar_sql(all_session_tables, this);
      break;

    default:
      throw std::runtime_error("invalid options");
    }

    options->at(option)->total_queries++;

    /* sql executed is at 0 index, and if successful at 1 */
    opt_feq[option][0]++;
    if (success) {
      options->at(option)->success_queries++;
      opt_feq[option][1]++;
      success = false;
    }

    if (run_query_failed) {
      break;
    }
  } // while

  /* print options frequency in logs */
  for (int i = 0; i < Option::MAX; i++) {
    if (opt_feq[i][0] > 0)
      thread_log << options->at(i)->help << ", total=>" << opt_feq[i][0]
                 << ", success=> " << opt_feq[i][1] << std::endl;
  }

  /* cleanup session temporary tables tables */
  for (auto &table : *all_session_tables)
    if (table->type == Table::TEMPORARY)
      delete table;
  delete all_session_tables;
  return true;
}
