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

const std::string TABLE_PREFIX = "tt_";
const std::string PARTITION_SUFFIX = "_p";
const std::string FK_SUFFIX = "_fk";
const std::string TEMP_SUFFIX = "_t";
const std::string VECTOR_SUFFIX = "_v";
const int version = 3;
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
/* vector support: set by setup_vector() in sum_of_all_options() */
static bool g_vector_enabled = false;
/* the server failed the HNSW probe */
static bool g_vector_probe_failed = false;
std::mutex ddl_logs_write;
static std::chrono::system_clock::time_point start_time =
    std::chrono::system_clock::now();

std::atomic<int> table_started(0);
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

/* return table pointer of matching table. This is only done during the
 * first step or during the prepare, so you would have only tables that are not
 * renamed  */
static Table *pick_table(Table::TABLE_TYPES type, int id) {
  std::string name = TABLE_PREFIX + std::to_string(id);
  if (type == Table::FK) {
    name += FK_SUFFIX;
  } else if (type == Table::PARTITION) {
    name += PARTITION_SUFFIX;
  } else if (type == Table::VECTOR) {
    name += VECTOR_SUFFIX;
  }
  for (auto const &table : *all_tables) {
    if (table->name_ == name)
      return table;
  }
  return nullptr;
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

/* generate random numbers to populate in primary and fk
@param[in] number_of_records
@param[out] vector containing unique elements */
static std::vector<int> generateUniqueRandomNumbers(int number_of_records) {

  std::unordered_set<int> unique_keys_set(number_of_records);

  int max_size =
      g_integer_range * options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt();

  while (unique_keys_set.size() < static_cast<size_t>(number_of_records)) {
    unique_keys_set.insert(rand_int(max_size, 1));
  }

  std::vector<int> unique_keys(unique_keys_set.begin(), unique_keys_set.end());
  return unique_keys;
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

  /* on error thd->result still holds the result of an earlier query */
  if (!execute_sql(sql, thd))
    return query_result;
  auto row = mysql_fetch_row_safe(thd);
  /* A row containing SQL NULL has a nullptr field, even when the row exists. */
  if (row && mysql_num_fields_safe(thd, 1) && row[0] != nullptr)
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

bool vector_enabled() { return g_vector_enabled; }

static bool table_enabled(const Table *table) {
  return table->type != Table::VECTOR || vector_enabled();
}

/* options that only make sense with vector support. They are zeroed when
 * vector support is off, and passing one of them on the command line asks for
 * vector support explicitly */
static const std::vector<Option::Opt> &vector_options() {
  static const std::vector<Option::Opt> opts = {
      Option::VECTOR_PROB, Option::VECTOR_MAX_DIM, Option::SELECT_VECTOR_ANN,
      Option::SET_HNSW_EF_SEARCH, Option::ADD_DROP_VECTOR_INDEX};
  return opts;
}

/* turn vector support off, log the reason once to stdout and the general log */
static void disable_vector(Thd1 *thd, const std::string &reason) {
  g_vector_enabled = false;
  for (auto o : vector_options()) {
    if (options->at(o)->getType() == Option::INT)
      options->at(o)->setInt(0);
  }
  std::string msg = "Vector support disabled: " + reason;
  std::cout << msg << std::endl;
  thd->ddl_logs << msg << std::endl;
}

/* decide if the run uses vector tables. Probe the server for HNSW support
 * unless vector is already turned off by the options */
static void setup_vector(Thd1 *thd) {
  /* checked before disable_vector() zeroes it. Above the server maximum the
   * CREATE TABLE of a vector table fails and the initial load aborts */
  if (options->at(Option::VECTOR_MAX_DIM)->getInt() < 1 ||
      options->at(Option::VECTOR_MAX_DIM)->getInt() > MAX_VECTOR_DIMENSIONS)
    throw std::runtime_error(
        "invalid range for --vector-max-dim. Choose between 1 and " +
        std::to_string(MAX_VECTOR_DIMENSIONS));

  std::string engine = opt_string(ENGINE);
  std::transform(engine.begin(), engine.end(), engine.begin(), ::toupper);

  if (options->at(Option::NO_VECTOR)->getBool()) {
    disable_vector(thd, "--no-vector");
    return;
  }
  if (engine.compare("INNODB") != 0) {
    disable_vector(thd, "engine " + engine + " is not InnoDB");
    return;
  }
  if (options->at(Option::ONLY_TEMPORARY)->getBool()) {
    disable_vector(thd, "--only-temp-tables");
    return;
  }
  if (options->at(Option::ONLY_PARTITION)->getBool()) {
    disable_vector(thd, "--only-partition-tables");
    return;
  }

  /* mysql_read_single_value() returns an empty string on error */
  auto ef_search =
      mysql_read_single_value("SELECT @@innodb_hnsw_ef_search", thd);
  auto distance = mysql_read_single_value(
      "SELECT DISTANCE(TO_VECTOR('[1]'),TO_VECTOR('[1]'),'EUCLIDEAN')", thd);
  if (!ef_search.empty() && !distance.empty()) {
    g_vector_enabled = true;
    thd->ddl_logs << "Vector support enabled" << std::endl;
    return;
  }

  g_vector_probe_failed = true;
  for (auto o : vector_options()) {
    if (options->at(o)->cl)
      throw std::runtime_error(
          "--" + std::string(options->at(o)->getName()) +
          " was given, but the server has no HNSW vector index support");
  }
  disable_vector(thd, "the server has no HNSW vector index support");
}

/* sum the final workload weights without repeating startup setup */
static int sum_sql_option_weights() {
  int total = 0;
  for (auto opt : *options) {
    if (opt != nullptr && opt->sql)
      total += opt->getInt();
  }
  if (total == 0)
    throw std::runtime_error("no option selected");
  return total;
}

/* return probabality of all options and disable some feature based on user
 * request/ branch/ fork */
int sum_of_all_options(Thd1 *thd) {

  /* must run before any option is disabled or summed below */
  setup_vector(thd);

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

  if (options->at(Option::ONLY_PARTITION)->getBool()) {
    options->at(Option::NO_TEMPORARY)->setBool("true");
    options->at(Option::PARTITION_PROB)->setInt(100);
  }

  if (options->at(Option::ONLY_TEMPORARY)->getBool()) {
    options->at(Option::NO_PARTITION)->setBool("true");
    options->at(Option::TEMPORARY_PROB)->setInt(100);
  }

  /* if select is set as zero, disable all type of selects */
  if (options->at(Option::NO_SELECT)->getBool()) {
    options->at(Option::SELECT_ALL_ROW)->setInt(0);
    options->at(Option::SELECT_ROW_USING_PKEY)->setInt(0);
    options->at(Option::SELECT_VECTOR_ANN)->setInt(0);
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

  for (auto &opt : *options) {
    if (opt == nullptr)
      continue;
    if (opt->getType() == Option::INT)
      thd->thread_log << opt->getName() << "=>" << opt->getInt() << std::endl;
    else if (opt->getType() == Option::BOOL)
      thd->thread_log << opt->getName() << "=>" << opt->getBool() << std::endl;
  }

  return sum_sql_option_weights();
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
  else if (type.compare("VECTOR") == 0)
    return VECTOR;
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
  case VECTOR:
    return "VECTOR";
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
  case Column::COLUMN_TYPES::VECTOR:
  case Column::COLUMN_TYPES::COLUMN_MAX:
    throw std::runtime_error("unhandled " + Column::col_type_to_string(type_) +
                             " at line " + std::to_string(__LINE__));
  }
  return "";
}

/* return random value of  a column*/
std::string Column::rand_value() { return rand_value_universal(type_, length); }

/* return random value of sub string */
std::string Generated_Column::rand_value() {
  return rand_value_universal(g_type, length);
}

/* range of vector components */
static const float g_vector_range = 100;
/* number of vectors in the duplicate pool and of cluster centres */
static const int g_vector_pool_size = 8;
static const int g_vector_clusters = 4;

/* new vector column, random dimension */
Vector_Column::Vector_Column(std::string name, Table *table)
    : Column(name, table, Column::VECTOR) {
  int max_dim = options->at(Option::VECTOR_MAX_DIM)->getInt();
  dim = rand_int(max_dim < 1 ? 1 : max_dim, 1);
  null = true; // NOT NULL, required by the HNSW index
  seed_base = std::hash<std::string>{}(table->name_);
}

/* constructor used by load_metadata */
Vector_Column::Vector_Column(std::string name, Table *table, int dim_)
    : Column(table, Column::VECTOR), dim(dim_) {
  name_ = name;
  seed_base = std::hash<std::string>{}(table->name_);
}

std::vector<float> Vector_Column::rand_vector() const {
  std::uniform_real_distribution<float> dis(-g_vector_range, g_vector_range);
  std::vector<float> values(dim);
  for (auto &v : values)
    v = dis(rng);
  return values;
}

std::vector<float> Vector_Column::seeded_vector(int n) const {
  std::mt19937 gen(static_cast<std::mt19937::result_type>(seed_base + n));
  std::uniform_real_distribution<float> dis(-g_vector_range, g_vector_range);
  std::vector<float> values(dim);
  for (auto &v : values)
    v = dis(gen);
  return values;
}

std::string Vector_Column::to_literal(const std::vector<float> &values) {
  std::ostringstream out;
  out << std::setprecision(6);
  out << (rand_int(9) == 0 ? "TO_VECTOR('[" : "STRING_TO_VECTOR('[");
  for (size_t i = 0; i < values.size(); i++) {
    if (i > 0)
      out << ",";
    out << values[i];
  }
  out << "]')";
  return out.str();
}

bool Vector_Column::parse(const std::string &text,
                          std::vector<float> &values) {
  values.clear();
  auto begin = text.find('[');
  auto end = text.rfind(']');
  if (begin == std::string::npos || end == std::string::npos || end <= begin)
    return false;
  std::stringstream ss(text.substr(begin + 1, end - begin - 1));
  std::string item;
  while (std::getline(ss, item, ',')) {
    char *rest = nullptr;
    float v = std::strtof(item.c_str(), &rest);
    if (rest == item.c_str())
      return false;
    values.push_back(v);
  }
  return values.size() > 0;
}

std::string Vector_Column::literal_with_noise(std::vector<float> values,
                                              float noise) {
  std::uniform_real_distribution<float> dis(-noise, noise);
  for (auto &v : values)
    v += dis(rng);
  return to_literal(values);
}

std::string Vector_Column::uniform_vector_literal() {
  return to_literal(rand_vector());
}

std::string Vector_Column::normal_vector_literal() {
  std::normal_distribution<float> dis(0.0f, g_vector_range / 3.0f);
  std::vector<float> values(dim);
  for (auto &v : values)
    v = dis(rng);
  return to_literal(values);
}

std::string Vector_Column::zero_vector_literal() {
  return to_literal(std::vector<float>(dim, 0));
}

std::string Vector_Column::rand_vector_literal() {
  auto prob = rand_int(99);
  if (prob < 35)
    return uniform_vector_literal();
  if (prob < 50)
    return normal_vector_literal();
  if (prob < 65)
    return to_literal(seeded_vector(rand_int(g_vector_pool_size - 1)));
  if (prob < 68)
    return zero_vector_literal();
  /* cluster: centre plus noise */
  auto values = seeded_vector(g_vector_pool_size +
                              rand_int(g_vector_clusters - 1));
  std::uniform_real_distribution<float> dis(-1, 1);
  for (auto &v : values)
    v += dis(rng);
  return to_literal(values);
}

std::string Vector_Column::rand_value() {
  /* DML always uses the modeled dimension: an index may be dropped before
   * execution, allowing a short vector to be stored. Invalid dimensions are
   * exercised by ANN queries, which cannot leave such rows behind. */
  return rand_vector_literal();
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
  case VECTOR:
    name_ = "e" + name;
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

bool Generated_Column::has_base_column(const Table *table) {
  for (auto col : *table->columns_) {
    if (!col->auto_increment && col->type_ != GENERATED && col->type_ != VECTOR)
      return true;
  }
  return false;
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
        table->columns_->at(col)->type_ != GENERATED &&
        table->columns_->at(col)->type_ != VECTOR)
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
      case VECTOR:
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
  writer.String("unsigned_big");
  writer.Bool(unsigned_big);
}

/* add dimension in metadata */
template <typename Writer> void Vector_Column::Serialize(Writer &writer) const {
  writer.String("dim");
  writer.Int(dim);
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
  writer.String("kind");
  std::string k = kind_to_string(kind);
  writer.String(k.c_str(), static_cast<SizeType>(k.length()));
  writer.String("m");
  writer.Int(m);
  writer.String("metric");
  writer.String(metric.c_str(), static_cast<SizeType>(metric.length()));
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
  } else if (type == FK) {
    auto fk_table = static_cast<const FK_table *>(this);
    std::string parent = fk_table->parent->name_;
    std::string on_update = fk_table->enumToString(fk_table->on_update);
    std::string on_delete = fk_table->enumToString(fk_table->on_delete);
    writer.String("parent");
    writer.String(parent.c_str(), static_cast<SizeType>(parent.length()));
    writer.String("on_update");
    writer.String(on_update.c_str(), static_cast<SizeType>(on_update.length()));
    writer.String("on_delete");
    writer.String(on_delete.c_str(), static_cast<SizeType>(on_delete.length()));
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
    } else if (col->type_ == Column::VECTOR) {
      static_cast<Vector_Column *>(col)->Serialize(writer);
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

const std::string Index::kind_to_string(KIND kind) {
  switch (kind) {
  case REGULAR:
    return "REGULAR";
  case HNSW:
    return "HNSW";
  }
  return "FAIL";
}

Index::KIND Index::string_to_kind(const std::string &str) {
  if (str.compare("REGULAR") == 0)
    return REGULAR;
  if (str.compare("HNSW") == 0)
    return HNSW;
  throw std::runtime_error("unhandled index kind " + str);
}

std::string Index::hnsw_type_clause() {
  std::string def = rand_int(1) == 0 ? " TYPE " : " USING ";
  def += rand_int(3) == 0 ? "HNSW" : "hnsw";
  std::vector<std::string> opts;
  if (m > 0)
    opts.push_back("M = " + std::to_string(m));
  if (!metric.empty())
    opts.push_back("metric = " + metric);
  if (opts.size() == 2 && rand_int(1) == 0)
    std::swap(opts[0], opts[1]);
  if (!opts.empty()) {
    def += " (";
    for (size_t i = 0; i < opts.size(); i++)
      def += (i > 0 ? ", " : "") + opts[i];
    def += ")";
  }
  return def;
}

/* index definition */
std::string Index::definition() {
  std::string def;
  if (kind == HNSW) {
    /* VECTOR KEY|INDEX name (col) TYPE|USING hnsw [(M = m, metric = x)] */
    def += rand_int(1) == 0 ? "VECTOR KEY " : "VECTOR INDEX ";
    def += name_ + " (" + columns_->at(0)->column->name_ + ")";
    def += hnsw_type_clause() + " ";
    return def;
  }
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
    if (!InsertBulkRecord(thd))
      return false;
  }

  thd->ddl_query = true;
  if (!load_secondary_indexes(thd)) {
    return false;
  }

  if (this->type == Table::TABLE_TYPES::FK) {
    if (!static_cast<FK_table *>(this)->load_fk_constraint(thd)) {
      return false;
    }
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

  for (size_t i = 0; i < indexes_->size(); i++) {
    auto id = indexes_->at(i);
    if (i == auto_inc_index)
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

bool FK_table::load_fk_constraint(Thd1 *thd) {

  std::string constraint = name_ + "_" + parent->name_;
  std::string pk;
  for (const auto &col : *parent->columns_) {
    if (col->primary_key == true) {
      pk = col->name_;
      break;
    }
  }
  assert(pk.size() > 0);

  std::string sql = "ALTER TABLE " + name_ + " ADD CONSTRAINT " + constraint +
                    " FOREIGN KEY (ifk_col) REFERENCES " + parent->name_ +
                    " (" + pk + ")";
  sql += " ON UPDATE " + enumToString(on_update);
  sql += " ON DELETE  " + enumToString(on_delete);

  if (!execute_sql(sql, thd)) {
    thd->thread_log << "Failed to add fk constraint "
                    << " on " << name_ << std::endl;
    run_query_failed = true;
    return false;
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
  std::unique_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  if (type == VECTOR && !schema_lock.try_lock())
    return;

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
    execute_sql("ALTER TABLE " + name_ + algorithm_lock() +
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
      if (execute_sql("ALTER TABLE " + name_ + algorithm_lock() +
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
        if (execute_sql("ALTER TABLE " + name_ + algorithm_lock() +
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
      if (execute_sql("ALTER TABLE " + name_ + algorithm_lock() +
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
  if (type == FK) {
    std::string name = "fk_col";
    Column::COLUMN_TYPES type = Column::INTEGER;
    AddInternalColumn(new Column{name, this, type});
  }

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

  CreateRandomColumns(true, false);
}

/* add random columns */
void Table::CreateRandomColumns(bool pk_allowed, bool has_auto_increment) {
  auto no_auto_inc = opt_bool(NO_AUTO_INC);

  /* create normal column */
  static auto max_col = opt_int(COLUMNS);

  auto max_columns = rand_int(max_col, 1);

  for (int i = 0; i < max_columns; i++) {
    std::string name;
    Column::COLUMN_TYPES type;
    Column *col;
    /*  if we need to create primary column */

    /* First column can be primary */
    if (pk_allowed && i == 0 &&
        rand_int(100) <= options->at(Option::PRIMARY_KEY)->getInt()) {
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
        if (!no_virtual_col && i >= .8 * max_columns && rand_int(1) == 1 &&
            Generated_Column::has_base_column(this))
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

    /* compressed and vector columns can't be in a regular index */
    int number_of_compressed = 0;

    for (auto column : *columns_)
      if (column->compressed || column->type_ == Column::VECTOR)
        number_of_compressed++;

    size_t number_of_columns = columns_->size() - number_of_compressed;

    /* only compressed or vector columns */
    if (number_of_columns == 0)
      return;

    number_of_columns = rand_int(
        (max_columns < number_of_columns ? max_columns : number_of_columns), 1);

    std::vector<int> col_pos; // position of columns

    /* pick some columns */
    while (col_pos.size() < number_of_columns) {
      int current = rand_int(columns_->size() - 1);
      if (columns_->at(current)->compressed ||
          columns_->at(current)->type_ == Column::VECTOR)
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

/* primary key, vector column, then the usual random columns */
void Vector_table::CreateDefaultColumn() {
  auto no_auto_inc = opt_bool(NO_AUTO_INC);

  auto pk = new Column{"pkey", this, Column::INT};
  pk->primary_key = true;
  pk->unsigned_big = true;
  pk->length = 0;
  if (!no_auto_inc && rand_int(3) < 3)
    pk->auto_increment = true;
  AddInternalColumn(pk);

  AddInternalColumn(new Vector_Column("vec", this));

  CreateRandomColumns(false, pk->auto_increment);
}

/* the usual regular indexes plus the HNSW index */
void Vector_table::CreateDefaultIndex() {
  Table::CreateDefaultIndex();

  /* the regular index creation can stop early, the auto_inc index must then
   * not point to the HNSW index */
  if (auto_inc_index >= indexes_->size())
    auto_inc_index = std::numeric_limits<size_t>::max();

  auto index = new_hnsw_index(name_ + "hnsw");
  if (index != nullptr)
    AddInternalIndex(index);
}

Column *Vector_table::pk_column() const {
  for (auto col : *columns_) {
    if (col->primary_key)
      return col;
  }
  return nullptr;
}

Vector_Column *Vector_table::vector_column() const {
  for (auto col : *columns_) {
    if (col->type_ == Column::VECTOR)
      return static_cast<Vector_Column *>(col);
  }
  return nullptr;
}

Index *Vector_table::new_hnsw_index(const std::string &name) const {
  auto col = vector_column();
  if (col == nullptr)
    return nullptr;
  auto index = new Index(name);
  index->kind = Index::HNSW;
  index->AddInternalColumn(new Ind_col(col, false));
  /* 30% use the server default M */
  auto prob = rand_int(99);
  if (prob < 30)
    index->m = 0;
  else if (prob < 35)
    index->m = rand_int(200, 2);
  else
    index->m = rand_int(32, 2);
  if (rand_int(2) == 0)
    index->metric = "euclidean";
  return index;
}

bool Vector_table::can_drop_column(const Column *col) const {
  return !col->primary_key && col->type_ != Column::VECTOR;
}

bool Vector_table::can_modify_column(const Column *col) const {
  return !col->primary_key;
}

/* " LOCK=x ALGORITHM=y" of CREATE INDEX and DROP INDEX: no comma, each part
 * can be left out and the order is random */
static std::string index_algorithm_lock(Table *table) {
  std::string algo;
  std::string lock;
  table->algorithm_lock(&algo, &lock);
  std::vector<std::string> parts;
  if (rand_int(3) > 0)
    parts.push_back(" LOCK=" + lock);
  if (rand_int(3) > 0)
    parts.push_back(" ALGORITHM=" + algo);
  if (parts.size() == 2 && rand_int(1) == 0)
    std::swap(parts[0], parts[1]);
  std::string str;
  for (auto &part : parts)
    str += part;
  return str;
}

void Vector_table::AddDropHnswIndex(Thd1 *thd) {
  std::unique_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  if (type == VECTOR && !schema_lock.try_lock())
    return;

  std::unique_lock<std::mutex> metadata_lock(table_mutex);
  auto index = hnsw_index();

  if (index != nullptr) {
    auto name = index->name_;
    std::string sql;
    if (rand_int(1) == 0)
      sql = "ALTER TABLE " + name_ + " DROP " +
            (rand_int(1) == 0 ? "INDEX " : "KEY ") + name + "," +
            algorithm_lock();
    else
      sql = "DROP INDEX " + name + " ON " + name_ + index_algorithm_lock(this);
    metadata_lock.unlock();

    thd->success = false;
    if (execute_sql(sql, thd)) {
      metadata_lock.lock();
      for (size_t i = 0; i < indexes_->size(); i++) {
        auto ix = indexes_->at(i);
        if (ix->name_.compare(name) == 0) {
          delete ix;
          indexes_->at(i) = indexes_->back();
          indexes_->pop_back();
          break;
        }
      }
    }
    return;
  }

  /* a name no other index of the model has, so that a drop by name never
   * removes the wrong model entry */
  std::string name;
  for (int i = 0; i < 10 && name.empty(); i++) {
    name = name_ + "hnsw" + std::to_string(rand_int(1000));
    for (auto ix : *indexes_) {
      if (ix->name_.compare(name) == 0) {
        name.clear();
        break;
      }
    }
  }

  index = name.empty() ? nullptr : new_hnsw_index(name);
  if (index == nullptr)
    return;

  /* the HNSW index needs a VECTOR NOT NULL column */
  auto col = vector_column();
  bool make_not_null = !col->null;

  std::string sql;
  if (make_not_null || rand_int(1) == 0) {
    sql = "ALTER TABLE " + name_ + " ADD " + index->definition();
    if (make_not_null)
      sql += ", MODIFY COLUMN " + col->name_ + " VECTOR(" +
             std::to_string(col->dim) + ") NOT NULL";
    sql += "," + algorithm_lock();
  } else {
    sql = "CREATE VECTOR INDEX " + name + " ON " + name_ + " (" + col->name_ +
          ")" + index->hnsw_type_clause() + index_algorithm_lock(this);
  }
  metadata_lock.unlock();

  thd->success = false;
  if (execute_sql(sql, thd)) {
    metadata_lock.lock();
    bool do_not_add = hnsw_index() != nullptr;
    for (auto ix : *indexes_) {
      if (ix->name_.compare(name) == 0)
        do_not_add = true;
    }
    if (do_not_add)
      delete index;
    else
      AddInternalIndex(index);
    if (make_not_null) {
      col = vector_column();
      if (col != nullptr)
        col->null = true;
    }
  } else {
    delete index;
  }
}

void Vector_table::ModifyVectorColumn(Thd1 *thd) {
  std::unique_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  if (type == VECTOR && !schema_lock.try_lock())
    return;

  static auto max_dim = opt_int(VECTOR_MAX_DIM);
  std::unique_lock<std::mutex> metadata_lock(table_mutex);
  auto col = vector_column();
  if (col == nullptr)
    return;

  /* Publish the dimension only after the ALTER succeeds. The schema guard
   * prevents vector DML from using it until the model is updated. */
  auto index = hnsw_index();
  bool has_index = index != nullptr;
  int dim = col->dim;
  bool not_null = col->null;

  /* about 1 in 10 changes the dimension, which only succeeds on an empty
   * table. With an HNSW index the server refuses a vector with fewer
   * dimensions than the column and a longer one is always refused, so any
   * new dimension works. Without the index a larger dimension would succeed
   * on a non-empty table and leave short vectors behind, which every later
   * ADD VECTOR INDEX refuses, so only a smaller one is used. The schema
   * guard keeps the index state unchanged until the ALTER completes. */
  if (rand_int(9) == 0) {
    if (has_index)
      dim = rand_int(max_dim < 1 ? 1 : max_dim, 1);
    else if (dim > 1)
      dim = rand_int(dim - 1, 1);
  }

  /* NULL is refused with an HNSW index. A nullable column goes back to NOT
   * NULL more often, so that the index can be added again */
  if (!has_index) {
    if (!not_null)
      not_null = rand_int(1) == 0;
    else if (rand_int(4) == 0)
      not_null = false;
  }

  std::string algo;
  std::string lock;
  algorithm_lock(&algo, &lock);
  /* a type change can't be INPLACE */
  if (dim != col->dim && algo.compare("INPLACE") == 0)
    algo = rand_int(1) == 0 ? "COPY" : "DEFAULT";

  std::string sql = "ALTER TABLE " + name_ + " MODIFY COLUMN " + col->name_ +
                    " VECTOR(" + std::to_string(dim) + ")" +
                    (not_null ? " NOT NULL" : "");
  sql += ", LOCK=" + lock + ", ALGORITHM=" + algo;
  metadata_lock.unlock();

  thd->success = false;
  if (execute_sql(sql, thd)) {
    metadata_lock.lock();
    col = vector_column();
    if (col != nullptr) {
      col->dim = dim;
      col->null = not_null;
    }
  }
}

std::string Table::algorithm_lock(std::string *const algo,
                                  std::string *const lock) {
  return pick_algorithm_lock(algo, lock);
}

std::string Vector_table::algorithm_lock(std::string *const algo,
                                         std::string *const lock) {
  std::string current_algo;
  std::string current_lock;
  pick_algorithm_lock(&current_algo, &current_lock);

  /* legal again once the HNSW index has been dropped */
  if (hnsw_index() != nullptr) {
    if (current_algo == "INSTANT")
      current_algo = rand_int(1) == 0 ? "INPLACE" : "DEFAULT";
    if (current_lock == "NONE")
      current_lock = rand_int(1) == 0 ? "SHARED" : "DEFAULT";
  }

  if (algo != nullptr)
    *algo = current_algo;
  if (lock != nullptr)
    *lock = current_lock;

  return " LOCK=" + current_lock + ", ALGORITHM=" + current_algo;
}

Vector_table *pick_vector_table() {
  if (!vector_enabled())
    return nullptr;

  int count = 0;
  for (auto table : *all_tables)
    if (table->type == Table::VECTOR)
      ++count;
  if (count == 0)
    return nullptr;

  /* One random draw preserves uniform selection and the run's RNG sequence. */
  int selected = rand_int(count - 1);
  for (auto table : *all_tables)
    if (table->type == Table::VECTOR && selected-- == 0)
      return static_cast<Vector_table *>(table);
  return nullptr;
}

/* Create new table and pick some attributes */
Table *Table::table_id(TABLE_TYPES type, int id) {
  Table *table;
  std::string name = TABLE_PREFIX + std::to_string(id);
  switch (type) {
  case PARTITION:
    table = new Partition(name + PARTITION_SUFFIX);
    break;
  case NORMAL:
    table = new Table(name);
    break;
  case TEMPORARY:
    table = new Temporary_table(name + TEMP_SUFFIX);
    break;
  default:
    throw std::runtime_error("Unhandle Table type");
  case FK:
    table = new FK_table(name + FK_SUFFIX);
    break;
  case VECTOR:
    table = new Vector_table(name + VECTOR_SUFFIX);
    break;
  }

  table->type = type;

  table->number_of_initial_records =
      options->at(Option::EXACT_INITIAL_RECORDS)->getBool()
          ? options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()
          : rand_int(options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt());
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

  /* with a vector index every later ALTER that keeps an explicit
   * KEY_BLOCK_SIZE is refused, so vector tables have none. That also rules
   * out the compressed general tablespaces. key_block_size > 0 is what set
   * ROW_FORMAT=COMPRESSED above, so clear that too */
  if (type == VECTOR && table->key_block_size > 0) {
    table->key_block_size = 0;
    table->tablespace.clear();
    table->row_format.clear();
  }

  static auto engine = options->at(Option::ENGINE)->getString();
  table->engine = engine;

  /* If indexes are disabled, also disable auto_inc */
  if (!options->at(Option::INDEXES)->getInt())
    options->at(Option::NO_AUTO_INC)->setBool(true);

  table->CreateDefaultColumn();
  table->CreateDefaultIndex();
  if (type == FK) {
    static_cast<FK_table *>(table)->pickRefrence(table);
  }

  return table;
}

/* check if table has a primary key */
bool Table::has_pk() const {
  for (const auto &col : *columns_) {
    if (col->primary_key)
      return true;
  }
  return false;
}

Index *Table::hnsw_index() const {
  for (auto index : *indexes_) {
    if (index->kind == Index::HNSW)
      return index;
  }
  return nullptr;
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
      if (type == FK) {
        auto fk = static_cast<FK_table *>(this);
        def += " FOREIGN KEY (ifk_col) REFERENCES " + fk->parent->name_ +
               " (ipkey) ";
        def += " ON UPDATE " + fk->enumToString(fk->on_update) + " ON DELETE " +
            fk->enumToString(fk->on_delete);
        def += ", ";
      }
    }

  } else {
    /* only load autoinc */
    if (auto_inc_index < indexes_->size()) {
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
      if (!options->at(Option::ONLY_PARTITION)->getBool()) {
        auto parent_table = Table::table_id(Table::NORMAL, i);
        all_tables->push_back(parent_table);

        /* Create FK table */
        if (!options->at(Option::NO_FK)->getBool() &&
            options->at(Option::FK_PROB)->getInt() > rand_int(100) &&
            parent_table->has_pk()) {
          auto child_table = Table::table_id(Table::FK, i);
          all_tables->push_back(child_table);
          static_cast<FK_table *>(child_table)->parent = parent_table;
        }
      }

      if (!options->at(Option::NO_PARTITION)->getBool() &&
          options->at(Option::PARTITION_PROB)->getInt() > rand_int(100))
        all_tables->push_back(Table::table_id(Table::PARTITION, i));

      /* vector_enabled() first, so the random sequence of a run without
       * vector support does not change */
      if (vector_enabled() &&
          options->at(Option::VECTOR_PROB)->getInt() > rand_int(100))
        all_tables->push_back(Table::table_id(Table::VECTOR, i));
      /*
      if (!options->at(Option::NO_FK)->getBool() &&
          options->at(Option::FK_PROB)->getInt() > rand_int(100))
        all_tables->push_back(Table::table_id(Table::FK, i));
        */
    }
  }
}

bool execute_sql(const std::string &sql, Thd1 *thd) {
  thd->action_executed_sql = true;
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

  if (res != 0) { // query failed
    thd->failed_queries_total++;
    thd->max_con_fail_count++;
    if (log_all || log_failed) {
      thd->thread_log << " F " << sql << std::endl;
      thd->thread_log << "Error " << mysql_error(thd->conn) << std::endl;
    }
    if (mysql_errno(thd->conn) == CR_SERVER_GONE_ERROR ||
        mysql_errno(thd->conn) == CR_SERVER_LOST) {
      thd->thread_log << "server gone, while processing " + sql << std::endl;
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
  /* algorithm_lock() reads the HNSW index on a vector table */
  table_mutex.lock();
  sql += enc + "'" + "," + algorithm_lock();
  table_mutex.unlock();
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
  table_mutex.lock();
  sql += comp + "'" + "," + algorithm_lock();
  table_mutex.unlock();
  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    compression = comp;
    table_mutex.unlock();
  }
}

void Table::SetAlterEngine(Thd1 *thd) {
  table_mutex.lock();
  std::string sql =
      "ALTER TABLE " + name_ + " ENGINE=InnoDB," + algorithm_lock();
  table_mutex.unlock();
  execute_sql(sql, thd);
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

  /* before a column is picked: DropColumn deletes columns while it holds
   * table_mutex, so waiting for it after the pick could leave col dangling */
  table_mutex.lock();
  std::string algo_lock = algorithm_lock();
  table_mutex.unlock();

  // try maximum 50 times to get a valid column
  int i = 0;
  while (i < 50 && col == nullptr) {
    auto col1 = columns_->at(rand_int(columns_->size() - 1));
    if (!can_modify_column(col1)) {
      i++;
      continue;
    }
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
    case Column::VECTOR:
      /* ModifyVectorColumn() takes table_mutex, which must not be taken
       * while holding a column mutex */
      if (type == VECTOR) {
        static_cast<Vector_table *>(this)->ModifyVectorColumn(thd);
        return;
      }
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

  sql += " " + col->definition() + "," + algo_lock;

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

  if (!can_drop_column(columns_->at(ps))) {
    table_mutex.unlock();
    return;
  }

  std::string sql = "ALTER TABLE " + name_ + " DROP COLUMN " + name + ",";

  sql += algorithm_lock();
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
      (columns_->size() == 1 && columns_->at(0)->auto_increment == true) ||
      !Generated_Column::has_base_column(this))
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
  std::string algo_lock = algorithm_lock(&algo);

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

  sql += algo_lock;

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
  std::unique_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  std::unique_lock<std::mutex> metadata_lock(table_mutex);
  if (indexes_ != nullptr && indexes_->size() > 0) {
    auto index = indexes_->at(rand_int(indexes_->size() - 1));
    if (type == VECTOR && index->kind == Index::HNSW &&
        !schema_lock.try_lock())
      return;
    auto name = index->name_;
    std::string sql = "ALTER TABLE " + name_ + " DROP INDEX " + name + ",";
    sql += algorithm_lock();
    metadata_lock.unlock();
    if (execute_sql(sql, thd)) {
      metadata_lock.lock();
      for (size_t i = 0; i < indexes_->size(); i++) {
        auto ix = indexes_->at(i);
        if (ix->name_.compare(name) == 0) {
          delete ix;
          indexes_->at(i) = indexes_->back();
          indexes_->pop_back();
          break;
        }
      }
    }
  } else {
    metadata_lock.unlock();
    thd->thread_log << "no index to drop " + name_ << std::endl;
  }
}

/*randomly add some index on the table */
void Table::AddIndex(Thd1 *thd) {
  auto i = rand_int(1000);
  auto id = std::make_unique<Index>(name_ + std::to_string(i));

  static size_t max_columns = opt_int(INDEX_COLUMNS);
  std::unique_lock<std::mutex> metadata_lock(table_mutex);

  /* vector columns can't be in a regular index */
  size_t index_columns = 0;
  for (auto col : *columns_)
    if (col->type_ != Column::VECTOR)
      index_columns++;

  if (index_columns == 0)
    return;

  /* number of columns to be added */
  int no_of_columns = rand_int(
      (max_columns < index_columns ? max_columns : index_columns), 1);

  std::vector<int> col_pos; // position of columns

  /* pick some columns */
  while (col_pos.size() < (size_t)no_of_columns) {
    int current = rand_int(columns_->size() - 1);
    if (columns_->at(current)->type_ == Column::VECTOR)
      continue;
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
  sql += algorithm_lock();
  metadata_lock.unlock();

  if (execute_sql(sql, thd)) {
    metadata_lock.lock();
    auto do_not_add = false; // check if there is already a index with this name
    for (auto ind : *indexes_) {
      if (ind->name_.compare(id->name_) == 0)
        do_not_add = true;
    }
    if (!do_not_add)
      AddInternalIndex(id.release());
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
  std::unique_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  std::unique_lock<std::mutex> metadata_lock(table_mutex);
  if (indexes_->size() == 0)
    metadata_lock.unlock();
  else {
    auto ps = rand_int(indexes_->size() - 1);
    auto index = indexes_->at(ps);
    if (type == VECTOR && index->kind == Index::HNSW &&
        !schema_lock.try_lock())
      return;
    auto name = index->name_;
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
    sql += algorithm_lock();
    metadata_lock.unlock();
    if (execute_sql(sql, thd)) {
      metadata_lock.lock();
      for (auto &ind : *indexes_) {
        if (ind->name_.compare(name) == 0)
          ind->name_ = new_name;
      }
    }
  }
}

void Table::ColumnRename(Thd1 *thd) {
  std::unique_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  std::unique_lock<std::mutex> metadata_lock(table_mutex);
  auto ps = rand_int(columns_->size() - 1);
  auto column = columns_->at(ps);
  if (type == VECTOR && column->type_ == Column::VECTOR &&
      !schema_lock.try_lock())
    return;
  auto name = column->name_;
  /* ALTER column to _rename or back to orignal_name */
  std::string new_name = "_rename";
  static auto s = new_name.size();
  if (name.size() > s && name.substr(name.length() - s).compare("_rename") == 0)
    new_name = name.substr(0, name.length() - s);
  else
    new_name = name + new_name;
  std::string sql = "ALTER TABLE " + name_ + " RENAME COLUMN " + name + " To " +
                    new_name + ",";
  sql += algorithm_lock();
  metadata_lock.unlock();
  if (execute_sql(sql, thd)) {
    metadata_lock.lock();
    for (auto &col : *columns_) {
      if (col->name_.compare(name) == 0)
        col->name_ = new_name;
    }
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
      /* a vector column is never a WHERE column */
      case Column::VECTOR:
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
    /* a vector column is never a WHERE column */
    case Column::VECTOR:
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
  std::shared_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  if (type == VECTOR && !schema_lock.try_lock())
    return;

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
    /* a vector column is never a WHERE column */
    case Column::VECTOR:
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

bool Table::InsertBulkRecord(Thd1 *thd) {
  bool is_list_partition = false;

  // if parent has no records, child can't have records
  if (type == FK) {
    if (static_cast<FK_table *>(this)->parent->number_of_initial_records == 0)
      number_of_initial_records = 0;
  }

  if (number_of_initial_records == 0)
    return true;

  std::string prepare_sql = "INSERT ";

  std::vector<int> fk_unique_keys;

  /* If a table has FK move its parent keys in fk_unique_keys */
  if (type == TABLE_TYPES::FK) {
    fk_unique_keys = std::move(thd->unique_keys);
  }
  if (has_pk()) {
    thd->unique_keys = generateUniqueRandomNumbers(number_of_initial_records);
  }

  /* ignore error in the case parition list  */
  if (type == PARTITION &&
      static_cast<Partition *>(this)->part_type == Partition::LIST) {
    is_list_partition = true;
  }

  if (is_list_partition)
    prepare_sql += "IGNORE ";

  prepare_sql += "INTO " + name_ + " (";

  assert(number_of_initial_records <=
         (g_integer_range *
          options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt()));

  for (const auto &column : *columns_) {
    prepare_sql += column->name_ + ", ";
  }

  prepare_sql.erase(prepare_sql.length() - 2);
  prepare_sql += ")";

  std::string values = " VALUES";
  int records = 0;

  while (records < number_of_initial_records) {
    std::string value = "(";
    for (const auto &column : *columns_) {
      /* For FK we get the unique value from the parent table unique vector */
      if (column->name_.find("fk_col") != std::string::npos) {
        value +=
            std::to_string(fk_unique_keys[rand_int(fk_unique_keys.size() - 1)]);
      } else if (column->type_ == Column::COLUMN_TYPES::GENERATED) {
        value += "DEFAULT";
      } else if (column->primary_key) {
        value += std::to_string(thd->unique_keys.at(records));
      } else if (column->auto_increment == true) {
        value += "NULL";
      } else if (column->type_ == Column::VECTOR) {
        /* the HNSW index is added after the load, so every value must have
         * the right dimension */
        value += static_cast<Vector_Column *>(column)->rand_vector_literal();
      } else if (is_list_partition && column->name_.compare("ip_col") == 0) {
        /* for list partition we insert only maximum possible value
         * todo modify rand_value to return list parititon range */
        value += std::to_string(
            rand_int(maximum_records_in_each_parititon_list *
                     options->at(Option::MAX_PARTITIONS)->getInt()));
      } else {
        value += column->rand_value();
      }

      value += ", ";
    }
    value.erase(value.size() - 2);
    value += ")";
    values += value;
    records++;
    if (values.size() > 1024 * 1024 || number_of_initial_records == records) {
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
  std::shared_lock<std::shared_mutex> schema_lock(vector_schema_mutex,
                                                  std::defer_lock);
  if (type == VECTOR && !schema_lock.try_lock())
    return;

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
  if (!can_discard_tablespace())
    return;
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
    } else if (table_type.compare("VECTOR") == 0) {
      table = new Vector_table(name);
    } else if (table_type == "FK") {
      std::string on_update = tab["on_update"].GetString();
      std::string on_delete = tab["on_delete"].GetString();
      std::string parent_name = tab["parent"].GetString();

      table = new FK_table(name, on_update, on_delete);
      for (auto &tbl : *all_tables) {
        if (tbl->name_ == parent_name) {
            static_cast<FK_table *>(table)->parent = tbl;
            break;
        }
      }
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
      } else if (type.compare("VECTOR") == 0) {
        a = new Vector_Column(col["name"].GetString(), table,
                              col["dim"].GetInt());
      } else
        throw std::runtime_error("unhandled column type");

      a->null = col["null"].GetBool();
      a->auto_increment = col["auto_increment"].GetBool();
      a->length = col["lenght"].GetInt(),
      a->primary_key = col["primary_key"].GetBool();
      a->compressed = col["compressed"].GetBool();
      a->unsigned_big = col["unsigned_big"].GetBool();
      table->AddInternalColumn(a);
    }

    for (auto &ind : tab["indexes"].GetArray()) {
      Index *index = new Index(ind["name"].GetString());
      index->kind = Index::string_to_kind(ind["kind"].GetString());
      index->m = ind["m"].GetInt();
      index->metric = ind["metric"].GetString();

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
  if (failures != 0) {
    check_failures++;
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

  bool has_vector_tables = false;
  for (auto table : *all_tables) {
    if (table->type != Table::VECTOR)
      continue;
    if (g_vector_probe_failed)
      throw std::runtime_error("the metadata has vector tables, but the server "
                               "has no HNSW vector index support");
    has_vector_tables = true;
    break;
  }

  /* with vector support off, disable_vector() already zeroed both options */
  if (vector_enabled() && !has_vector_tables) {
    opt_int_set(SELECT_VECTOR_ANN, 0);
    opt_int_set(ADD_DROP_VECTOR_INDEX, 0);
    thread_log << "No vector tables: select-vector-ann and "
                  "add-drop-vector-index disabled" << std::endl;
  }
  sum_of_all_opts = sum_sql_option_weights();

  return 1;
}

/* names of a vector table taken under table_mutex, so that ANN statements are
 * built and executed without holding the lock */
struct Ann_names {
  std::string table;
  std::string pk;
  std::string vec;
  /* name of the HNSW index, empty if the table has none */
  std::string hnsw;
};

static bool ann_names(Vector_table *table, Ann_names &names) {
  std::lock_guard<std::mutex> guard(table->table_mutex);
  auto pk = table->pk_column();
  auto vec = table->vector_column();
  if (pk == nullptr || vec == nullptr)
    return false;
  names.table = table->name_;
  names.pk = pk->name_;
  names.vec = vec->name_;
  auto index = table->hnsw_index();
  names.hnsw = index == nullptr ? "" : index->name_;
  return true;
}

/* --records, the upper bound of initial rows and of primary key values of
 * auto increment tables */
static int ann_records() {
  return options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt();
}

/* metric of DISTANCE(). The HNSW index only serves EUCLIDEAN and
 * EUCLIDEAN_SQUARED, the others are always answered by a scan */
static std::string ann_metric() {
  auto prob = rand_int(99);
  std::string metric;
  if (prob < 50)
    metric = "EUCLIDEAN";
  else if (prob < 90)
    metric = "EUCLIDEAN_SQUARED";
  else if (prob < 94)
    metric = "COSINE";
  else if (prob < 97)
    metric = "DOT";
  else
    metric = "MANHATTAN";
  /* metric names are case insensitive */
  if (rand_int(4) == 0)
    std::transform(metric.begin(), metric.end(), metric.begin(), ::tolower);
  return "'" + metric + "'";
}

/* query vector of an ANN statement. Caller holds table_mutex.
param[in] col   vector column
param[in] row   text form of an existing row's vector, empty if none was read
*/
static std::string ann_query_vector(Vector_Column *col,
                                    const std::string &row) {
  if (!row.empty()) {
    std::vector<float> values;
    /* the row can have fewer dimensions if it was stored while the table had
     * no HNSW index */
    if (!Vector_Column::parse(row, values) ||
        values.size() != static_cast<size_t>(col->dim))
      return col->uniform_vector_literal();
    static const float noise[] = {0, 0.001f, 0.1f, 1, 10};
    return col->literal_with_noise(std::move(values), noise[rand_int(4)]);
  }
  auto prob = rand_int(99);
  if (prob < 2)
    return "NULL";
  if (prob < 4) {
    /* wrong dimension, DISTANCE() fails */
    std::vector<float> values(col->dim > 1 && rand_int(1) == 0 ? col->dim - 1
                                                                : col->dim + 1,
                              1);
    return Vector_Column::to_literal(values);
  }
  if (prob < 10)
    return col->zero_vector_literal();
  if (prob < 40)
    return col->uniform_vector_literal();
  return col->rand_vector_literal();
}

/* condition for the WHERE clause of an ANN statement: a primary key range or
 * equality, another column, or the distance itself. Caller holds table_mutex.
param[in] table     vector table
param[in] alias     table alias or name used to qualify columns
param[in] distance  DISTANCE() call of the statement
*/
static std::string ann_condition(Vector_table *table, const std::string &alias,
                                 const std::string &distance) {
  auto pk = table->pk_column();
  auto prob = rand_int(99);
  if (prob < 35 && pk != nullptr) {
    auto name = alias + "." + pk->name_;
    auto value = pk->rand_value();
    switch (rand_int(3)) {
    case 0:
      return name + " > " + value;
    case 1:
      return name + " < " + value;
    case 2: {
      /* mostly a range that is not empty */
      auto low = rand_int(ann_records());
      auto high = low + rand_int(ann_records());
      if (rand_int(9) == 0)
        std::swap(low, high);
      return name + " BETWEEN " + std::to_string(low) + " AND " +
             std::to_string(high);
    }
    default:
      return name + " >= " + std::to_string(rand_int(ann_records()));
    }
  }
  if (prob < 40 && pk != nullptr)
    return alias + "." + pk->name_ + " = " + pk->rand_value();
  if (prob < 55)
    return distance + (rand_int(1) == 0 ? " < " : " <= ") +
           std::to_string(rand_int(300, 1));

  /* another column, never the vector column */
  std::vector<Column *> others;
  for (auto col : *table->columns_) {
    if (col->type_ != Column::VECTOR && !col->primary_key)
      others.push_back(col);
  }
  if (others.empty())
    return alias + "." + table->vector_column()->name_ + " IS NOT NULL";
  auto col = others.at(rand_int(others.size() - 1));
  auto name = alias + "." + col->name_;
  switch (rand_int(4)) {
  case 0:
  case 1:
    return name + " = " + col->rand_value();
  case 2:
    return name + " >= " + col->rand_value();
  case 3:
    return name + " IS NOT NULL";
  default:
    return name + " <> " + col->rand_value();
  }
}

/* LIMIT of an ANN statement: mostly small, sometimes above the default
 * innodb_hnsw_ef_search (40) or above --records. Tables often have fewer rows
 * than --records, so a LIMIT above ef_search is kept small too */
static std::string ann_limit() {
  auto prob = rand_int(99);
  int limit;
  if (prob < 75)
    limit = rand_int(10, 1);
  else if (prob < 88)
    limit = rand_int(200, 41);
  else
    limit = ann_records() + rand_int(1000, 1);
  if (rand_int(19) == 0)
    return " LIMIT " + std::to_string(rand_int(20)) + ", " +
           std::to_string(limit);
  return " LIMIT " + std::to_string(limit);
}

/* table to join with an ANN statement on its primary key: another vector
 * table, or the same one. Returns false if none can be used */
static bool ann_join_table(Ann_names &names) {
  auto table = pick_vector_table();
  return table != nullptr && ann_names(table, names);
}

/* ORDER BY DISTANCE(...) LIMIT k on a random vector table. Most statements
 * have the shape the HNSW index serves. Some have a shape it must not serve:
 * another metric, DESC, no LIMIT, a grouped or windowed query, or a hint that
 * turns the index off. The query vector is random, an existing row's vector,
 * with or without noise, the zero vector, NULL or of the wrong dimension, and
 * it is given as a literal, a user variable or a statement parameter */
void select_vector_ann(Thd1 *thd) {
  auto table = pick_vector_table();
  if (table == nullptr)
    return;
  Ann_names names;
  if (!ann_names(table, names))
    return;

  /* an existing row's vector, read before the statement */
  std::string row;
  if (rand_int(99) < 30) {
    std::string pk_value;
    {
      std::lock_guard<std::mutex> guard(table->table_mutex);
      auto pk = table->pk_column();
      pk_value = pk == nullptr ? "0" : pk->rand_value();
    }
    if (rand_int(1) == 0)
      pk_value = std::to_string(rand_int(ann_records()));
    row = mysql_read_single_value("SELECT FROM_VECTOR(" + names.vec +
                                      ") FROM " + names.table + " WHERE " +
                                      names.pk + " >= " + pk_value +
                                      " LIMIT 1",
                                  thd);
  }

  /* table to join with, picked before table_mutex is taken: two table
   * mutexes are never held at once */
  bool join = rand_int(99) < 8;
  Ann_names other;
  if (join && !ann_join_table(other))
    join = false;

  enum { PLAIN, SUBQUERY, PREPARED } form = PLAIN;
  auto form_prob = rand_int(99);
  if (form_prob < 10)
    form = SUBQUERY;
  else if (form_prob < 20)
    form = PREPARED;

  std::string alias = join || rand_int(3) == 0 ? "a" : names.table;
  std::string distance;
  std::string where;
  std::string set_variable;
  bool parameter = false;
  {
    std::lock_guard<std::mutex> guard(table->table_mutex);
    auto col = table->vector_column();
    if (col == nullptr)
      return;
    names.vec = col->name_;
    auto pk = table->pk_column();
    if (pk != nullptr)
      names.pk = pk->name_;
    auto index = table->hnsw_index();
    names.hnsw = index == nullptr ? "" : index->name_;
    auto query_vector = ann_query_vector(col, row);

    /* a statement parameter or a user variable holds the query vector */
    if (form == PREPARED && rand_int(1) == 0) {
      set_variable = "SET @ann_q = " + query_vector;
      query_vector = "?";
      parameter = true;
    } else if (rand_int(9) == 0) {
      set_variable = "SET @ann_q = " + query_vector;
      query_vector = "@ann_q";
    }

    auto column = alias + "." + names.vec;
    auto metric = ann_metric();
    distance = rand_int(1) == 0 ? "DISTANCE(" + column + ", " + query_vector +
                                      ", " + metric + ")"
                                : "DISTANCE(" + query_vector + ", " + column +
                                      ", " + metric + ")";
    if (rand_int(99) < 35) {
      where = " WHERE " + ann_condition(table, alias, distance);
      if (rand_int(9) == 0)
        where += " AND " + ann_condition(table, alias, distance);
    }
  }

  /* select list and ORDER BY */
  auto pk = alias + "." + names.pk;
  std::string select_list;
  std::string order_by;
  auto list_prob = rand_int(99);
  if (list_prob < 30) {
    select_list = pk;
    order_by = distance;
  } else if (list_prob < 45) {
    select_list = alias + ".*";
    order_by = distance;
  } else if (list_prob < 55) {
    select_list = pk + ", FROM_VECTOR(" + alias + "." + names.vec + ")";
    order_by = distance;
  } else {
    select_list = pk + ", " + distance + " AS dist";
    auto order_prob = rand_int(9);
    order_by = order_prob < 6 ? "dist" : order_prob < 8 ? "2" : distance;
  }

  /* shapes the index must not serve: about 10% DESC or no LIMIT, a few
   * grouped, windowed or DISTINCT queries */
  std::string limit = ann_limit();
  std::string group_by;
  auto shape_prob = rand_int(99);
  if (shape_prob < 5) {
    order_by += " DESC";
  } else if (shape_prob < 10) {
    limit = "";
  } else if (shape_prob < 11 && list_prob >= 55) {
    /* with DISTINCT the ORDER BY must be in the select list */
    select_list = "DISTINCT " + select_list;
  } else if (shape_prob < 12) {
    /* MIN() keeps this valid under ONLY_FULL_GROUP_BY. A grouped query is
     * not the ANN shape the HNSW index serves */
    select_list = pk + ", COUNT(*), MIN(" + distance + ") AS dist";
    group_by = " GROUP BY " + pk;
    order_by = "dist";
  } else if (shape_prob < 13) {
    select_list = pk + ", ROW_NUMBER() OVER () AS rn";
    order_by = distance;
  } else if (shape_prob < 15) {
    order_by += " ASC";
  }

  /* index hints */
  std::string hint;
  std::string index_hint;
  auto hint_prob = rand_int(99);
  if (hint_prob < 5) {
    hint = "/*+ NO_INDEX(" + alias +
           (names.hnsw.empty() || rand_int(1) == 0 ? "" : " " + names.hnsw) +
           ") */ ";
  } else if (!names.hnsw.empty() && hint_prob < 10) {
    index_hint = " IGNORE INDEX (" + names.hnsw + ")";
  } else if (!names.hnsw.empty() && hint_prob < 16) {
    index_hint = (rand_int(1) == 0 ? " FORCE INDEX (" : " USE INDEX (") +
                 names.hnsw + ")";
  }

  std::string from = names.table;
  if (alias != names.table)
    from += " AS " + alias;
  from += index_hint;
  if (join) {
    auto join_prob = rand_int(2);
    from += join_prob == 0   ? " JOIN "
            : join_prob == 1 ? " STRAIGHT_JOIN "
                             : " LEFT JOIN ";
    from += other.table + " AS b ON " + pk + " = b." + other.pk;
  }

  std::string sql = "SELECT " + hint + select_list + " FROM " + from + where +
                    group_by + " ORDER BY " + order_by + limit;

  if (form == SUBQUERY) {
    auto sub_prob = rand_int(3);
    if (sub_prob == 0)
      sql = "SELECT * FROM (" + sql + ") AS sq";
    else if (sub_prob == 1)
      sql = "SELECT COUNT(*) FROM (" + sql + ") AS sq";
    else if (sub_prob == 2)
      /* LIMIT is not allowed in an IN subquery, only in a derived table */
      sql = "SELECT " + names.pk + " FROM " + names.table + " WHERE " +
            names.pk + " IN (SELECT * FROM (SELECT " + pk + " FROM " + from +
            where + " ORDER BY " + distance + ann_limit() + ") AS sq)";
    else
      sql = "SELECT (SELECT " + pk + " FROM " + from + where + " ORDER BY " +
            distance + " LIMIT 1)";
  }

  if (!set_variable.empty())
    execute_sql(set_variable, thd);

  if (form != PREPARED) {
    /* helper statements do not count */
    thd->success = false;
    execute_sql(sql, thd);
    return;
  }

  std::string quoted;
  for (auto c : sql) {
    quoted += c;
    if (c == '\'')
      quoted += c;
  }
  if (!execute_sql("PREPARE ann_stmt FROM '" + quoted + "'", thd)) {
    thd->success = false;
    return;
  }
  /* executed more than once, a re-executed statement reopens the scan */
  thd->success = false;
  std::string execute = "EXECUTE ann_stmt";
  if (parameter) {
    /* one parameter per DISTANCE() call */
    std::string using_list;
    for (auto pos = sql.find(distance); pos != std::string::npos;
         pos = sql.find(distance, pos + distance.size()))
      using_list += using_list.empty() ? " USING @ann_q" : ", @ann_q";
    execute += using_list;
  }
  auto executions = rand_int(2, 1);
  for (int i = 0; i < executions; i++)
    execute_sql(execute, thd);
  auto success = thd->success;
  execute_sql("DEALLOCATE PREPARE ann_stmt", thd);
  thd->success = success;
}

/* set the session innodb_hnsw_ef_search, the minimum candidate list width of
 * HNSW searches */
void set_hnsw_ef_search(Thd1 *thd) {
  std::string value = rand_int(9) == 0 ? "DEFAULT"
                                        : std::to_string(rand_int(1000, 1));
  execute_sql("SET SESSION innodb_hnsw_ef_search = " + value, thd);
}

/* return true if successful or error out in case of fail */
bool Thd1::run_some_query() {
  /* VECTOR after FK: the FK table takes the keys of its parent from
   * thd->unique_keys, which every bulk load overwrites */
  std::vector<Table::TABLE_TYPES> tableTypes = {Table::NORMAL, Table::FK,
                                                Table::PARTITION,
                                                Table::VECTOR};
  execute_sql("USE " + options->at(Option::DATABASE)->getString(), this);

  /* first create temporary tables metadata if requried */
  int temp_tables;
  if (options->at(Option::ONLY_TEMPORARY)->getBool())
    temp_tables = options->at(Option::TABLES)->getInt();
  else if (options->at(Option::NO_TEMPORARY)->getBool())
    temp_tables = 0;
  else
    temp_tables = options->at(Option::TABLES)->getInt() /
                  options->at(Option::TEMPORARY_PROB)->getInt();

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

    while (current <= options->at(Option::TABLES)->getInt()) {
      /* first load normal table , then FK and then partition
       FK table uses thd->unique_key vector to pick random FK
       thd->unique_key is populated from primary key */

      for (const auto &tableType : tableTypes) {
        auto table = pick_table(tableType, current + 1);
        if (table == nullptr)
          continue;
        if (!table->load(this)) {
          return false;
        }
      table_completed++;
      }
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
    /* table initial data is created delete , empty the unique_keys */
    this->unique_keys.resize(0);

  } else if (options->at(Option::CHECK_TABLE_PRELOAD)->getBool()) {
    int number_of_tables = all_tables->size();
    auto current = table_started++;

    while (current < number_of_tables) {
      auto table = all_tables->at(current);
      if (table_enabled(table))
        check_tables_partitions_preload(table, this);
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

  /* keep disabled vector tables in saved metadata, but never select them
   * for generic DML/DDL or grammar SQL */
  for (auto table : *all_tables) {
    if (table_enabled(table))
      all_session_tables->push_back(table);
  }
  if (all_session_tables->empty()) {
    delete all_session_tables;
    throw std::runtime_error("no eligible tables to work on");
  }

  /* action counts per option for this thread */
  struct Action_counts {
    unsigned long total = 0;
    unsigned long successful = 0;
    unsigned long skipped = 0;
  };
  Action_counts opt_feq[Option::MAX];

  static auto savepoint_prob = options->at(Option::SAVEPOINT_PRB_K)->getInt();

  int trx_left = 0;
  int current_save_point = 0;
  while (std::chrono::system_clock::now() < end) {


    /* check if we need to make sql as part of existing or new trx */
    if (trx_left > 0) {
      trx_left--;
      if (trx_left == 0) {
        if (rand_int(100, 1) > options->at(Option::COMMIT_PROB)->getInt()) {
          execute_sql("ROLLBACK", this);
        } else {
          execute_sql("COMMIT", this);
        }
        current_save_point = 0;
      } else {
        if (rand_int(1000) < savepoint_prob) {
          current_save_point++;
          execute_sql("SAVEPOINT SAVE" + std::to_string(current_save_point),
                      this);
        }

        /* 10% chances of rollbacking to savepoint */
        if (current_save_point > 0 && rand_int(10) == 1) {
          auto sv = rand_int(current_save_point, 1);
          execute_sql("ROLLBACK TO SAVEPOINT SAVE" + std::to_string(sv), this);
          current_save_point = sv - 1;
        }
      }
    }

    if (trx_left == 0 &&
        rand_int(1000) < options->at(Option::TRANSATION_PRB_K)->getInt()) {
      execute_sql("START TRANSACTION", this);
      trx_left = rand_int(options->at(Option::TRANSACTIONS_SIZE)->getInt(), 1);
    }

    auto table =
        all_session_tables->at(rand_int(all_session_tables->size() - 1));
    auto option = pick_some_option();
    ddl_query = options->at(option)->ddl == true ? true : false;
    /* helper statements and skipped actions must not count as successes */
    success = false;
    action_executed_sql = false;

    switch (option) {
    case Option::DROP_INDEX:
      table->DropIndex(this);
      break;
    case Option::ADD_INDEX:
      table->AddIndex(this);
      break;
    case Option::ADD_DROP_VECTOR_INDEX: {
      /* a vector table, not the table picked above */
      auto vector_table = pick_vector_table();
      if (vector_table != nullptr)
        vector_table->AddDropHnswIndex(this);
      break;
    }
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
    case Option::ALTER_ENGINE:
      table->SetAlterEngine(this);
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
    case Option::SELECT_VECTOR_ANN:
      select_vector_ann(this);
      break;
    case Option::SET_HNSW_EF_SEARCH:
      set_hnsw_ef_search(this);
      break;

    default:
      throw std::runtime_error("invalid options");
    }

    options->at(option)->total_queries++;

    opt_feq[option].total++;
    if (!action_executed_sql) {
      options->at(option)->skipped_queries++;
      opt_feq[option].skipped++;
    } else if (success) {
      options->at(option)->success_queries++;
      opt_feq[option].successful++;
      success = false;
    }

    if (run_query_failed) {
      break;
    }
  } // while

  /* print options frequency in logs */
  for (int i = 0; i < Option::MAX; i++) {
    if (opt_feq[i].total > 0)
      thread_log << options->at(i)->help << ", total=>" << opt_feq[i].total
                 << ", success=> " << opt_feq[i].successful
                 << ", skipped=> " << opt_feq[i].skipped << std::endl;
  }

  /* cleanup session temporary tables tables */
  for (auto &table : *all_session_tables)
    if (table->type == Table::TEMPORARY)
      delete table;
  delete all_session_tables;
  return true;
}
