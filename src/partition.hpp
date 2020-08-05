#ifndef __PARTITION_HPP__
#define __PARTITION_HPP__

#include "random_test.hpp"

struct Partition_table : public Table {
public:
std::string p_name;
virtual std::string definition();
Partition_table(std::string n) : Table(n){}
virtual ~Partition_table() {}
};

struct Partition_By_Range : public Partition_table {
public:
virtual std::string definition();
Partition_By_Range(std::string n);
virtual ~Partition_By_Range() {}
};

struct Partition_By_List : public Partition_table {
public:
virtual std::string definition();
Partition_By_List(std::string n);
virtual ~Partition_By_List() {}
};

struct Partition_By_Hash : public Partition_table {
public:
virtual std::string definition();
Partition_By_Hash(std::string n);
virtual ~Partition_By_Hash() {}
};

#endif
