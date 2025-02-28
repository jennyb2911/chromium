// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_IOS_BROWSER_AUTOFILL_DRIVER_IOS_WEBFRAME_H_
#define COMPONENTS_AUTOFILL_IOS_BROWSER_AUTOFILL_DRIVER_IOS_WEBFRAME_H_

#include "components/autofill/ios/browser/autofill_driver_ios.h"
#include "ios/web/public/web_state/web_frame_user_data.h"
#include "ios/web/public/web_state/web_state_user_data.h"

namespace web {
class WebFrame;
class WebState;
}  // namespace web

namespace autofill {

class AutofillDriverIOSWebFrame;

// This factory will keep the parameters needed to create an
// AutofillDriverIOSWebFrame. These parameters only depend on the web_state, so
// there is one AutofillDriverIOSWebFrameFactory per WebState.
class AutofillDriverIOSWebFrameFactory
    : public web::WebStateUserData<AutofillDriverIOSWebFrameFactory> {
 public:
  // Creates a AutofillDriverIOSWebFrameFactory that will store all the
  // needed to create a AutofillDriverIOS.
  static void CreateForWebStateAndDelegate(
      web::WebState* web_state,
      AutofillClient* client,
      id<AutofillDriverIOSBridge> bridge,
      const std::string& app_locale,
      AutofillManager::AutofillDownloadManagerState enable_download_manager);
  ~AutofillDriverIOSWebFrameFactory() override;

  // Returns a AutofillDriverIOSFromWebFrame for |web_frame|, creating it if
  // needed.
  AutofillDriverIOSWebFrame* AutofillDriverIOSFromWebFrame(
      web::WebFrame* web_frame);

 private:
  AutofillDriverIOSWebFrameFactory(
      web::WebState* web_state,
      AutofillClient* client,
      id<AutofillDriverIOSBridge> bridge,
      const std::string& app_locale,
      AutofillManager::AutofillDownloadManagerState enable_download_manager);
  web::WebState* web_state_ = nullptr;
  AutofillClient* client_ = nullptr;
  id<AutofillDriverIOSBridge> bridge_ = nil;
  std::string app_locale_;
  AutofillManager::AutofillDownloadManagerState enable_download_manager_;
};

// TODO(crbug.com/883203): Merge with AutofillDriverIOS class once WebFrame is
// released.
class AutofillDriverIOSWebFrame
    : public AutofillDriverIOS,
      public web::WebFrameUserData<AutofillDriverIOSWebFrame> {
 public:
  // Creates a AutofillDriverIOSWebFrame for |web_frame|.
  static void CreateForWebFrameAndDelegate(
      web::WebState* web_state,
      web::WebFrame* web_frame,
      AutofillClient* client,
      id<AutofillDriverIOSBridge> bridge,
      const std::string& app_locale,
      AutofillManager::AutofillDownloadManagerState enable_download_manager);

  ~AutofillDriverIOSWebFrame() override;

 private:
  AutofillDriverIOSWebFrame(
      web::WebState* web_state,
      web::WebFrame* web_frame,
      AutofillClient* client,
      id<AutofillDriverIOSBridge> bridge,
      const std::string& app_locale,
      AutofillManager::AutofillDownloadManagerState enable_download_manager);
};
}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CONTENT_BROWSER_AUTOFILL_DRIVER_IOS_WEBSTATE_H_
