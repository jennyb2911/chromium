// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <utility>

#include "base/macros.h"
#include "base/strings/string16.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_task_environment.h"
#include "build/build_config.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/test/test_bookmark_client.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/omnibox_field_trial.h"
#include "components/omnibox/browser/omnibox_view.h"
#include "components/omnibox/browser/test_omnibox_client.h"
#include "components/omnibox/browser/test_omnibox_edit_controller.h"
#include "components/omnibox/browser/test_omnibox_edit_model.h"
#include "components/omnibox/browser/test_omnibox_view.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/test/material_design_controller_test_api.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/favicon_size.h"
#include "ui/gfx/paint_vector_icon.h"
#if !defined(OS_ANDROID) && !defined(OS_IOS)
#include "components/omnibox/browser/vector_icons.h"  // nogncheck
#include "components/vector_icons/vector_icons.h"     // nogncheck
#endif

using base::ASCIIToUTF16;

namespace {

class OmniboxViewTest : public testing::Test {
 public:
  OmniboxViewTest() {
    controller_ = std::make_unique<TestOmniboxEditController>();
    view_ = std::make_unique<TestOmniboxView>(controller_.get());
    view_->SetModel(
        std::make_unique<TestOmniboxEditModel>(view_.get(), controller_.get()));

    bookmark_model_ = bookmarks::TestBookmarkClient::CreateModel();
    client()->SetBookmarkModel(bookmark_model_.get());

    material_design_ =
        std::make_unique<ui::test::MaterialDesignControllerTestAPI>(
            ui::MaterialDesignController::MATERIAL_REFRESH);
  }

  TestOmniboxView* view() { return view_.get(); }

  TestOmniboxEditModel* model() {
    return static_cast<TestOmniboxEditModel*>(view_->model());
  }

  TestOmniboxClient* client() {
    return static_cast<TestOmniboxClient*>(model()->client());
  }

  bookmarks::BookmarkModel* bookmark_model() { return bookmark_model_.get(); }

 private:
  base::test::ScopedTaskEnvironment task_environment_;
  std::unique_ptr<ui::test::MaterialDesignControllerTestAPI> material_design_;
  std::unique_ptr<TestOmniboxEditController> controller_;
  std::unique_ptr<TestOmniboxView> view_;
  std::unique_ptr<bookmarks::BookmarkModel> bookmark_model_;
};

TEST_F(OmniboxViewTest, TestStripSchemasUnsafeForPaste) {
  const char* urls[] = {
      " \x01 ",                                       // Safe query.
      "http://www.google.com?q=javascript:alert(0)",  // Safe URL.
      "JavaScript",                                   // Safe query.
      "javaScript:",                                  // Unsafe JS URL.
      " javaScript: ",                                // Unsafe JS URL.
      "javAscript:Javascript:javascript",             // Unsafe JS URL.
      "javAscript:alert(1)",                          // Unsafe JS URL.
      "javAscript:javascript:alert(2)",               // Single strip unsafe.
      "jaVascript:\njavaScript:\x01 alert(3) \x01",   // Single strip unsafe.
      "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x10\x11\x12\x13\x14\x15\x16\x17"
      "\x18\x19 JavaScript:alert(4)",  // Leading control chars unsafe.
      "\x01\x02javascript:\x03\x04JavaScript:alert(5)"  // Embedded control
                                                        // characters unsafe.
  };

  const char* expecteds[] = {
      " \x01 ",                                       // Safe query.
      "http://www.google.com?q=javascript:alert(0)",  // Safe URL.
      "JavaScript",                                   // Safe query.
      "",                                             // Unsafe JS URL.
      "",                                             // Unsafe JS URL.
      "javascript",                                   // Unsafe JS URL.
      "alert(1)",                                     // Unsafe JS URL.
      "alert(2)",                                     // Single strip unsafe.
      "alert(3) \x01",                                // Single strip unsafe.
      "alert(4)",  // Leading control chars unsafe.
      "alert(5)"   // Embedded control characters unsafe.
  };

  for (size_t i = 0; i < arraysize(urls); i++) {
    EXPECT_EQ(ASCIIToUTF16(expecteds[i]),
              OmniboxView::StripJavascriptSchemas(base::UTF8ToUTF16(urls[i])));
  }
}

TEST_F(OmniboxViewTest, SanitizeTextForPaste) {
  // Broken URL has newlines stripped.
  const base::string16 kWrappedURL(ASCIIToUTF16(
      "http://www.chromium.org/developers/testing/chromium-\n"
      "build-infrastructure/tour-of-the-chromium-buildbot"));

  const base::string16 kFixedURL(ASCIIToUTF16(
      "http://www.chromium.org/developers/testing/chromium-"
      "build-infrastructure/tour-of-the-chromium-buildbot"));
  EXPECT_EQ(kFixedURL, OmniboxView::SanitizeTextForPaste(kWrappedURL));

  // Multi-line address is converted to a single-line address.
  const base::string16 kWrappedAddress(ASCIIToUTF16(
      "1600 Amphitheatre Parkway\nMountain View, CA"));

  const base::string16 kFixedAddress(ASCIIToUTF16(
      "1600 Amphitheatre Parkway Mountain View, CA"));
  EXPECT_EQ(kFixedAddress, OmniboxView::SanitizeTextForPaste(kWrappedAddress));

  // Line-breaking the JavaScript scheme with no other whitespace results in a
  // dangerous URL that is sanitized by dropping the scheme.
  const base::string16 kDangerousJavaScriptUrl(
      ASCIIToUTF16("java\x0d\x0ascript:alert(0)"));
  const base::string16 kFixedDangerousJavaScriptUrl(ASCIIToUTF16("alert(0)"));
  EXPECT_EQ(kFixedDangerousJavaScriptUrl,
            OmniboxView::SanitizeTextForPaste(kDangerousJavaScriptUrl));

  // Line-breaking the JavaScript scheme with whitespace elsewhere in the string
  // results in a safe string with a space replacing the line break.
  const base::string16 kSafeJavaScriptUrl(
      ASCIIToUTF16("java\x0d\x0ascript: alert(0)"));
  const base::string16 kFixedSafeJavaScriptUrl(
      ASCIIToUTF16("java script: alert(0)"));
  EXPECT_EQ(kFixedSafeJavaScriptUrl,
            OmniboxView::SanitizeTextForPaste(kSafeJavaScriptUrl));
}

#if !defined(OS_ANDROID) && !defined(OS_IOS)
// Tests GetIcon returns the default search icon when the match is a search
// query.
TEST_F(OmniboxViewTest, GetIcon_Default) {
  gfx::ImageSkia expected_icon = gfx::CreateVectorIcon(
      vector_icons::kSearchIcon, gfx::kFaviconSize, gfx::kPlaceholderColor);

  gfx::ImageSkia icon = view()->GetIcon(
      gfx::kFaviconSize, gfx::kPlaceholderColor, base::DoNothing());

  EXPECT_EQ(icon.bitmap(), expected_icon.bitmap());
}

// Tests GetIcon returns the bookmark icon when the match is bookmarked.
TEST_F(OmniboxViewTest, GetIcon_BookmarkIcon) {
  const GURL kUrl("https://bookmarks.com");

  AutocompleteMatch match;
  match.destination_url = kUrl;
  model()->SetCurrentMatch(match);

  bookmark_model()->AddURL(bookmark_model()->bookmark_bar_node(), 0,
                           base::ASCIIToUTF16("a bookmark"), kUrl);

  gfx::ImageSkia expected_icon =
      gfx::CreateVectorIcon(omnibox::kTouchableBookmarkIcon, gfx::kFaviconSize,
                            gfx::kPlaceholderColor);

  gfx::ImageSkia icon = view()->GetIcon(
      gfx::kFaviconSize, gfx::kPlaceholderColor, base::DoNothing());

  EXPECT_EQ(icon.bitmap(), expected_icon.bitmap());
}

// Tests GetIcon returns the website's favicon when the match is a website.
TEST_F(OmniboxViewTest, GetIcon_Favicon) {
  const GURL kUrl("https://woahDude.com");

  base::test::ScopedFeatureList scoped_feature_list_;
  scoped_feature_list_.InitAndEnableFeature(
      omnibox::kUIExperimentShowSuggestionFavicons);

  AutocompleteMatch match;
  match.type = AutocompleteMatchType::URL_WHAT_YOU_TYPED;
  match.destination_url = kUrl;
  model()->SetCurrentMatch(match);

  view()->GetIcon(gfx::kFaviconSize, gfx::kPlaceholderColor, base::DoNothing());

  EXPECT_EQ(client()->GetPageUrlForLastFaviconRequest(), kUrl);
}
#endif  // !defined(OS_IOS)

}  // namespace
