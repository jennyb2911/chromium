// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/explore_sites/import_catalog_task.h"

#include <memory>

#include "base/logging.h"
#include "base/test/bind_test_util.h"
#include "base/test/mock_callback.h"
#include "chrome/browser/android/explore_sites/catalog.pb.h"
#include "chrome/browser/android/explore_sites/explore_sites_schema.h"
#include "components/offline_pages/task/task.h"
#include "components/offline_pages/task/task_test_base.h"
#include "sql/database.h"
#include "sql/meta_table.h"
#include "sql/statement.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using offline_pages::TaskTestBase;

namespace explore_sites {

const char kVersionToken[] = "12345";
const char kGoogleUrl[] = "https://www.google.com";
const char kGoogleTitle[] = "Google Search";
const char kGmailUrl[] = "https://mail.google.com/mail";
const char kGmailTitle[] = "GMail";
const char kGoogleCategoryTitle[] = "Goooogle";
const char kIcon[] =
    "data:image/gif;"
    "base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==";

class ExploreSitesImportCatalogTaskTest : public TaskTestBase {
 public:
  ExploreSitesImportCatalogTaskTest() = default;
  ~ExploreSitesImportCatalogTaskTest() override = default;

  void SetUp() override {
    store_ = std::make_unique<ExploreSitesStore>(task_runner());
    success_ = false;
    callback_called_ = false;
  }

  ExploreSitesStore* store() { return store_.get(); }

  void ExecuteSync(base::RepeatingCallback<bool(sql::Database*)> query) {
    store()->Execute(base::OnceCallback<bool(sql::Database*)>(query),
                     base::BindOnce([](bool result) { ASSERT_TRUE(result); }),
                     false);
    RunUntilIdle();
  }

  void OnImportTaskDone(bool success) {
    success_ = success;
    callback_called_ = true;
  }

  bool success() { return success_; }

  bool callback_called() { return callback_called_; }

 private:
  std::unique_ptr<ExploreSitesStore> store_;
  bool success_;
  bool callback_called_;

  DISALLOW_COPY_AND_ASSIGN(ExploreSitesImportCatalogTaskTest);
};

TEST_F(ExploreSitesImportCatalogTaskTest, StoreFailure) {
  store()->SetInitializationStatusForTest(InitializationStatus::FAILURE);
  ImportCatalogTask task(
      store(), kVersionToken, std::make_unique<Catalog>(),
      base::BindOnce(&ExploreSitesImportCatalogTaskTest::OnImportTaskDone,
                     base::Unretained(this)));
  RunTask(&task);

  // A null catalog should be completed but return with an error.
  EXPECT_TRUE(task.complete());
  EXPECT_FALSE(task.result());
}

TEST_F(ExploreSitesImportCatalogTaskTest, EmptyTask) {
  ImportCatalogTask task(
      store(), kVersionToken, std::unique_ptr<Catalog>(),
      base::BindOnce(&ExploreSitesImportCatalogTaskTest::OnImportTaskDone,
                     base::Unretained(this)));
  RunTask(&task);

  // A null catalog should be completed but return with an error.
  EXPECT_TRUE(task.complete());
  EXPECT_FALSE(task.result());
}

// This tests the behavior of the catalog task when there is already a catalog
// with the current version_token in the database. This tests both the case
// where it is the "current" catalog, and where it is the "downloading" catalog.
TEST_F(ExploreSitesImportCatalogTaskTest, CatalogAlreadyInUse) {
  // Successfully import a catalog with "version_token".
  ImportCatalogTask task(
      store(), kVersionToken, std::make_unique<Catalog>(),
      base::BindOnce(&ExploreSitesImportCatalogTaskTest::OnImportTaskDone,
                     base::Unretained(this)));
  RunTask(&task);
  ASSERT_TRUE(task.result());

  // Importing the same catalog again should cause a successful import,
  // since the catalog was not "current".
  ImportCatalogTask task2(
      store(), kVersionToken, std::make_unique<Catalog>(),
      base::BindOnce(&ExploreSitesImportCatalogTaskTest::OnImportTaskDone,
                     base::Unretained(this)));
  RunTask(&task2);
  EXPECT_TRUE(task2.result());

  // Now make the catalog "current".
  ExecuteSync(base::BindLambdaForTesting([&](sql::Database* db) {
    sql::MetaTable meta_table;
    ExploreSitesSchema::InitMetaTable(db, &meta_table);
    meta_table.SetValue("current_catalog", kVersionToken);
    meta_table.DeleteKey("downloading_catalog");
    return true;
  }));

  // Now it should fail to import another copy of the same catalog.
  ImportCatalogTask task3(
      store(), kVersionToken, std::make_unique<Catalog>(),
      base::BindOnce(&ExploreSitesImportCatalogTaskTest::OnImportTaskDone,
                     base::Unretained(this)));
  RunTask(&task3);
  EXPECT_TRUE(task3.complete());
  EXPECT_FALSE(task3.result());
}

TEST_F(ExploreSitesImportCatalogTaskTest, BasicCatalog) {
  auto catalog = std::make_unique<Catalog>();
  auto* categories = catalog->mutable_categories();
  Category* first = categories->Add();
  first->set_type(Category_CategoryType_GOOGLE);
  first->set_localized_title(kGoogleCategoryTitle);
  first->set_icon(kIcon);

  auto* sites = first->mutable_sites();
  Site* google = sites->Add();
  google->set_title(kGoogleTitle);
  google->set_site_url(kGoogleUrl);
  google->set_icon(kIcon);
  Site* gmail = sites->Add();
  gmail->set_title(kGmailTitle);
  gmail->set_site_url(kGmailUrl);
  gmail->set_icon(kIcon);

  ImportCatalogTask task(
      store(), kVersionToken, std::move(catalog),
      base::BindOnce(&ExploreSitesImportCatalogTaskTest::OnImportTaskDone,
                     base::Unretained(this)));
  RunTask(&task);

  EXPECT_TRUE(task.complete());
  EXPECT_TRUE(task.result());
  EXPECT_TRUE(success());
  EXPECT_TRUE(callback_called());

  ExecuteSync(base::BindLambdaForTesting([&](sql::Database* db) {
    sql::Statement cat_count_s(
        db->GetUniqueStatement("SELECT COUNT(*) FROM categories"));
    cat_count_s.Step();
    EXPECT_EQ(1, cat_count_s.ColumnInt(0));

    sql::Statement cat_data_s(
        db->GetUniqueStatement("SELECT version_token, type, label, image, "
                               "category_id FROM categories"));
    cat_data_s.Step();
    EXPECT_EQ(kVersionToken, cat_data_s.ColumnString(0));
    EXPECT_EQ(static_cast<int>(Category_CategoryType_GOOGLE),
              cat_data_s.ColumnInt(1));
    EXPECT_EQ(kGoogleCategoryTitle, cat_data_s.ColumnString(2));
    EXPECT_EQ(kIcon, cat_data_s.ColumnString(3));

    // Extract category id for joining with sites.
    int category_id = cat_data_s.ColumnInt(4);

    sql::Statement site_count_s(
        db->GetUniqueStatement("SELECT COUNT(*) FROM sites"));
    site_count_s.Step();
    EXPECT_EQ(2, site_count_s.ColumnInt(0));

    sql::Statement site_data_s(db->GetUniqueStatement(
        "SELECT url, category_id, title, favicon FROM sites"));
    site_data_s.Step();
    EXPECT_EQ(kGoogleUrl, site_data_s.ColumnString(0));
    EXPECT_EQ(category_id, site_data_s.ColumnInt(1));
    EXPECT_EQ(kGoogleTitle, site_data_s.ColumnString(2));
    EXPECT_EQ(kIcon, site_data_s.ColumnString(3));

    site_data_s.Step();
    EXPECT_EQ(kGmailUrl, site_data_s.ColumnString(0));
    EXPECT_EQ(category_id, site_data_s.ColumnInt(1));
    EXPECT_EQ(kGmailTitle, site_data_s.ColumnString(2));
    EXPECT_EQ(kIcon, site_data_s.ColumnString(3));
    return true;
  }));
}

}  // namespace explore_sites
