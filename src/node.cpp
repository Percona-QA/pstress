#include "node.hpp"
#include "common.hpp"
#include "random_test.hpp"
#include <cerrno>
#include <cstring>
#include <iostream>

Node::Node() {
  workers.clear();
  performed_queries_total = 0;
  failed_queries_total = 0;
}

void Node::end_node() {
  writeFinalReport();
  if (general_log)
    general_log.close();
  if (options->at(Option::PQUERY)->getBool() && querylist)
    delete querylist;
}

/* create the logdir if does not exist
@return false on error, true on success */

static bool create_log_dir() {
  std::string logdir_path(opt_string(LOGDIR));
  struct stat st;
  if (stat(logdir_path.c_str(),&st) != 0) {
    if (mkdir(logdir_path.c_str(), 0755) == -1)
      return false;
  }
  return true;
}

bool Node::createGeneralLog() {
  std::string logName;
  logName = myParams.logdir + "/" + myParams.myName + "_ddl_step_" + std::to_string(options->at(Option::STEP)->getInt()) + ".log";
  bool is_success = create_log_dir();
  if (!is_success) {
    std::cerr << "Could not create log dir: " << strerror(errno) << std::endl;
    return false;
  }
  general_log.open(logName, std::ios::out | std::ios::trunc);
  general_log << "- PStress v" << PQVERSION << "-" << PQREVISION
              << " compiled with " << FORK << "-" << mysql_get_client_info()
              << std::endl;

  if (!general_log.is_open()) {
    std::cout << "Unable to open log file " << logName << ": "
              << std::strerror(errno) << std::endl;
    return false;
  }
  return true;
}

void Node::writeFinalReport() {
  if (general_log.is_open()) {
    std::ostringstream exitmsg;
    exitmsg.precision(2);
    exitmsg << std::fixed;
    exitmsg << "* NODE SUMMARY: " << failed_queries_total << "/"
            << performed_queries_total << " queries failed, ("
            << (performed_queries_total - failed_queries_total) * 100.0 /
                   performed_queries_total
            << "% were successful)";
    general_log << exitmsg.str() << std::endl;
  }
}

int Node::startWork() {

  if (!createGeneralLog()) {
    std::cerr << "Exiting..." << std::endl;
    return 2;
  }

  std::cout << "- Connecting to " << myParams.myName << " [" << myParams.address
            << "]..." << std::endl;
  general_log << "- Connecting to " << myParams.myName << " ["
              << myParams.address << "]..." << std::endl;
  tryConnect();

  if (options->at(Option::PQUERY)->getBool()) {
    std::ifstream sqlfile_in;
    sqlfile_in.open(myParams.infile);

    if (!sqlfile_in.is_open()) {
      std::cerr << "Unable to open SQL file " << myParams.infile << ": "
                << strerror(errno) << std::endl;
      general_log << "Unable to open SQL file " << myParams.infile << ": "
                  << strerror(errno) << std::endl;
      return EXIT_FAILURE;
    }
    querylist = new std::vector<std::string>;
    std::string line;

    while (getline(sqlfile_in, line)) {
      if (!line.empty()) {
        querylist->push_back(line);
      }
    }

    sqlfile_in.close();
    general_log << "- Read " << querylist->size() << " lines from "
                << myParams.infile << std::endl;

    /* log replaying */
    if (options->at(Option::NO_SHUFFLE)->getBool()) {
      myParams.threads = 1;
      myParams.queries_per_thread = querylist->size();
    }
  }
  /* END log replaying */
  workers.resize(myParams.threads);

  for (int i = 0; i < myParams.threads; i++) {
    workers[i] = std::thread(&Node::workerThread, this, i);
  }

  for (int i = 0; i < myParams.threads; i++) {
    workers[i].join();
  }
  return EXIT_SUCCESS;
}

void Node::tryConnect() {
  MYSQL *conn;
  conn = mysql_init(NULL);
  if (conn == NULL) {
    std::cerr << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
              << std::endl;
    std::cerr << "* PSTRESS: Unable to continue [1], exiting" << std::endl;
    general_log << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
                << std::endl;
    general_log << "* PSTRESS: Unable to continue [1], exiting" << std::endl;
    mysql_close(conn);
    mysql_library_end();
    exit(EXIT_FAILURE);
  }
  if (mysql_real_connect(conn, myParams.address.c_str(),
                         myParams.username.c_str(), myParams.password.c_str(),
                         options->at(Option::DATABASE)->getString().c_str(),
                         myParams.port, myParams.socket.c_str(), 0) == NULL) {
    std::cerr << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
              << std::endl;
    std::cerr << "* PSTRESS: Unable to continue [2], exiting" << std::endl;
    general_log << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
                << std::endl;
    general_log << "* PSTRESS: Unable to continue [2], exiting" << std::endl;
    mysql_close(conn);
    mysql_library_end();
    exit(EXIT_FAILURE);
  }
  general_log << "- Connected to " << mysql_get_host_info(conn) << "..."
              << std::endl;
  // getting the real server version
  MYSQL_RES *result = NULL;
  std::string server_version;

  if (!mysql_query(conn, "select @@version_comment limit 1") &&
      (result = mysql_use_result(conn))) {
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row && row[0]) {
      server_version = mysql_get_server_info(conn);
      server_version.append(" ");
      server_version.append(row[0]);
    }
  } else {
    server_version = mysql_get_server_info(conn);
  }
  general_log << "- Connected server version: " << server_version << std::endl;
  if (strcmp(PLATFORM_ID,"Darwin") == 0)
    general_log << "- Table compression is disabled as hole punching is not supported on OSX"
                << std::endl;
  if (result != NULL) {
    mysql_free_result(result);
  }
  mysql_close(conn);
  mysql_thread_end();
  if (options->at(Option::TEST_CONNECTION)->getBool()) {
    exit(EXIT_SUCCESS);
  }
}

