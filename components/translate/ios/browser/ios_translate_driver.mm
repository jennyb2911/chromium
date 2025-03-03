// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/translate/ios/browser/ios_translate_driver.h"

#include "base/bind.h"
#include "base/logging.h"
#include "base/strings/sys_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "components/translate/core/browser/translate_client.h"
#include "components/translate/core/browser/translate_manager.h"
#include "components/translate/core/common/language_detection_details.h"
#include "components/translate/core/common/translate_constants.h"
#include "components/translate/core/common/translate_metrics.h"
#import "components/translate/ios/browser/js_language_detection_manager.h"
#import "components/translate/ios/browser/js_translate_manager.h"
#import "components/translate/ios/browser/language_detection_controller.h"
#import "components/translate/ios/browser/translate_controller.h"
#include "ios/web/public/browser_state.h"
#include "ios/web/public/navigation_item.h"
#include "ios/web/public/navigation_manager.h"
#include "ios/web/public/referrer.h"
#include "ios/web/public/web_state/js/crw_js_injection_receiver.h"
#include "ios/web/public/web_state/navigation_context.h"
#include "ios/web/public/web_state/web_state.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace translate {

namespace {
// Language name passed to the Translate element for it to detect the language.
const char kAutoDetectionLanguage[] = "auto";

}  // namespace

IOSTranslateDriver::IOSTranslateDriver(
    web::WebState* web_state,
    web::NavigationManager* navigation_manager,
    TranslateManager* translate_manager)
    : web_state_(web_state),
      navigation_manager_(navigation_manager),
      translate_manager_(translate_manager->GetWeakPtr()),
      page_seq_no_(0),
      pending_page_seq_no_(0),
      weak_method_factory_(this) {
  DCHECK(navigation_manager_);
  DCHECK(translate_manager_);
  DCHECK(web_state_);

  web_state_->AddObserver(this);

  CRWJSInjectionReceiver* receiver = web_state->GetJSInjectionReceiver();
  DCHECK(receiver);

  // Create the language detection controller.
  JsLanguageDetectionManager* language_detection_manager =
      static_cast<JsLanguageDetectionManager*>(
          [receiver instanceOfClass:[JsLanguageDetectionManager class]]);
  language_detection_controller_.reset(new LanguageDetectionController(
      web_state, language_detection_manager,
      translate_manager_->translate_client()->GetPrefs()));

  // Create the translate controller.
  JsTranslateManager* js_translate_manager = static_cast<JsTranslateManager*>(
      [receiver instanceOfClass:[JsTranslateManager class]]);
  translate_controller_.reset(
      new TranslateController(web_state, js_translate_manager));
  translate_controller_->set_observer(this);
}

IOSTranslateDriver::~IOSTranslateDriver() {
  if (web_state_) {
    web_state_->RemoveObserver(this);
    web_state_ = nullptr;
  }
}

language::IOSLanguageDetectionTabHelper::Callback
IOSTranslateDriver::CreateLanguageDetectionCallback() {
  return base::Bind(&IOSTranslateDriver::OnLanguageDetermined,
                    weak_method_factory_.GetWeakPtr());
}

void IOSTranslateDriver::OnLanguageDetermined(
    const translate::LanguageDetectionDetails& details) {
  if (!translate_manager_)
    return;
  translate_manager_->GetLanguageState().LanguageDetermined(details, true);

  translate_manager_->translate_client()->RecordLanguageDetectionEvent(details);

  if (web_state_)
    translate_manager_->InitiateTranslation(details.adopted_language);
}

// web::WebStateObserver methods

void IOSTranslateDriver::DidFinishNavigation(
    web::WebState* web_state,
    web::NavigationContext* navigation_context) {
  DCHECK_EQ(web_state_, web_state);

  // Interrupt pending translations and reset various data when a navigation
  // happens. Desktop does it by tracking changes in the page ID, and
  // through WebContentObserver, but these concepts do not exist on iOS.
  if (!navigation_context->IsSameDocument()) {
    ++page_seq_no_;
    translate_manager_->set_current_seq_no(page_seq_no_);
  }

  // TODO(droger): support navigation types, like content/ does.
  const bool reload = ui::PageTransitionCoreTypeIs(
      navigation_context->GetPageTransition(), ui::PAGE_TRANSITION_RELOAD);
  translate_manager_->GetLanguageState().DidNavigate(
      navigation_context->IsSameDocument(), true, reload);
}

void IOSTranslateDriver::WebStateDestroyed(web::WebState* web_state) {
  DCHECK_EQ(web_state_, web_state);
  web_state_->RemoveObserver(this);
  web_state_ = nullptr;
}

// TranslateDriver methods

bool IOSTranslateDriver::IsLinkNavigation() {
  return navigation_manager_->GetVisibleItem() &&
         ui::PageTransitionCoreTypeIs(
             navigation_manager_->GetVisibleItem()->GetTransitionType(),
             ui::PAGE_TRANSITION_LINK);
}

void IOSTranslateDriver::OnTranslateEnabledChanged() {
}

void IOSTranslateDriver::OnIsPageTranslatedChanged() {
}

void IOSTranslateDriver::TranslatePage(int page_seq_no,
                                       const std::string& translate_script,
                                       const std::string& source_lang,
                                       const std::string& target_lang) {
  if (page_seq_no != page_seq_no_)
    return;  // The user navigated away.
  source_language_ = source_lang;
  target_language_ = target_lang;
  pending_page_seq_no_ = page_seq_no;
  translate_controller_->InjectTranslateScript(translate_script);
}

void IOSTranslateDriver::RevertTranslation(int page_seq_no) {
  if (page_seq_no != page_seq_no_)
    return;  // The user navigated away.
  translate_controller_->RevertTranslation();
}

bool IOSTranslateDriver::IsIncognito() {
  return navigation_manager_->GetBrowserState()->IsOffTheRecord();
}

const std::string& IOSTranslateDriver::GetContentsMimeType() {
  return web_state_->GetContentsMimeType();
}

const GURL& IOSTranslateDriver::GetLastCommittedURL() {
  return web_state_->GetLastCommittedURL();
}

const GURL& IOSTranslateDriver::GetVisibleURL() {
  return web_state_->GetVisibleURL();
}

bool IOSTranslateDriver::HasCurrentPage() {
  return (navigation_manager_->GetVisibleItem() != nullptr);
}

void IOSTranslateDriver::OpenUrlInNewTab(const GURL& url) {
  web::WebState::OpenURLParams params(url, web::Referrer(),
                                      WindowOpenDisposition::NEW_FOREGROUND_TAB,
                                      ui::PAGE_TRANSITION_LINK, false);
  web_state_->OpenURL(params);
}

void IOSTranslateDriver::TranslationDidSucceed(
    const std::string& source_lang,
    const std::string& target_lang,
    int page_seq_no,
    const std::string& original_page_language,
    double translation_time) {
  if (!IsPageValid(page_seq_no))
    return;
  std::string actual_source_lang;
  translate::TranslateErrors::Type translate_errors = TranslateErrors::NONE;
  // Translation was successfull; if it was auto, retrieve the source
  // language the Translate Element detected.
  if (source_lang == kAutoDetectionLanguage) {
    actual_source_lang = original_page_language;
    if (actual_source_lang.empty()) {
      translate_errors = TranslateErrors::UNKNOWN_LANGUAGE;
    } else if (actual_source_lang == target_lang) {
      translate_errors = TranslateErrors::IDENTICAL_LANGUAGES;
    }
  } else {
    actual_source_lang = source_lang;
  }
  if (translate_errors == TranslateErrors::NONE)
    translate::ReportTimeToTranslate(translation_time);
  // Notify the manage of completion.
  translate_manager_->PageTranslated(actual_source_lang, target_lang,
                                     translate_errors);
}

bool IOSTranslateDriver::IsPageValid(int page_seq_no) const {
  bool user_navigated_away = page_seq_no != page_seq_no_;
  return !user_navigated_away && web_state_;
}

// TranslateController::Observer implementation.

void IOSTranslateDriver::OnTranslateScriptReady(
    TranslateErrors::Type error_type,
    double load_time,
    double ready_time) {
  if (!IsPageValid(pending_page_seq_no_))
    return;

  if (error_type != TranslateErrors::NONE) {
    translate_manager_->PageTranslated(source_language_, target_language_,
                                       error_type);
    return;
  }

  translate::ReportTimeToLoad(load_time);
  translate::ReportTimeToBeReady(ready_time);
  const char kAutoDetectionLanguage[] = "auto";
  std::string source = (source_language_ != translate::kUnknownLanguageCode)
                           ? source_language_
                           : kAutoDetectionLanguage;
  translate_controller_->StartTranslation(source_language_, target_language_);
}

void IOSTranslateDriver::OnTranslateComplete(
    TranslateErrors::Type error_type,
    const std::string& original_language,
    double translation_time) {
  if (!IsPageValid(pending_page_seq_no_))
    return;

  if (error_type != TranslateErrors::NONE) {
    translate_manager_->PageTranslated(source_language_, target_language_,
                                       error_type);
    return;
  }

  TranslationDidSucceed(source_language_, target_language_,
                        pending_page_seq_no_, original_language,
                        translation_time);
}

}  // namespace translate
