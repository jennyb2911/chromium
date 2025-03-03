// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/feed/core/feed_journal_database.h"

#include <map>
#include <utility>

#include "base/test/scoped_task_environment.h"
#include "components/feed/core/feed_journal_mutation.h"
#include "components/feed/core/proto/journal_storage.pb.h"
#include "components/leveldb_proto/testing/fake_db.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using leveldb_proto::test::FakeDB;
using testing::Mock;
using testing::NotNull;
using testing::_;

namespace feed {

namespace {

const char kJournalKey1[] = "JournalKey1";
const char kJournalKey2[] = "JournalKey2";
const char kJournalKey3[] = "JournalKey3";
const char kJournalData1[] = "Journal Data1";
const char kJournalData2[] = "Journal Data2";
const char kJournalData3[] = "Journal Data3";
const char kJournalData4[] = "Journal Data4";
const char kJournalData5[] = "Journal Data5";
const char kJournalData6[] = "Journal Data6";

}  // namespace

class FeedJournalDatabaseTest : public testing::Test {
 public:
  FeedJournalDatabaseTest() : journal_db_(nullptr) {}

  void CreateDatabase(bool init_database) {
    // The FakeDBs are owned by |feed_db_|, so clear our pointers before
    // resetting |feed_db_| itself.
    journal_db_ = nullptr;
    // Explicitly destroy any existing database before creating a new one.
    feed_db_.reset();

    auto storage_db =
        std::make_unique<FakeDB<JournalStorageProto>>(&journal_db_storage_);

    journal_db_ = storage_db.get();
    feed_db_ = std::make_unique<FeedJournalDatabase>(base::FilePath(),
                                                     std::move(storage_db));
    if (init_database) {
      journal_db_->InitCallback(true);
      ASSERT_TRUE(db()->IsInitialized());
    }
  }

  void InjectJournalStorageProto(const std::string& key,
                                 const std::vector<std::string>& entries) {
    JournalStorageProto storage_proto;
    storage_proto.set_key(key);
    for (const std::string& entry : entries) {
      storage_proto.add_journal_data(entry);
    }
    journal_db_storage_[key] = storage_proto;
  }

  void RunUntilIdle() { scoped_task_environment_.RunUntilIdle(); }

  FakeDB<JournalStorageProto>* storage_db() { return journal_db_; }

  FeedJournalDatabase* db() { return feed_db_.get(); }

  MOCK_METHOD2(OnJournalEntryReceived, void(bool, std::vector<std::string>));
  MOCK_METHOD1(OnStorageCommitted, void(bool));
  MOCK_METHOD2(OnCheckJournalExistReceived, void(bool, bool));

 private:
  base::test::ScopedTaskEnvironment scoped_task_environment_;

  std::map<std::string, JournalStorageProto> journal_db_storage_;

  // Owned by |feed_db_|.
  FakeDB<JournalStorageProto>* journal_db_;

  std::unique_ptr<FeedJournalDatabase> feed_db_;

  DISALLOW_COPY_AND_ASSIGN(FeedJournalDatabaseTest);
};

TEST_F(FeedJournalDatabaseTest, Init) {
  ASSERT_FALSE(db());

  CreateDatabase(/*init_database=*/false);

  EXPECT_FALSE(db()->IsInitialized());
  storage_db()->InitCallback(true);
  EXPECT_TRUE(db()->IsInitialized());
}

TEST_F(FeedJournalDatabaseTest, LoadJournalEntry) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Store |kJournalKey1| and |kJournalKey2|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4});

  // Try to Load |kJournalKey1|.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 3U);
        EXPECT_EQ(results[0], kJournalData1);
        EXPECT_EQ(results[1], kJournalData2);
        EXPECT_EQ(results[2], kJournalData3);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
}

TEST_F(FeedJournalDatabaseTest, LoadNonExistingJournalEntry) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Try to Load |kJournalKey1|.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 0U);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
}

TEST_F(FeedJournalDatabaseTest, AppendJournal) {
  CreateDatabase(/*init_database=*/true);

  // Save |kJournalKey1|.
  auto journal_mutation = std::make_unique<JournalMutation>(kJournalKey1);
  journal_mutation->AddAppendOperation(kJournalData1);
  journal_mutation->AddAppendOperation(kJournalData2);
  EXPECT_CALL(*this, OnStorageCommitted(true));
  db()->CommitJournalMutation(
      std::move(journal_mutation),
      base::BindOnce(&FeedJournalDatabaseTest::OnStorageCommitted,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
  storage_db()->UpdateCallback(true);

  // Make sure they're there.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 2U);
        EXPECT_EQ(results[0], kJournalData1);
        EXPECT_EQ(results[1], kJournalData2);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);

  Mock::VerifyAndClearExpectations(this);

  // Append more for |kJournalKey1|.
  journal_mutation = std::make_unique<JournalMutation>(kJournalKey1);
  journal_mutation->AddAppendOperation(kJournalData3);
  journal_mutation->AddAppendOperation(kJournalData4);
  journal_mutation->AddAppendOperation(kJournalData5);
  EXPECT_CALL(*this, OnStorageCommitted(true));
  db()->CommitJournalMutation(
      std::move(journal_mutation),
      base::BindOnce(&FeedJournalDatabaseTest::OnStorageCommitted,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
  storage_db()->UpdateCallback(true);

  // Check new instances are there.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 5U);
        EXPECT_EQ(results[0], kJournalData1);
        EXPECT_EQ(results[1], kJournalData2);
        EXPECT_EQ(results[2], kJournalData3);
        EXPECT_EQ(results[3], kJournalData4);
        EXPECT_EQ(results[4], kJournalData5);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
}

TEST_F(FeedJournalDatabaseTest, CopyJournal) {
  CreateDatabase(/*init_database=*/true);

  // Save |kJournalKey1|.
  InjectJournalStorageProto(kJournalKey1, {kJournalData1, kJournalData2});

  // Copy |kJournalKey1| to |kJournalKey2|.
  auto journal_mutation = std::make_unique<JournalMutation>(kJournalKey1);
  journal_mutation->AddCopyOperation(kJournalKey2);
  journal_mutation->AddAppendOperation(kJournalData3);
  journal_mutation->AddAppendOperation(kJournalData4);
  journal_mutation->AddCopyOperation(kJournalKey3);
  EXPECT_CALL(*this, OnStorageCommitted(true));
  db()->CommitJournalMutation(
      std::move(journal_mutation),
      base::BindOnce(&FeedJournalDatabaseTest::OnStorageCommitted,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
  storage_db()->UpdateCallback(true);

  // Check new journal is there.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 2U);
        EXPECT_EQ(results[0], kJournalData1);
        EXPECT_EQ(results[1], kJournalData2);
      });
  db()->LoadJournal(
      kJournalKey2,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);

  Mock::VerifyAndClearExpectations(this);

  // Check new journal is there.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 4U);
        EXPECT_EQ(results[0], kJournalData1);
        EXPECT_EQ(results[1], kJournalData2);
        EXPECT_EQ(results[2], kJournalData3);
        EXPECT_EQ(results[3], kJournalData4);
      });
  db()->LoadJournal(
      kJournalKey3,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);

  Mock::VerifyAndClearExpectations(this);

  // Check first journal is still there.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 4U);
        EXPECT_EQ(results[0], kJournalData1);
        EXPECT_EQ(results[1], kJournalData2);
        EXPECT_EQ(results[2], kJournalData3);
        EXPECT_EQ(results[3], kJournalData4);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
}

TEST_F(FeedJournalDatabaseTest, DeleteJournal) {
  CreateDatabase(/*init_database=*/true);

  // Store |kJournalKey1|, |kJournalKey2|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4, kJournalData5});

  // Delete |kJournalKey2|.
  auto journal_mutation = std::make_unique<JournalMutation>(kJournalKey2);
  journal_mutation->AddDeleteOperation();
  EXPECT_CALL(*this, OnStorageCommitted(true));
  db()->CommitJournalMutation(
      std::move(journal_mutation),
      base::BindOnce(&FeedJournalDatabaseTest::OnStorageCommitted,
                     base::Unretained(this)));
  RunUntilIdle();
  storage_db()->UpdateCallback(true);

  // Make sure |kJournalKey2| got deleted.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 0U);
      });
  db()->LoadJournal(
      kJournalKey2,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);

  Mock::VerifyAndClearExpectations(this);

  // Make sure |kJournalKey1| is still there.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 3U);
        EXPECT_EQ(results[0], kJournalData1);
        EXPECT_EQ(results[1], kJournalData2);
        EXPECT_EQ(results[2], kJournalData3);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
}

TEST_F(FeedJournalDatabaseTest, ChecExistingJournal) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Store |kJournalKey1| and |kJournalKey2|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4});

  // Check |kJournalKey1|.
  EXPECT_CALL(*this, OnCheckJournalExistReceived(true, true));

  db()->DoesJournalExist(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnCheckJournalExistReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
}

TEST_F(FeedJournalDatabaseTest, CheckNonExistingJournal) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Check |kJournalKey1|.
  EXPECT_CALL(*this, OnCheckJournalExistReceived(true, false));
  db()->DoesJournalExist(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnCheckJournalExistReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(true);
}

TEST_F(FeedJournalDatabaseTest, LoadAllJournalKeys) {
  CreateDatabase(/*init_database=*/true);

  // Store |kJournalKey1|, |kJournalKey2| and |kJournalKey3|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4, kJournalData5});
  InjectJournalStorageProto(kJournalKey3, {kJournalData6});

  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 3U);
        EXPECT_EQ(results[0], kJournalKey1);
        EXPECT_EQ(results[1], kJournalKey2);
        EXPECT_EQ(results[2], kJournalKey3);
      });
  db()->LoadAllJournalKeys(
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->LoadKeysCallback(true);
}

TEST_F(FeedJournalDatabaseTest, DeleteAllJournals) {
  CreateDatabase(/*init_database=*/true);

  // Store |kJournalKey1|, |kJournalKey2|, |kJournalKey3|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4, kJournalData5});
  InjectJournalStorageProto(kJournalKey3, {kJournalData6});

  // Delete all journals, meaning |kJournalKey1|, |kJournalKey2| and
  // |kJournalKey3| are expected to be deleted.
  EXPECT_CALL(*this, OnStorageCommitted(true));
  db()->DeleteAllJournals(base::BindOnce(
      &FeedJournalDatabaseTest::OnStorageCommitted, base::Unretained(this)));
  storage_db()->UpdateCallback(true);

  // Make sure all journals got deleted.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_TRUE(success);
        ASSERT_EQ(results.size(), 0U);
      });
  db()->LoadAllJournalKeys(
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->LoadKeysCallback(true);
}

TEST_F(FeedJournalDatabaseTest, LoadJournalEntryFail) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Store |kJournalKey1| and |kJournalKey2|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4});

  // Try to Load |kJournalKey1|.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_FALSE(success);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(false);
}

TEST_F(FeedJournalDatabaseTest, LoadNonExistingJournalEntryFail) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Try to Load |kJournalKey1|.
  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_FALSE(success);
      });
  db()->LoadJournal(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(false);
}

TEST_F(FeedJournalDatabaseTest, LoadAllJournalKeysFail) {
  CreateDatabase(/*init_database=*/true);

  // Store |kJournalKey1|, |kJournalKey2| and |kJournalKey3|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4, kJournalData5});
  InjectJournalStorageProto(kJournalKey3, {kJournalData6});

  EXPECT_CALL(*this, OnJournalEntryReceived(_, _))
      .WillOnce([](bool success, std::vector<std::string> results) {
        EXPECT_FALSE(success);
      });
  db()->LoadAllJournalKeys(
      base::BindOnce(&FeedJournalDatabaseTest::OnJournalEntryReceived,
                     base::Unretained(this)));
  storage_db()->LoadKeysCallback(false);
}

TEST_F(FeedJournalDatabaseTest, ChecExistingJournalFail) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Store |kJournalKey1| and |kJournalKey2|.
  InjectJournalStorageProto(kJournalKey1,
                            {kJournalData1, kJournalData2, kJournalData3});
  InjectJournalStorageProto(kJournalKey2, {kJournalData4});

  // Check |kJournalKey1|.
  EXPECT_CALL(*this, OnCheckJournalExistReceived(false, true));

  db()->DoesJournalExist(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnCheckJournalExistReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(false);
}

TEST_F(FeedJournalDatabaseTest, CheckNonExistingJournalFail) {
  CreateDatabase(/*init_database=*/true);
  EXPECT_TRUE(db()->IsInitialized());

  // Check |kJournalKey1|.
  EXPECT_CALL(*this, OnCheckJournalExistReceived(false, false));
  db()->DoesJournalExist(
      kJournalKey1,
      base::BindOnce(&FeedJournalDatabaseTest::OnCheckJournalExistReceived,
                     base::Unretained(this)));
  storage_db()->GetCallback(false);
}

}  // namespace feed
