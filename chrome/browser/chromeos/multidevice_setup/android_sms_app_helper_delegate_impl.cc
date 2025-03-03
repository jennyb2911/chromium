// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/multidevice_setup/android_sms_app_helper_delegate_impl.h"

#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "chrome/browser/chromeos/android_sms/android_sms_urls.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/extensions/app_launch_params.h"
#include "chrome/browser/ui/extensions/application_launch.h"
#include "chrome/browser/web_applications/components/install_result_code.h"
#include "chrome/browser/web_applications/components/pending_app_manager.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chromeos/components/proximity_auth/logging/logging.h"
#include "extensions/common/constants.h"
#include "ui/base/window_open_disposition.h"

namespace chromeos {

namespace multidevice_setup {

AndroidSmsAppHelperDelegateImpl::AndroidSmsAppHelperDelegateImpl(
    Profile* profile)
    : pending_app_manager_(
          &web_app::WebAppProvider::Get(profile)->pending_app_manager()),
      profile_(profile),
      weak_ptr_factory_(this) {}

AndroidSmsAppHelperDelegateImpl::AndroidSmsAppHelperDelegateImpl(
    web_app::PendingAppManager* pending_app_manager)
    : pending_app_manager_(pending_app_manager), weak_ptr_factory_(this) {}

AndroidSmsAppHelperDelegateImpl::~AndroidSmsAppHelperDelegateImpl() = default;

void AndroidSmsAppHelperDelegateImpl::InstallAndroidSmsApp(
    bool launch_on_install) {
  // TODO(crbug.com/874605): Consider retries and error handling here. This call
  // can easily fail.
  pending_app_manager_->Install(
      web_app::PendingAppManager::AppInfo(
          chromeos::android_sms::GetAndroidMessagesURLWithExperiments(),
          web_app::PendingAppManager::LaunchContainer::kWindow,
          web_app::PendingAppManager::InstallSource::kInternal,
          web_app::PendingAppManager::AppInfo::kDefaultCreateShortcuts,
          true),  // override_previous_user_uninstall
      base::BindOnce(&AndroidSmsAppHelperDelegateImpl::OnAppInstalled,
                     weak_ptr_factory_.GetWeakPtr(), launch_on_install));
}

void AndroidSmsAppHelperDelegateImpl::InstallAndroidSmsApp() {
  InstallAndroidSmsApp(false /* launch_on_install */);
}

void AndroidSmsAppHelperDelegateImpl::InstallAndLaunchAndroidSmsApp() {
  const extensions::Extension* android_sms_pwa =
      extensions::util::GetInstalledPwaForUrl(
          profile_, chromeos::android_sms::GetAndroidMessagesURL());
  if (!android_sms_pwa) {
    PA_LOG(INFO) << "No Messages app found. Installing it.";
    InstallAndroidSmsApp(true /* launch_on_install */);
    return;
  }

  LaunchAndroidSmsApp();
}

void AndroidSmsAppHelperDelegateImpl::LaunchAndroidSmsApp() {
  const extensions::Extension* android_sms_pwa =
      extensions::util::GetInstalledPwaForUrl(
          profile_, chromeos::android_sms::GetAndroidMessagesURL());
  DCHECK(android_sms_pwa);

  PA_LOG(INFO) << "Messages app Launching...";
  AppLaunchParams params(
      profile_, android_sms_pwa, extensions::LAUNCH_CONTAINER_WINDOW,
      WindowOpenDisposition::NEW_WINDOW, extensions::SOURCE_CHROME_INTERNAL);
  // OpenApplications() is defined in application_launch.h.
  OpenApplication(params);
}

void AndroidSmsAppHelperDelegateImpl::OnAppInstalled(
    bool launch_on_install,
    const GURL& app_url,
    web_app::InstallResultCode code) {
  if (code == web_app::InstallResultCode::kSuccess) {
    PA_LOG(INFO) << "Messages app installed! URL: " << app_url;
    if (launch_on_install)
      LaunchAndroidSmsApp();
  } else {
    PA_LOG(WARNING) << "Messages app failed to install! URL: " << app_url
                    << ", InstallResultCode: " << static_cast<int>(code);
  }
}

}  // namespace multidevice_setup

}  // namespace chromeos
