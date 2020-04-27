/*
 =========================================================
 #       Created by Rahul Malik, Percona LLC             #
 #       Library to create random json data
 =========================================================
*/
#ifndef __JSON_HPP__
#define __JSON_HPP__
#include "common.hpp"
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <document.h>
#include <filereadstream.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <mysql.h>
#include <prettywriter.h>
#include <random>
#include <sstream>
#include <string.h>
#include <string>
#include <vector>
#include <writer.h>

struct Json;
struct Json {
  enum type { HASH, ARRAY } type_;

  // constructor used to create random object
  Json(int depth);
  int size_;
  std::map<int, std::shared_ptr<Json>> sub_json;

  /* randomly return hash or array */
  static type rand_type() { return rand_int(1) == 0 ? ARRAY : HASH; }

  /* maximum size of the element in json  */
  static int max_size;

  static int max_sub_set;
  ~Json();

  template <typename Writer> void Serialize(Writer &writer) const {
    writer.String("sub_type");
    std::string type = sub_type_to_string(type_);
    writer.String(type.c_str(),
                  static_cast<rapidjson::SizeType>(type.length()));
  }

  static std::string sub_type_to_string(type s) {
    switch (s) {
    case HASH:
      return "HASH";
    case ARRAY:
      return "ARRAY";
    }
  }
};

#endif
