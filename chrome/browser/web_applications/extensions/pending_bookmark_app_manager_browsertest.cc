// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/extensions/pending_bookmark_app_manager.h"

#include "base/bind.h"
#include "base/callback.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/test/bind_test_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/web_applications/components/install_result_code.h"
#include "chrome/browser/web_applications/extensions/web_app_extension_ids_map.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "net/test/embedded_test_server/embedded_test_server.h"

namespace extensions {

class PendingBookmarkAppManagerBrowserTest : public InProcessBrowserTest {};

// Basic integration test to make sure the whole flow works. Each step in the
// flow is unit tested separately.
IN_PROC_BROWSER_TEST_F(PendingBookmarkAppManagerBrowserTest, InstallSucceeds) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url(embedded_test_server()->GetURL("/banners/manifest_test_page.html"));
  base::RunLoop run_loop;
  base::Optional<web_app::InstallResultCode> result_code;

  web_app::WebAppProvider::Get(browser()->profile())
      ->pending_app_manager()
      .Install(web_app::PendingAppManager::AppInfo(
                   url, web_app::PendingAppManager::LaunchContainer::kWindow,
                   web_app::PendingAppManager::InstallSource::kInternal,
                   false /* create_shortcuts */),  // Avoid creating real
                                                   // shortcuts in tests.
               base::BindLambdaForTesting(
                   [&run_loop, &result_code](const GURL& provided_url,
                                             web_app::InstallResultCode code) {
                     result_code = code;
                     run_loop.QuitClosure().Run();
                   }));
  run_loop.Run();

  ASSERT_TRUE(result_code.has_value());
  EXPECT_EQ(web_app::InstallResultCode::kSuccess, result_code.value());
  base::Optional<std::string> id =
      web_app::ExtensionIdsMap(browser()->profile()->GetPrefs())
          .LookupExtensionId(url);
  ASSERT_TRUE(id.has_value());
  const Extension* app = ExtensionRegistry::Get(browser()->profile())
                             ->enabled_extensions()
                             .GetByID(id.value());
  ASSERT_TRUE(app);
  EXPECT_EQ("Manifest test app", app->name());
}

// Tests that the browser doesn't crash if it gets shutdown with a pending
// installation.
IN_PROC_BROWSER_TEST_F(PendingBookmarkAppManagerBrowserTest,
                       ShutdownWithPendingInstallation) {
  ASSERT_TRUE(embedded_test_server()->Start());

  // Start an installation but don't wait for it to finish.
  web_app::WebAppProvider::Get(browser()->profile())
      ->pending_app_manager()
      .Install(web_app::PendingAppManager::AppInfo(
                   embedded_test_server()->GetURL(
                       "/banners/manifest_test_page.html"),
                   web_app::PendingAppManager::LaunchContainer::kWindow,
                   web_app::PendingAppManager::InstallSource::kInternal,
                   false /* create_shortcuts */),  // Avoid creating real
                                                   // shortcuts in tests.
               base::DoNothing());

  // The browser should shutdown cleanly even if there is a pending
  // installation.
}

}  // namespace extensions
