#ifndef __RANDOM_HPP__
#define __RANDOM_HPP__

#include "common.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filereadstream.h>
#include <fstream>
#include <iostream>
#include <memory> //shared_ptr
#include <mutex>
#include <mysql.h>
#include <prettywriter.h>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <writer.h>
#define INNODB_16K_PAGE_SIZE 16
#define INNODB_8K_PAGE_SIZE 8
#define INNODB_32K_PAGE_SIZE 32
#define INNODB_64K_PAGE_SIZE 64
#define MAX_PATH 512
#define MIN_SEED_SIZE 10000
#define MAX_SEED_SIZE 100000
#define MAX_RANDOM_STRING_SIZE 32
#define DESC_INDEXES_IN_COLUMN 34
#define MYSQL_8 8.0

#define opt_int(a) options->at(Option::a)->getInt();
#define opt_int_set(a, b) options->at(Option::a)->setInt(b);
#define opt_bool(a) options->at(Option::a)->getBool();
#define opt_string(a) options->at(Option::a)->getString()

int rand_int(long int upper, long int lower = 0);
std::string rand_float(float upper, float lower = 0);
std::string rand_double(double upper, double lower = 0);
std::string rand_string(int upper, int lower = 2);

struct Table;
class Column {
public:
  enum COLUMN_TYPES {
    /* interger type columns are small ints. they are used for LIST
             PARTITION */
    INTEGER,
    INT,
    CHAR,
    VARCHAR,
    FLOAT,
    DOUBLE,
    BOOL,
    BLOB,
    BIT,
    GENERATED,
    DATE,
    DATETIME,
    TIMESTAMP,
    TEXT,
    COLUMN_MAX // should be last
  } type_;
  /* used to create new table/alter table add column*/
  Column(std::string name, Table *table, COLUMN_TYPES type);

  Column(Table *table, COLUMN_TYPES type);
  Column(std::string name, std::string type, Table *table)
      : type_(col_type(type)), name_(name), table_(table){};

  std::string definition();
  /* return random value of that column */
  std::string rand_value();
  /* return string to call type */
  static const std::string col_type_to_string(COLUMN_TYPES type);
  /* return column type from a string */
  COLUMN_TYPES col_type(std::string type);
  /* used to create_metadata */
  template <typename Writer> void Serialize(Writer &writer) const;
  /* return the clause of column */
  std::string rand_value_universal();

private:
  virtual std::string clause() {
    std::string str = col_type_to_string(type_);
    if (length > 0)
      str += "(" + std::to_string(length) + ")";
    return str;
  };

public:
  virtual ~Column(){};
  std::string name_;
  std::mutex mutex;
  bool null_val = true;
  int length = 0;
  std::string default_value;
  bool primary_key = false;
  bool auto_increment = false;
  bool compressed = false; // percona type compressed
  bool not_secondary = false;
  std::vector<int> unique_values;
  Table *table_;
  virtual bool is_col_string() {
    return type_ == COLUMN_TYPES::CHAR || type_ == COLUMN_TYPES::VARCHAR;
  }
  virtual bool is_col_number() {
    return type_ == COLUMN_TYPES::INT || type_ == COLUMN_TYPES::INTEGER;
  }
};

struct Blob_Column : public Column {
  Blob_Column(std::string name, Table *table);
  Blob_Column(std::string name, Table *table, std::string sub_type_);
  std::string sub_type; // sub_type can be tiny, medium, large blob
  std::string clause() { return sub_type; };
  template <typename Writer> void Serialize(Writer &writer) const;
  bool is_col_string() { return false; }
  bool is_col_number() { return false; }
};

struct Text_Column : public Column {
  Text_Column(std::string name, Table *table);
  Text_Column(std::string name, Table *table, std::string sub_type_);
  std::string sub_type; // sub_type can be tiny, medium, large text
  std::string clause() { return sub_type; };
  template <typename Writer> void Serialize(Writer &writer) const;
  bool is_col_string() { return true; }
  bool is_col_number() { return false; }
};

struct Generated_Column : public Column {

  /* constructor for new random generated column */
  Generated_Column(std::string name, Table *table);

  /* constructor used to prepare metadata */
  Generated_Column(std::string name, Table *table, std::string clause,
                   std::string sub_type);

  template <typename Writer> void Serialize(Writer &writer) const;

  std::string str;
  std::string clause() { return str; };
  ~Generated_Column(){};
  COLUMN_TYPES g_type; // sub type can be blob,int, varchar
  COLUMN_TYPES generate_type() const { return g_type; };
  bool is_col_string() {
    return g_type == COLUMN_TYPES::CHAR || g_type == COLUMN_TYPES::VARCHAR ||
           g_type == COLUMN_TYPES::BLOB || g_type == COLUMN_TYPES::TEXT;
  }
  bool is_col_number() {
    return g_type == COLUMN_TYPES::INT || g_type == COLUMN_TYPES::INTEGER;
  }
};

struct Ind_col {
  Ind_col(Column *c, bool d);
  template <typename Writer> void Serialize(Writer &writer) const;
  Column *column;
  bool desc = false;
  int length = 0;
};

struct Index {
  Index(std::string n, bool u = false);
  void AddInternalColumn(Ind_col *column);
  template <typename Writer> void Serialize(Writer &writer) const;
  ~Index();

  std::string definition();

  std::string name_;
  std::vector<Ind_col *> *columns_;
  bool unique;
};

struct Thd1 {
  Thd1(int id, std::ofstream &tl, std::ofstream &ddl_l, std::ofstream &client_l,
       MYSQL *c, std::atomic<unsigned long long> &p,
       std::atomic<unsigned long long> &f)
      : thread_id(id), thread_log(tl), ddl_logs(ddl_l), client_log(client_l),
        conn(c), performed_queries_total(p), failed_queries_total(f){};

  bool run_some_query(); // create default tables and run random queries
  bool load_metadata();  // load metada of tool in memory

  int thread_id;
  long int seed;
  std::ofstream &thread_log;
  std::ofstream &ddl_logs;
  std::ofstream &client_log;
  MYSQL *conn;
  std::atomic<unsigned long long> &performed_queries_total;
  std::atomic<unsigned long long> &failed_queries_total;
  std::shared_ptr<MYSQL_RES> result; // result set of sql
  bool ddl_query = false;            // is the query ddl
  bool success = false;              // if the sql is successfully executed
  int max_con_fail_count = 0;        // consecutive failed queries

  /* for loading Bulkdata, Primary key of current table is stored in this vector
   * which  is used for the FK tables  */
  std::vector<int> unique_keys;
  int query_number = 0;
  struct workerParams *myParam;
  bool tryreconnet();
};

/* Table basic properties */
struct Table {
  enum TABLE_TYPES { PARTITION, NORMAL, TEMPORARY, FK } type;

  Table(std::string n);
  static Table *table_id(TABLE_TYPES choice, int id, bool suffix = false);
  std::string definition(bool with_index = true, bool with_fk = true);
  /* add secondary indexes */
  bool load_secondary_indexes(Thd1 *thd);
  /* execute table definition, Bulk data and then secondary index */
  bool load(Thd1 *thd, bool bulk_insert = true,
            bool set_global_run_query_failed = true);
  /* methods to create table of choice */
  void AddInternalColumn(Column *column) { columns_->push_back(column); }
  void AddInternalIndex(Index *index) { indexes_->push_back(index); }
  virtual void CreateDefaultColumn();
  void CreateDefaultIndex();
  void CopyDefaultColumn(Table *table);
  void CopyDefaultIndex(Table *table);
  void DropCreate(Thd1 *thd);
  void Optimize(Thd1 *thd);
  void Analyze(Thd1 *thd);
  void Check(Thd1 *thd);
  void Truncate(Thd1 *thd);
  void SetEncryption(Thd1 *thd);
  void EnforceRebuildInSecondary(Thd1 *thd);
  void SetEncryptionInplace(Thd1 *thd);
  void SetTableCompression(Thd1 *thd);
  void ModifyColumn(Thd1 *thd);
  void InsertRandomRow(Thd1 *thd);
  void Compare_between_engine(const std::string &sql, Thd1 *thd);
  void InsertClause();
  bool InsertBulkRecord(Thd1 *thd);
  void DropColumn(Thd1 *thd);
  void AddColumn(Thd1 *thd);
  void ModifyColumnSecondaryEngine(Thd1 *thd);
  void DropIndex(Thd1 *thd);
  void AddIndex(Thd1 *thd);
  void Alter_discard_tablespace(Thd1 *thd);
  void DeleteRandomRow(Thd1 *thd);
  void UpdateRandomROW(Thd1 *thd);
  void SelectRandomRow(Thd1 *thd, bool select_for_update = false);
  void SelectAllRow(Thd1 *thd, bool select_for_update = false);
  void CreateFunction(Thd1 *thd);
  void SetSecondaryEngine(Thd1 *thd);
  std::string GetRandomPartition();
  Column *GetRandomColumn();
  std::string GetWherePrecise();
  std::string GetWhereBulk();
  std::string ColumnValues();
  std::string SelectColumn();
  std::string SetClause();
  void DeleteAllRows(Thd1 *thd);
  void UpdateAllRows(Thd1 *thd);
  void ColumnRename(Thd1 *thd);
  void IndexRename(Thd1 *thd);
  template <typename Writer> void Serialize(Writer &writer) const;
  virtual ~Table();

  std::string name_;
  std::string engine;
  std::string row_format;
  std::string tablespace;
  std::string compression;
  std::string encryption = "N";
  int key_block_size = 0;
  int number_of_initial_records;
  size_t auto_inc_index;
  // std::string data_directory; todo add corressponding code
  std::vector<Column *> *columns_;
  std::vector<Index *> *indexes_;
  mutable std::mutex table_mutex;
  mutable std::shared_mutex dml_mutex;

  const std::string get_type() const {
    switch (type) {
    case NORMAL:
      return "NORMAL";
      break;
    case PARTITION:
      return "PARTITION";
    case TEMPORARY:
      return "TEMPORARY";
    case FK:
      return "FK";
    }
    return "FAIL";
  };
  bool has_pk() const;

  void set_type(std::string s) {
    if (s.compare("PARTITION") == 0)
      type = PARTITION;
    else if (s.compare("NORMAL") == 0)
      type = NORMAL;
    else if (s.compare("TEMPORARY") == 0)
      type = TEMPORARY;
    else if (s.compare("FK") == 0)
      type = FK;
  };
};

/* Fk table */
struct FK_table : Table {
  FK_table(std::string n) : Table(n){};

  /* conustruct used for load_metadata */
  FK_table(std::string n, std::string on_update, std::string on_delete)
      : Table(n) {
    set_refrence(on_update, on_delete);
  }

  bool load_fk_constrain(Thd1 *thd, bool set_run_query_failed = true);

  std::string fk_constrain();

  void pickRefrence(Table *table) {
    on_delete = getRandomForeignKeyAction(table);
    on_update = getRandomForeignKeyAction(table);
  }

  enum class ForeignKeyAction {
    RESTRICT,
    SET_NULL,
    NO_ACTION,
    SET_DEFAULT,
    CASCADE
  };
  std::string enumToString(ForeignKeyAction value) const {
    switch (value) {
    case ForeignKeyAction::RESTRICT:
      return "RESTRICT";
    case ForeignKeyAction::CASCADE:
      return "CASCADE";
    case ForeignKeyAction::SET_NULL:
      return "SET NULL";
    case ForeignKeyAction::NO_ACTION:
      return "NO ACTION";
    case ForeignKeyAction::SET_DEFAULT:
      return "SET DEFAULT";
    default:
      return "Unknown";
    }
  }
  // Function to convert string to enum
  ForeignKeyAction stringToEnum(const std::string &str) {
    static const std::unordered_map<std::string, ForeignKeyAction> enumMap = {
        {"RESTRICT", ForeignKeyAction::RESTRICT},
        {"CASCADE", ForeignKeyAction::CASCADE},
        {"SET NULL", ForeignKeyAction::SET_NULL},
        {"NO ACTION", ForeignKeyAction::NO_ACTION},
        {"SET DEFAULT", ForeignKeyAction::SET_DEFAULT}};

    auto it = enumMap.find(str);
    if (it != enumMap.end()) {
      return it->second;
    }

    // Throw an exception if the string does not correspond to any enum value
    throw std::invalid_argument("Invalid enum value: " + str);
  }
  ForeignKeyAction on_update;
  ForeignKeyAction on_delete;

  ForeignKeyAction getRandomForeignKeyAction(Table *table) {
    /* if a table has virtual generated column and if any of  the base column
     * colum use set DEFAULT*/

    for (const auto &col : *table->columns_) {
      if (col->type_ == Column::COLUMN_TYPES::GENERATED) {
        const std::string &clause =
            static_cast<Generated_Column *>(col)->clause();
        if (clause.find("fk_col") != std::string::npos &&
            clause.find("STORED") != std::string::npos) {
          return ForeignKeyAction::SET_DEFAULT;
        }
      }
    }
    /* Grep any value upto cascade */
    int randomValue = rand_int(static_cast<int>(ForeignKeyAction::CASCADE));

    if (options->at(Option::NO_FK_CASCADE)->getBool()) {
      randomValue = rand_int(static_cast<int>(ForeignKeyAction::SET_DEFAULT));
    }
    return static_cast<ForeignKeyAction>(randomValue);
  }

  void set_refrence(std::string on_update_str, std::string on_delete_str) {
    on_update = stringToEnum(on_update_str);
    on_delete = stringToEnum(on_delete_str);
  }
};

/* Partition table */
struct Partition : public Table {
public:
  enum PART_TYPE { RANGE, LIST, HASH, KEY } part_type;

  Partition(std::string n);

  Partition(std::string n, std::string part_type_, int number_of_part_);

  /* add drop partitions */
  void AddDrop(Thd1 *thd);
  ~Partition() {}

  const std::string get_part_type() const {
    switch (part_type) {
    case LIST:
      return "LIST";
    case RANGE:
      return "RANGE";
    case HASH:
      return "HASH";
    case KEY:
      return "KEY";
    }
    return "FAIL";
  }

  void set_part_type(const std::string &sb_type) {
    if (sb_type.compare("LIST") == 0)
      part_type = LIST;
    else if (sb_type.compare("RANGE") == 0)
      part_type = RANGE;
    else if (sb_type.compare("HASH") == 0)
      part_type = HASH;
    else if (sb_type.compare("KEY") == 0)
      part_type = KEY;
  }
  int number_of_part;
  /* type of partition supported for current run */
  static std::vector<PART_TYPE> supported;
  /* how ranges are distributed */

  /* Used by Range Parititon */
  struct Range {
    Range(std::string n, int r) : name(n), range(r){};
    std::string name;
    int range;
  };
  std::vector<Range> positions;
  static bool compareRange(Range P1, Range P2) { return P1.range < P2.range; }

  /* Used by List Partition */
  struct List {
    List(std::string n) : name(n){};
    std::string name;
    std::vector<int> list;
  };
  std::vector<int> total_left_list;
  std::vector<List> lists;
};

/* Temporary table */
struct Temporary_table : Table {
  Temporary_table(std::string n) : Table(n){};
  Temporary_table(const Temporary_table &table) : Table(table.name_){};
};

int set_seed(Thd1 *thd);
int sum_of_all_options(Thd1 *thd);
int sum_of_all_server_options();
Option::Opt pick_some_option();
std::vector<std::string> *random_strs_generator(unsigned long int seed);
bool load_metadata(Thd1 *thd);

/* Execute SQL and update thd variables
param[in] sql	 	query that we want to execute
param[in/out] thd	Thd used to execute sql
*/
bool execute_sql(const std::string &sql, Thd1 *thd);

void save_metadata_to_file();
void clean_up_at_end();
void alter_tablespace_encryption(Thd1 *thd);
void alter_tablespace_rename(Thd1 *thd);
void set_mysqld_variable(Thd1 *thd);
void add_server_options(std::string str);
void alter_database_encryption(Thd1 *thd);
void create_in_memory_data();
void generate_metadata_for_tables();
void create_database_tablespace(Thd1 *thd);
/* Grammar table class used for parsing the grammar file */
struct grammar_table {
  grammar_table(std::string n)
      : name(n), column_count(grammar_table::MAX, 0),
        columns(grammar_table::MAX) {}
  std::string name;
  std::string found_name;
  std::vector<int> column_count;
  /* each pair contains random value*/
  std::vector<std::vector<std::pair<std::string, std::string>>> columns;
  bool table_found = false;
  enum sql_col_types {
    INT,
    CHAR,
    VARCHAR,
    DATETIME,
    DATE,
    TIMESTAMP,
    FLOAT,
    TEXT,
    MAX
  };
  static sql_col_types get_col_type(std::string type) {
    if (type == "INT")
      return INT;
    if (type == "CHAR")
      return CHAR;
    if (type == "VARCHAR")
      return VARCHAR;
    if (type == "DATETIME")
      return DATETIME;
    if (type == "DATE")
      return DATE;
    if (type == "TIMESTAMP")
      return TIMESTAMP;
    if (type == "FLOAT")
      return FLOAT;
    if (type == "TEXT")
      return TEXT;
    return MAX;
  }
  static std::string get_col_type(sql_col_types type) {
    switch (type) {
    case INT:
      return "INT";
    case VARCHAR:
      return "VARCHAR";
    case CHAR:
      return "CHAR";
    case DATETIME:
      return "DATETIME";
    case DATE:
      return "DATE";
    case TIMESTAMP:
      return "TIMESTAMP";
    case FLOAT:
      return "FLOAT";
    case TEXT:
      return "TEXT";
    case MAX:
      break;
    }
    return "";
  }
  int total_column_count() {
    int total = 0;
    for (auto &i : column_count)
      total += i;
    return total;
  }
  int total_column_written() {
    int total = 0;
    for (auto &i : columns)
      total += i.size();
    return total;
  }
  void reset_columns() {
    for (auto &i : columns) {
      i.clear();
    }
    found_name = "";
  }

  static std::vector<sql_col_types> get_vector_of_col_type() {
    std::vector<sql_col_types> all_types;
    for (int i = 0; i < MAX; i++) {
      all_types.push_back(static_cast<sql_col_types>(i));
    }
    return all_types;
  }
};
struct grammar_tables {
  /* use tables move constructor */
  grammar_tables(std::string sql_, std::vector<grammar_table> tables_)
      : sql(sql_), tables(tables_){};
  std::string sql;
  std::vector<grammar_table> tables;
};
#endif
