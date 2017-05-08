#ifndef DBTEST_TEST_H_
#define DBTEST_TEST_H_

#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/cache.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/hash.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace leveldb {
	
	class DBTestRunner {
	public:
		DBTestRunner();
		~DBTestRunner();

		void RunAllTests();
	};
}
#endif  // DBTEST_TEST_H_