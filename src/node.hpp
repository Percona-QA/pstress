#ifndef __NODE_HPP__
#define __NODE_HPP__

#include "pstress.hpp"
#include "random_test.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#ifdef USE_MYSQL
#include <mysql.h>
#endif

#ifdef USE_DUCKDB
#include "duckdb.hpp"
#endif
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

/* struct for node setup */
struct workerParams {
  workerParams(short port_ = 0) {
    myName = "default.node.tld";
    database = opt_string(DATABASE);
    address = opt_string(ADDRESS);
    socket = opt_string(SOCKET);
    username = opt_string(USER);
    password = opt_string(PASSWORD);
    logdir = opt_string(LOGDIR);
    infile = opt_string(INFILE);
    threads = opt_int(THREADS);
    queries_per_thread = opt_int(QUERIES_PER_THREAD);
    port = port_;
  };
  std::string myName; // unique name for worker
  std::string database;
  std::string address;
  std::string socket;
  std::string username;
  std::string password;
  std::string infile;
  std::string logdir;
  short port;
  short threads;
  unsigned long queries_per_thread;
  unsigned long maxpacket;
};

enum LogLevel {
  LOG_NOTHING = 0,
  LOG_QUERY_NUMBERS = 1 << 0,
  LOG_CLIENT_OUTPUT = 1 << 1,
  LOG_QUERY_DURATION = 1 << 2,
  LOG_QUERY_STATISTICS = 1 << 3,
  LOG_FAILED_QUERIES = 1 << 4,
  LOG_SUCCEDED_QUERIES = 1 << 5,
  LOG_ALL_QUERIES = LOG_FAILED_QUERIES | LOG_SUCCEDED_QUERIES,
  LOG_N_QUERIES = 1 << 6, // New log enum
  LOG_CURRENT = LOG_NOTHING
};

/*
It represents standalone MySQL server or MySQL node in cluster (PXC) setup
*/

class Node {
public:
  Node();
  void end_node();
  void setAllParams(struct workerParams *Params) { myParams = *Params; }
  int startWork();

private:
  // declaration for worker thread function
  void workerThread(int);
  void tryConnect();
  #ifdef USE_MYSQL
    inline unsigned long long getAffectedRows(MYSQL *);
    void tryConnect(); // MySQL-specific connection logic
  #endif

  #ifdef USE_DUCKDB
    inline unsigned long long getAffectedRows(duckdb::Connection *);
    void tryConnect(); // DuckDB-specific connection logic
  #endif
  bool createGeneralLog();
  void readSettings(std::string);
  void writeFinalReport();

  std::vector<std::thread> workers;
  std::vector<std::string> *querylist;
  struct workerParams myParams;
  std::ofstream general_log;
  std::atomic<unsigned long long> performed_queries_total;
  std::atomic<unsigned long long> failed_queries_total;
};
#endif
