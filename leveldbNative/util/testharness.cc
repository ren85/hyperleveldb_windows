// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/testharness.h"

#include <string>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "db/autocompact_test.h"
#include "db/corruption_test.h"
#include "db/db_test.h"
#include <iostream>
namespace leveldb {
	namespace test {

		namespace {
			struct Test {
				const char* base;
				const char* name;
				void(*func)();
			};
			std::vector<Test>* tests;
		}

		bool RegisterTest(const char* base, const char* name, void(*func)()) {
			if (tests == NULL) {
				tests = new std::vector<Test>;
			}
			Test t;
			t.base = base;
			t.name = name;
			t.func = func;
			tests->push_back(t);
			return true;
		}

		int RunAllTests() {
			//const char* matcher = getenv("LEVELDB_TESTS");

			//int num = 0;
			//if (tests != NULL) {
			//	for (size_t i = 0; i < tests->size(); i++) {
			//		const Test& t = (*tests)[i];
			//		if (matcher != NULL) {
			//			std::string name = t.base;
			//			name.push_back('.');
			//			name.append(t.name);
			//			if (strstr(name.c_str(), matcher) == NULL) {
			//				continue;
			//			}
			//		}
			//		fprintf(stderr, "==== Test %s.%s\n", t.base, t.name);
			//		(*t.func)();
			//		++num;
			//	}
			//}

			std::cout << "==========AutoCompactTest==========" << std::endl;
			AutoCompactTest *ac = new AutoCompactTest();
			ac->DoReads(ac->kCount);
			delete ac;

			std::cout << "==========CorruptionTest==========" << std::endl;
			CorruptionTest *ct = new CorruptionTest();
			ct->RecoveryTest();
			delete ct;
			ct = new CorruptionTest();
			ct->RecoverWriteError();
			delete ct;
			ct = new CorruptionTest();
			ct->NewFileErrorDuringWrite();
			delete ct;
			ct = new CorruptionTest();
			ct->TableFile();
			delete ct;
			ct = new CorruptionTest();
			ct->TableFileRepair();
			delete ct;
			ct = new CorruptionTest();
			ct->TableFileIndexData();
			delete ct;
			ct = new CorruptionTest();
			ct->MissingDescriptor();
			delete ct;
			ct = new CorruptionTest();
			ct->SequenceNumberRecovery();
			delete ct;
			ct = new CorruptionTest();
			ct->CorruptedDescriptor();
			delete ct;
			ct = new CorruptionTest();
			ct->CompactionInputError();
			delete ct;
			ct = new CorruptionTest();
			ct->CompactionInputErrorParanoid();
			delete ct;
			ct = new CorruptionTest();
			ct->UnrelatedKeys();
			delete ct;

			std::cout << "==========DbTest==========" << std::endl;
			DBTestRunner *dt = new DBTestRunner();
			dt->RunAllTests();
			delete dt;
			
			
			

			
			


			//fprintf(stderr, "==== PASSED %d tests\n", num);
			return 0;
		}

		std::string TmpDir() {
			std::string dir;
			Status s = Env::Default()->GetTestDirectory(&dir);
			ASSERT_TRUE(s.ok()) << s.ToString();
			return dir;
		}

		int RandomSeed() {
			const char* env = getenv("TEST_RANDOM_SEED");
			int result = (env != NULL ? atoi(env) : 301);
			if (result <= 0) {
				result = 301;
			}
			return result;
		}

	}  // namespace test
}  // namespace leveldb
