// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/bookmark_apps/external_web_apps.h"

#include <memory>
#include <string>
#include <utility>

#include "base/callback.h"
#include "base/files/file_enumerator.h"
#include "base/json/json_file_value_serializer.h"
#include "base/path_service.h"
#include "base/task/post_task.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/browser/browser_thread.h"
#include "url/gurl.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#endif

namespace {

// kAppUrl is a required string specifying a URL inside the scope of the web
// app that contains a link to the app manifest.
constexpr char kAppUrl[] = "app_url";

// kCreateShortcuts is an optional boolean which controls whether OS
// level shortcuts are created. On Chrome OS this controls whether the app is
// pinned to the shelf.
// The default value of kCreateShortcuts if false.
constexpr char kCreateShortcuts[] = "create_shortcuts";

// kLaunchContainer is a required string which can be "window" or "tab"
// and controls what sort of container the web app is launched in.
constexpr char kLaunchContainer[] = "launch_container";
constexpr char kLaunchContainerTab[] = "tab";
constexpr char kLaunchContainerWindow[] = "window";

#if defined(OS_CHROMEOS)
// The sub-directory of the extensions directory in which to scan for external
// web apps (as opposed to external extensions or external ARC apps).
const base::FilePath::CharType kWebAppsSubDirectory[] =
    FILE_PATH_LITERAL("web_apps");
#endif

std::vector<web_app::PendingAppManager::AppInfo> ScanDir(base::FilePath dir) {
  base::ScopedBlockingCall scoped_blocking_call(base::BlockingType::MAY_BLOCK);
  base::FilePath::StringType extension(FILE_PATH_LITERAL(".json"));
  base::FileEnumerator json_files(dir,
                                  false,  // Recursive.
                                  base::FileEnumerator::FILES);

  std::vector<web_app::PendingAppManager::AppInfo> app_infos;

  for (base::FilePath file = json_files.Next(); !file.empty();
       file = json_files.Next()) {
    if (!file.MatchesExtension(extension)) {
      continue;
    }

    // TODO(benwells): Remove deprecated use of base::DictionaryValue API.
    JSONFileValueDeserializer deserializer(file);
    std::string error_msg;
    std::unique_ptr<base::Value> value =
        deserializer.Deserialize(nullptr, &error_msg);
    if (!value) {
      VLOG(2) << file.value() << " was not valid JSON: " << error_msg;
      continue;
    }
    if (value->type() != base::Value::Type::DICTIONARY) {
      VLOG(2) << file.value() << " was not a dictionary as the top level";
      continue;
    }
    std::unique_ptr<base::DictionaryValue> dict_value =
        base::DictionaryValue::From(std::move(value));

    std::string app_url_str;
    if (!dict_value->GetString(kAppUrl, &app_url_str) || app_url_str.empty()) {
      VLOG(2) << file.value() << " had an invalid " << kAppUrl;
      continue;
    }
    GURL app_url(app_url_str);
    if (!app_url.is_valid()) {
      VLOG(2) << file.value() << " had an invalid " << kAppUrl;
      continue;
    }

    bool create_shortcuts = false;
    if (dict_value->HasKey(kCreateShortcuts) &&
        !dict_value->GetBoolean(kCreateShortcuts, &create_shortcuts)) {
      VLOG(2) << file.value() << " had an invalid " << kCreateShortcuts;
      continue;
    }

    web_app::PendingAppManager::LaunchContainer launch_container =
        web_app::PendingAppManager::LaunchContainer::kTab;
    std::string launch_container_str;
    if (!dict_value->GetString(kLaunchContainer, &launch_container_str)) {
      VLOG(2) << file.value() << " had an invalid " << kLaunchContainer;
      continue;
    }
    if (launch_container_str == kLaunchContainerTab) {
      launch_container = web_app::PendingAppManager::LaunchContainer::kTab;
    } else if (launch_container_str == kLaunchContainerWindow) {
      launch_container = web_app::PendingAppManager::LaunchContainer::kWindow;
    } else {
      VLOG(2) << file.value() << " had an invalid " << kLaunchContainer;
      continue;
    }

    app_infos.emplace_back(
        std::move(app_url), launch_container,
        web_app::PendingAppManager::InstallSource::kExternalDefault,
        create_shortcuts);
  }

  return app_infos;
}

base::FilePath DetermineScanDir(Profile* profile) {
  base::FilePath dir;
#if defined(OS_CHROMEOS)
  // As of mid 2018, only Chrome OS has default/external web apps, and
  // chrome::DIR_STANDALONE_EXTERNAL_EXTENSIONS is only defined for OS_LINUX,
  // which includes OS_CHROMEOS.

  if (chromeos::ProfileHelper::IsPrimaryProfile(profile)) {
    // For manual testing, you can change s/STANDALONE/USER/, as writing to
    // "$HOME/.config/chromium/test-user/.config/chromium/External
    // Extensions/web_apps" does not require root ACLs, unlike
    // "/usr/share/chromium/extensions/web_apps".
    if (!base::PathService::Get(chrome::DIR_STANDALONE_EXTERNAL_EXTENSIONS,
                                &dir)) {
      LOG(ERROR) << "ScanForExternalWebApps: base::PathService::Get failed";
    } else {
      dir = dir.Append(kWebAppsSubDirectory);
    }
  }

#endif
  return dir;
}

}  // namespace

namespace web_app {

std::vector<web_app::PendingAppManager::AppInfo>
ScanDirForExternalWebAppsForTesting(base::FilePath dir) {
  return ScanDir(dir);
}

void ScanForExternalWebApps(Profile* profile,
                            ScanForExternalWebAppsCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  base::FilePath dir = DetermineScanDir(profile);
  if (dir.empty()) {
    std::move(callback).Run(std::vector<web_app::PendingAppManager::AppInfo>());
    return;
  }
  // Do a two-part callback dance, across different TaskRunners.
  //
  // 1. Schedule ScanDir to happen on a background thread, so that we don't
  // block the UI thread. When that's done,
  // base::PostTaskWithTraitsAndReplyWithResult will bounce us back to the
  // originating thread (the UI thread).
  //
  // 2. In |callback|, forward the vector of AppInfo's on to the
  // pending_app_manager_, which can only be called on the UI thread.
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&ScanDir, dir), std::move(callback));
}

}  //  namespace web_app
