/*
 =========================================================
 #       Created by Alexey Bychko, Percona LLC           #
 #     Expanded by Roel Van de Paar, Percona LLC         #
 =========================================================
*/

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.hpp"
#include "node.hpp"
#include "pstress.hpp"
#include "random_test.hpp"
#include <INIReader.hpp>
#include <mysql.h>
#include <thread>
#include <string>
#include <libgen.h> //dirname() uses this

/* Global variable to hold pstress build directory path */
const char *binary_fullpath;

void read_section_settings(struct workerParams *wParams, std::string secName,
                           std::string confFile) {
  INIReader reader(confFile);
  wParams->myName = secName;
  wParams->socket = reader.Get(secName, "socket", "");
  wParams->address = reader.Get(secName, "address", "localhost");
  wParams->username = reader.Get(secName, "user", "test");
  wParams->password = reader.Get(secName, "password", "");
  wParams->database = reader.Get(secName, "database", "");
  wParams->port = reader.GetInteger(secName, "port", 3306);
  wParams->threads = reader.GetInteger(secName, "threads", 10);
  wParams->queries_per_thread =
      reader.GetInteger(secName, "queries-per-thread", 10000);
#ifdef MAXPACKET
  wParams->maxpacket =
      reader.GetInteger(secName, "max-packet-size", MAX_PACKET_DEFAULT);
#endif
  wParams->infile = reader.Get(secName, "infile", "pquery.sql");
  wParams->logdir = reader.Get(secName, "logdir", "/tmp");
}

void create_worker(struct workerParams *Params) {
  Node newNode;
  newNode.setAllParams(Params);
  newNode.startWork();
  newNode.end_node();
}

int main(int argc, char *argv[]) {

  /* Fetching the directory path where the executable is present */
  char buffer[MAX_PATH];
  realpath(argv[0], buffer);
  binary_fullpath = dirname(buffer);

  std::vector<std::thread> nodes;
  std::ios_base::sync_with_stdio(false);
  add_options();
  int c;
  while (true) {
    struct option long_options[Option::MAX];
    int option_index = 0;
    int i = 0;
    for (auto op : *options) {
      if (op == nullptr)
        continue;
      long_options[i++] = {op->getName(), op->getArgs(), 0, op->getOption()};
    };
    long_options[i] = {0, 0, 0, 0};
    c = getopt_long_only(argc, argv, "c:d:a:i:l:s:p:u:P:t:q:vAEFNLDTNOSk",
                         long_options, &option_index);
    if (c == -1) {
      break;
      exit(EXIT_FAILURE);
    }

    switch (c) {
    case 'I':
      show_config_help();
      exit(EXIT_FAILURE);
    case 'C':
      show_cli_help();
      exit(EXIT_FAILURE);
      break;
    case Option::MYSQLD_SERVER_OPTION:
      std::cout << optarg << std::endl;
      add_server_options(optarg);
      break;
    case Option::SERVER_OPTION_FILE:
      add_server_options_file(optarg);
      break;
    case Option::INVALID_OPTION:
      std::cout << "Invalid option , exiting" << std::endl;
      exit(EXIT_FAILURE);
      break;
    default:
      if (c >= Option::MAX) {
        break;
      }
      if (options->at(c) == nullptr) {
        throw std::runtime_error("INVALID OPTION at line " +
                                 std::to_string(__LINE__) + " file " +
                                 __FILE__);
        break;
      }
      auto op = options->at(c);
      /* set command line */
      op->set_cl();
      if (op->getArgs() == required_argument) {
        switch (op->getType()) {
        case Option::INT:
          op->setInt(optarg);
          break;
        case Option::STRING:
          op->setString(optarg);
          break;
        case Option::BOOL:
          std::string s(optarg);
          op->setBool(s);
          break;
        }
      } else if (op->getArgs() == no_argument) {
        op->setBool(true);
      } else if (op->getArgs() == optional_argument) {
        op->setBool(true);
        if (optarg)
          op->setString(optarg);
        break;
      }
    }
  } // while

  /* check if user has asked for help */
  if (options->at(Option::HELP)->getBool() == true) {
    if (options->at(Option::VERBOSE)->getBool() == true)
      show_help("verbose");
    else if (options->at(Option::HELP)->getString().empty())
      show_help();
    else
      show_help(options->at(Option::HELP)->getString());
    delete_options();
    exit(0);
  }

  if (options->at(Option::PQUERY)->getBool()) {
    std::cout << "runnng as pquery" << std::endl;
  }

  auto confFile = options->at(Option::CONFIGFILE)->getString();
  if (confFile.empty()) {
    /*single node and command line */
    workerParams *wParams = new workerParams;
    create_worker(wParams);
  } else {
    INIReader reader(confFile);
    if (reader.ParseError() < 0) {
      std::cout << "Can't load " << confFile << std::endl;
      exit(1);
    }

    auto sections = reader.GetSections();
    for (auto it = sections.begin(); it != sections.end(); it++) {
      std::string secName = *it;
      std::cerr << ": Processing config file for " << secName << std::endl;
      if (reader.GetBoolean(secName, "run", false)) {
        workerParams *wParams = new workerParams;
        read_section_settings(wParams, secName, confFile);
        nodes.push_back(std::thread(create_worker, wParams));
      }
    }

    /* join all nodes */
    for (auto node = nodes.begin(); node != nodes.end(); node++)
      node->join();
  }

  save_metadata_to_file();
  clean_up_at_end();
  mysql_library_end();
  delete_options();
  std::cout << "COMPLTED" << std::endl;

  return EXIT_SUCCESS;
}
