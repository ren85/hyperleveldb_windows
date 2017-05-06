// Test.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <stdio.h>
#include "leveldb/db.h"
#include <leveldb/options.h>
#include "leveldb/../../util/testharness.h"

int main()
{
	//leveldb::test::RegisterTest(#base, #name, &TCONCAT(_Test_, name)::_RunIt); 
	//return leveldb::test::RunAllTests();


	//leveldb::DB* db;
	//leveldb::Options options;
	//options.create_if_missing = true;

	//leveldb::Status status = leveldb::DB::Open(options, "testdb", &db);

	//if (false == status.ok())
	//{
	//	std::cerr << "Unable to open/create test database 'testdb'" << std::endl;
	//	std::cerr << status.ToString() << std::endl;
	//	return -1;
	//}

	//status = db->Put(leveldb::WriteOptions(), "a", "b");
	//if (false == status.ok())
	//{
	//	std::cerr << "Unable to open/create test database 'testdb'" << std::endl;
	//	std::cerr << status.ToString() << std::endl;
	//	return -1;
	//}

	//std::string value;
	//status = db->Get(leveldb::ReadOptions(), "a", &value);
	//if (false == status.ok())
	//{
	//	std::cerr << "Unable to open/create test database 'testdb'" << std::endl;
	//	std::cerr << status.ToString() << std::endl;
	//	return -1;
	//}
	//
	//std::cout << value << std::endl;
	//delete db;

	//int a = 8;
	//std::cout << a << std::endl;
    return 0;
}

