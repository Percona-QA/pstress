#include "partition.hpp"
#include "random_test.hpp"

std::string Partition_By_Range::definition() {
  std::string def = Table::definition();
  /* ToDo: Add SQL syntax for Partition By Range */
  def += " ";
  return def;
}

std::string Partition_By_List::definition() {
  std::string def = Table::definition();
  /* ToDo: Add SQL syntax for Partition By List */
  def += " ";
  return def;
}

std::string Partition_By_Hash::definition() {
  std::string def = Table::definition();
  /* ToDo: Add SQL synatx for Partition By Hash */
  def += " ";
  return def;
}

