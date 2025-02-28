// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/optional.h"
#include "pdf/pdfium/pdfium_engine.h"
#include "pdf/pdfium/pdfium_test_base.h"
#include "pdf/test/test_client.h"
#include "testing/gmock/include/gmock/gmock.h"

using testing::InSequence;
using testing::_;

namespace chrome_pdf {

namespace {

class FindTextTestClient : public TestClient {
 public:
  FindTextTestClient() = default;
  ~FindTextTestClient() override = default;

  // PDFEngine::Client:
  MOCK_METHOD2(NotifyNumberOfFindResultsChanged, void(int, bool));
  MOCK_METHOD1(NotifySelectedFindResultChanged, void((int)));

  std::vector<SearchStringResult> SearchString(const base::char16* string,
                                               const base::char16* term,
                                               bool case_sensitive) override {
    EXPECT_TRUE(case_sensitive);
    base::string16 haystack = base::string16(string);
    base::string16 needle = base::string16(term);

    std::vector<SearchStringResult> results;

    size_t pos = 0;
    while (1) {
      pos = haystack.find(needle, pos);
      if (pos == base::string16::npos)
        break;

      SearchStringResult result;
      result.length = needle.size();
      result.start_index = pos;
      results.push_back(result);
      pos += needle.size();
    }
    return results;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FindTextTestClient);
};

}  // namespace

using FindTextTest = PDFiumTestBase;

TEST_F(FindTextTest, FindText) {
  SetDocumentForTest(FILE_PATH_LITERAL("hello_world2.pdf"));
  pp::URLLoader dummy_loader;
  FindTextTestClient client;
  PDFiumEngine engine(&client, true);
  ASSERT_TRUE(engine.New("https://chromium.org/dummy.pdf", ""));
  ASSERT_TRUE(engine.HandleDocumentLoad(dummy_loader));

  {
    InSequence sequence;

    EXPECT_CALL(client, NotifyNumberOfFindResultsChanged(1, false));
    EXPECT_CALL(client, NotifySelectedFindResultChanged(0));
    for (int i = 1; i < 10; ++i)
      EXPECT_CALL(client, NotifyNumberOfFindResultsChanged(i + 1, false));
    EXPECT_CALL(client, NotifyNumberOfFindResultsChanged(10, true));
  }

  engine.StartFind("o", /*case_sensitive=*/true);
}

TEST_F(FindTextTest, FindHyphenatedText) {
  SetDocumentForTest(FILE_PATH_LITERAL("spanner.pdf"));
  pp::URLLoader dummy_loader;
  FindTextTestClient client;
  PDFiumEngine engine(&client, true);
  ASSERT_TRUE(engine.New("https://chromium.org/dummy.pdf", ""));
  ASSERT_TRUE(engine.HandleDocumentLoad(dummy_loader));

  {
    InSequence sequence;

    EXPECT_CALL(client, NotifyNumberOfFindResultsChanged(1, false));
    EXPECT_CALL(client, NotifySelectedFindResultChanged(0));
    for (int i = 1; i < 6; ++i)
      EXPECT_CALL(client, NotifyNumberOfFindResultsChanged(i + 1, false));
    EXPECT_CALL(client, NotifyNumberOfFindResultsChanged(6, true));
  }

  engine.StartFind("application", /*case_sensitive=*/true);
}

}  // namespace chrome_pdf
