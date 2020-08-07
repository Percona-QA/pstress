#include "partition.hpp"
#include "random_test.hpp"

std::string Partition_By_Range::definition() {
  std::string def = Table::definition();
  int no_partition = rand_int(1,4);
  /* ToDo: Add SQL syntax for Partition By Range */
  def += "PARTITION BY RANGE (" + columns_->name + ") ( ";
  for(int i=0; i<no_partition; i++) {
    def += "PARTITION p" + i + " VALUES LESS THAN (" + column_value + "), "
  }
  def += ");";
  return def;
}

std::string Partition_By_List::definition() {
  std::string def = Table::definition();
  /* ToDo: Add SQL syntax for Partition By List */
  std::string def = Table::definition();
  int no_partition = rand_int(1,4);
  def += "PARTITION BY LIST (" + columns_->name + ") (";
  for(int i=0; i<no_partition; i++) {
    def += "PARTITION " + columns_->name + "VALUES IN (" + values + ")";
  }
  def += " ";
  return def;
}

std::string Partition_By_Hash::definition() {
  std::string def = Table::definition();
  /* ToDo: Add SQL synatx for Partition By Hash */
  def += " ";
  return def;
}

