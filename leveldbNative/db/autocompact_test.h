#ifndef AUTOCOMPACT_TEST_H_
#define AUTOCOMPACT_TEST_H_

#include "leveldb/db.h"
#include "db/db_impl.h"
#include "leveldb/cache.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace leveldb {
	class AutoCompactTest {
	public:
		std::string dbname_;
		Cache* tiny_cache_;
		Options options_;
		DB* db_;

		AutoCompactTest();

		~AutoCompactTest();

		std::string Key(int i);

		uint64_t Size(const Slice& start, const Slice& limit);

		void DoReads(int n);

		static const int kValueSize = 200 * 1024;
		static const int kTotalSize = 100 * 1024 * 1024;
		static const int kCount = kTotalSize / kValueSize;
	};
}
#endif  // STORAGE_LEVELDB_UTIL_TESTHARNESS_H_