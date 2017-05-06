// Test.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <stdio.h>
#include "leveldb/db.h"
#include <leveldb/options.h>

int main()
{
	leveldb::DB* db;
	leveldb::Options options;
	options.create_if_missing = true;

	leveldb::Status status = leveldb::DB::Open(options, "./testdb", &db);

	if (false == status.ok())
	{
		std::cerr << "Unable to open/create test database './testdb'" << std::endl;
		std::cerr << status.ToString() << std::endl;
		return -1;
	}

	// Add 256 values to the database
	/*leveldb::WriteOptions writeOptions;
	for (unsigned int i = 0; i < 256; ++i)
	{
		ostringstream keyStream;
		keyStream << "Key" << i;

		ostringstream valueStream;
		valueStream << "Test data value: " << i;

		db->Put(writeOptions, keyStream.str(), valueStream.str());
	}*/

	// Iterate over each item in the database and print them
	/*leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());

	for (it->SeekToFirst(); it->Valid(); it->Next())
	{
		cout << it->key().ToString() << " : " << it->value().ToString() << endl;
	}

	if (false == it->status().ok())
	{
		cerr << "An error was found during the scan" << endl;
		cerr << it->status().ToString() << endl;
	}

	delete it;*/

	// Close the database
	delete db;
	int a = 8;
	std::cout << a << std::endl;
    return 0;
}

