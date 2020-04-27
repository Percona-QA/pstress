/*
 =========================================================
 #       Created by Rahul Malik, Percona LLC             #
 #       Library to create random json data
 =========================================================
*/
#include "json.hpp"
#include <set>

int Json::max_size = 300;
int Json::max_sub_set = 3;
Json::Json(int depth) {
  std::cout << "calling distructor " << depth << std::endl;
  size_ = rand_int(max_size);
  type_ = rand_type();
  if (depth > 0) {
    std::set<int> position;
    for (int i = 0; i < max_sub_set; i++)
      position.insert(rand_int(max_size, 1));

    for (auto t : position)
      sub_json.insert({t, std::make_shared<Json>(depth - 1)});
  }
}
Json::~Json() { std::cout << "distructor " << type_ << std::endl; }


