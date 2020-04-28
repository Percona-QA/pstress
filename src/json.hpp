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

  // constructor to read from file
  Json(rapidjson::Document &d);

  /* randomly return hash or array */
  static type rand_type() { return rand_int(1) == 0 ? ARRAY : HASH; }

  /* maximum size of the element in json  */
  static int max_size;

  static int max_sub_set;
  ~Json();

  // Serialzie metadata
  template <typename Writer> void Serialize(Writer &writer) const {
    writer.String("sub_type");
    std::string type = sub_type_to_string(type_);
    writer.String(type.c_str(),
                  static_cast<rapidjson::SizeType>(type.length()));
    writer.String("size");
    writer.Int(size_);
    writer.String("sub_json");
      writer.StartArray();
      for (auto i : sub_json) {
        writer.StartObject();
        writer.String("position");
        writer.Int(i.first);
        i.second->Serialize(writer);
        writer.EndObject();
      }
      writer.EndArray();
  }

  static std::string sub_type_to_string(type s) {
    switch (s) {
    case HASH:
      return "HASH";
    case ARRAY:
      return "ARRAY";
    }
  }

  static type string_to_sub_type(std::string s) {
    if (s.compare("HASH") == 0)
      return HASH;
    else if (s.compare("ARRAY") == 0)
      return ARRAY;
    else
      throw std::runtime_error("unhandled " + s + " at line " +
                               std::to_string(__LINE__));
  }
  int size_;
  std::map<int, std::shared_ptr<Json>> sub_json;
  /* return random JSON using column defination
  std::string Json_Column::rand_value() {
    StringBuffer sb;
    PrettyWriter<StringBuffer> writer(sb);
    if (sub_type == HASH) {
      writer.StartObject();
      for (int i = 0; i < size; i++) {
        writer.String(rand_string(5).c_str());
        writer.String(rand_string(10).c_str());
      }
      writer.EndObject();
    } else if (sub_type == ARRAY) {
      writer.StartArray();
      for (int i = 0; i < size; i++)
        writer.String(rand_string(10).c_str());
      writer.EndArray();
    }
    return sb.GetString();
  }

  void Json_Column::rand_sub_value(std::string &where, std::string &value) {
    if (sub_type == HASH) {
      where = "some_hash";
      value = "some_value";
    } else {
      where = "some_array";
      value = "some_value";
    }
  }
  std::string rand_value();

  void rand_sub_value(std::string &where, std::string &value);
  */
};

#endif
