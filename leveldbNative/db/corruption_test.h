#ifndef CORRUPTION_TEST_H_
#define CORRUPTION_TEST_H_

#include "leveldb/db.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "leveldb/cache.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "leveldb/write_batch.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/log_format.h"
#include "db/version_set.h"
#include "util/logging.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace leveldb {
	static const int kValueSize = 1000;

	class CorruptionTest {
	public:
		test::ErrorEnv env_;
		std::string dbname_;
		Cache* tiny_cache_;
		Options options_;
		DB* db_;

		CorruptionTest();

		~CorruptionTest();
		Status TryReopen();

		void Reopen();

		void RepairDB();

		void Build(int n);

		void Check(int min_expected, int max_expected);

		void Corrupt(FileType filetype, int offset, int bytes_to_corrupt);

		int Property(const std::string& name);

		// Return the ith key
		Slice Key(int i, std::string* storage);

		// Return the value to associate with the specified key
		Slice Value(int k, std::string* storage);

		void RecoveryTest();
		void RecoverWriteError();
		void NewFileErrorDuringWrite();
		void TableFile();
		void TableFileRepair();
		void TableFileIndexData();
		void MissingDescriptor();
		void SequenceNumberRecovery();
		void CorruptedDescriptor();
		void CompactionInputError();
		void CompactionInputErrorParanoid();
		void UnrelatedKeys();
	};
}
#endif  // STORAGE_LEVELDB_UTIL_TESTHARNESS_H_