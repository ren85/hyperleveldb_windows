VS 2015 project

Requires boost (assuming in C:\boost_1_60_0). Actually not so many references to boost left, so maybe not too hard to get rid of it. 

leveldb.native must be static (.lib) for c++ test to run and dynamic (.dll) for C# tests to run.

All tests pass except multithreading test (commented in db_test.cc), which is stuck in deadlock.

No snappy.

Tested in different scenarios - working, but sometimes throws exceptions.

Good starting point for someone with better knowledge of C++ and hyperleveldb.

Stitched together from

https://github.com/Reactive-Extensions/LevelDB (not very good port, but has clear project structure)

https://github.com/ren85/leveldb-windows (much better port)