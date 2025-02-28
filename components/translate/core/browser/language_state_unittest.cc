// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/translate/core/browser/language_state.h"

#include <memory>

#include "base/macros.h"
#include "components/translate/core/browser/language_state.h"
#include "components/translate/core/browser/mock_translate_driver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

using translate::testing::MockTranslateDriver;

namespace translate {

TEST(LanguageStateTest, IsPageTranslated) {
  std::unique_ptr<MockTranslateDriver> driver(new MockTranslateDriver);
  LanguageState language_state(driver.get());
  EXPECT_FALSE(language_state.IsPageTranslated());

  // Navigate to a French page.
  LanguageDetectionDetails details_fr;
  details_fr.adopted_language = "fr";
  language_state.LanguageDetermined(details_fr, true);
  EXPECT_EQ("fr", language_state.original_language());
  EXPECT_EQ("fr", language_state.current_language());
  EXPECT_FALSE(language_state.IsPageTranslated());

  // Translate the page into English.
  language_state.SetCurrentLanguage("en");
  EXPECT_EQ("fr", language_state.original_language());
  EXPECT_EQ("en", language_state.current_language());
  EXPECT_TRUE(language_state.IsPageTranslated());

  // Move on another page in Japanese.
  LanguageDetectionDetails details_ja;
  details_ja.adopted_language = "ja";
  language_state.LanguageDetermined(details_ja, true);
  EXPECT_EQ("ja", language_state.original_language());
  EXPECT_EQ("ja", language_state.current_language());
  EXPECT_FALSE(language_state.IsPageTranslated());
}

TEST(LanguageStateTest, Driver) {
  std::unique_ptr<MockTranslateDriver> driver(new MockTranslateDriver);
  LanguageState language_state(driver.get());

  // Enable/Disable translate.
  EXPECT_FALSE(language_state.translate_enabled());
  EXPECT_FALSE(driver->on_translate_enabled_changed_called());
  language_state.SetTranslateEnabled(true);
  EXPECT_TRUE(language_state.translate_enabled());
  EXPECT_TRUE(driver->on_translate_enabled_changed_called());

  driver->Reset();
  language_state.SetTranslateEnabled(false);
  EXPECT_FALSE(language_state.translate_enabled());
  EXPECT_TRUE(driver->on_translate_enabled_changed_called());

  // Navigate to a French page.
  driver->Reset();
  LanguageDetectionDetails details;
  details.adopted_language = "fr";
  language_state.LanguageDetermined(details, true);
  EXPECT_FALSE(language_state.translate_enabled());
  EXPECT_FALSE(driver->on_is_page_translated_changed_called());
  EXPECT_FALSE(driver->on_translate_enabled_changed_called());

  // Translate.
  language_state.SetCurrentLanguage("en");
  EXPECT_TRUE(language_state.IsPageTranslated());
  EXPECT_TRUE(driver->on_is_page_translated_changed_called());

  // Translate feature must be enabled after an actual translation.
  EXPECT_TRUE(language_state.translate_enabled());
  EXPECT_TRUE(driver->on_translate_enabled_changed_called());
}

}  // namespace translate
