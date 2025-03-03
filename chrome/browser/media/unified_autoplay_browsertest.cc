// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/scoped_feature_list.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_frame_navigation_observer.h"
#include "content/public/test/test_navigation_observer.h"
#include "media/base/media_switches.h"
#include "net/dns/mock_host_resolver.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/platform/autoplay.mojom.h"

namespace {

static constexpr char const kFramedTestPagePath[] =
    "/media/autoplay_iframe.html";

static constexpr char const kTestPagePath[] = "/media/unified_autoplay.html";

}  // anonymous namespace

// Integration tests for the unified autoplay policy that require the //chrome
// layer.
// These tests are called "UnifiedAutoplayBrowserTest" in order to avoid name
// conflict with "AutoplayBrowserTest" in extensions code.
class UnifiedAutoplayBrowserTest : public InProcessBrowserTest {
 public:
  ~UnifiedAutoplayBrowserTest() override = default;

  void SetUpOnMainThread() override {
    scoped_feature_list_.InitAndEnableFeature(media::kUnifiedAutoplay);

    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());

    InProcessBrowserTest::SetUpOnMainThread();
  }

  content::WebContents* OpenNewTab(const GURL& url, bool from_context_menu) {
    return OpenInternal(
        url, from_context_menu, WindowOpenDisposition::NEW_FOREGROUND_TAB,
        false /* is_renderer_initiated */, true /* user_gesture */);
  }

  content::WebContents* OpenNewWindow(const GURL& url, bool from_context_menu) {
    return OpenInternal(
        url, from_context_menu, WindowOpenDisposition::NEW_WINDOW,
        false /* is_renderer_initiated */, true /* user_gesture */);
  }

  content::WebContents* OpenFromRenderer(const GURL& url, bool user_gesture) {
    return OpenInternal(url, false /* from_context_menu */,
                        WindowOpenDisposition::NEW_FOREGROUND_TAB,
                        true /* is_renderer_initiated */, user_gesture);
  }

  bool AttemptPlay(content::WebContents* web_contents) {
    bool played = false;
    EXPECT_TRUE(content::ExecuteScriptWithoutUserGestureAndExtractBool(
        web_contents, "attemptPlay();", &played));
    return played;
  }

  void SetAutoplayForceAllowFlag(const GURL& url) {
    blink::mojom::AutoplayConfigurationClientAssociatedPtr client;
    GetWebContents()
        ->GetMainFrame()
        ->GetRemoteAssociatedInterfaces()
        ->GetInterface(&client);
    client->AddAutoplayFlags(url::Origin::Create(url),
                             blink::mojom::kAutoplayFlagForceAllow);
  }

  content::WebContents* GetWebContents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

 private:
  content::WebContents* OpenInternal(const GURL& url,
                                     bool from_context_menu,
                                     WindowOpenDisposition disposition,
                                     bool is_renderer_initiated,
                                     bool user_gesture) {
    content::WebContents* active_contents =
        browser()->tab_strip_model()->GetActiveWebContents();

    content::Referrer referrer(
        active_contents->GetLastCommittedURL(),
        blink::WebReferrerPolicy::kWebReferrerPolicyAlways);

    content::OpenURLParams open_url_params(
        url, referrer, disposition, ui::PAGE_TRANSITION_LINK,
        is_renderer_initiated, from_context_menu);

    open_url_params.source_render_process_id =
        active_contents->GetMainFrame()->GetProcess()->GetID();
    open_url_params.source_render_frame_id =
        active_contents->GetMainFrame()->GetRoutingID();
    open_url_params.user_gesture = user_gesture;

    return active_contents->OpenURL(open_url_params);
  }

  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenSameOriginOutsideMenu) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  content::WebContents* new_contents = OpenNewTab(kTestPageUrl, false);
  content::WaitForLoadStop(new_contents);

  EXPECT_FALSE(AttemptPlay(new_contents));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenSameOriginFromMenu) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  content::WebContents* new_contents = OpenNewTab(kTestPageUrl, true);
  content::WaitForLoadStop(new_contents);

  EXPECT_TRUE(AttemptPlay(new_contents));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenCrossOriginFromMenu) {
  ui_test_utils::NavigateToURL(
      browser(),
      embedded_test_server()->GetURL("foo.example.com", kTestPagePath));

  content::WebContents* new_contents = OpenNewTab(
      embedded_test_server()->GetURL("bar.example.com", kTestPagePath), true);
  content::WaitForLoadStop(new_contents);

  EXPECT_TRUE(AttemptPlay(new_contents));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenCrossDomainFromMenu) {
  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL(kTestPagePath));

  content::WebContents* new_contents = OpenNewTab(
      embedded_test_server()->GetURL("example.com", kTestPagePath), true);
  content::WaitForLoadStop(new_contents);

  EXPECT_FALSE(AttemptPlay(new_contents));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenWindowFromContextMenu) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  content::WebContents* new_contents = OpenNewTab(kTestPageUrl, true);
  content::WaitForLoadStop(new_contents);

  EXPECT_TRUE(AttemptPlay(new_contents));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenWindowNotContextMenu) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  content::WebContents* new_contents = OpenNewTab(kTestPageUrl, false);
  content::WaitForLoadStop(new_contents);

  EXPECT_FALSE(AttemptPlay(new_contents));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenFromRendererGesture) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  content::WebContents* new_contents = OpenFromRenderer(kTestPageUrl, true);
  content::WaitForLoadStop(new_contents);

  EXPECT_TRUE(AttemptPlay(new_contents));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, OpenFromRendererNoGesture) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  content::WebContents* new_contents = OpenFromRenderer(kTestPageUrl, false);
  EXPECT_EQ(nullptr, new_contents);
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, NoBypassUsingAutoplayFlag) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  EXPECT_FALSE(AttemptPlay(GetWebContents()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, BypassUsingAutoplayFlag) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  SetAutoplayForceAllowFlag(kTestPageUrl);
  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  EXPECT_TRUE(AttemptPlay(GetWebContents()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest,
                       BypassUsingAutoplayFlag_SameDocument) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  SetAutoplayForceAllowFlag(kTestPageUrl);
  ui_test_utils::NavigateToURL(browser(), kTestPageUrl);

  // Simulate a same document navigation by navigating to #test.
  GURL::Replacements replace_ref;
  replace_ref.SetRefStr("test");
  ui_test_utils::NavigateToURL(browser(),
                               kTestPageUrl.ReplaceComponents(replace_ref));

  EXPECT_TRUE(AttemptPlay(GetWebContents()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, ForceWasActivated_Default) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  NavigateParams params(browser(), kTestPageUrl, ui::PAGE_TRANSITION_LINK);
  ui_test_utils::NavigateToURL(&params);

  EXPECT_FALSE(AttemptPlay(GetWebContents()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplayBrowserTest, ForceWasActivated_Yes) {
  const GURL kTestPageUrl = embedded_test_server()->GetURL(kTestPagePath);

  NavigateParams params(browser(), kTestPageUrl, ui::PAGE_TRANSITION_LINK);
  params.was_activated = content::WasActivatedOption::kYes;
  ui_test_utils::NavigateToURL(&params);

  EXPECT_TRUE(AttemptPlay(GetWebContents()));
}

// Integration tests for the new unified autoplay sound settings UI.

class UnifiedAutoplaySettingBrowserTest : public UnifiedAutoplayBrowserTest {
 public:
  ~UnifiedAutoplaySettingBrowserTest() override = default;

  void SetUpOnMainThread() override {
    scoped_feature_list_.InitAndEnableFeature(media::kAutoplaySoundSettings);
    UnifiedAutoplayBrowserTest::SetUpOnMainThread();
  }

  bool AutoplayAllowed(const content::ToRenderFrameHost& adapter) {
    bool played = false;
    EXPECT_TRUE(content::ExecuteScriptWithoutUserGestureAndExtractBool(
        adapter, "tryPlayback();", &played));
    return played;
  }

  void NavigateFrameAndWait(content::RenderFrameHost* rfh, const GURL& url) {
    content::TestFrameNavigationObserver observer(rfh);
    content::NavigationController::LoadURLParams params(url);
    params.transition_type = ui::PAGE_TRANSITION_LINK;
    params.frame_tree_node_id = rfh->GetFrameTreeNodeId();
    content::WebContents::FromRenderFrameHost(rfh)
        ->GetController()
        .LoadURLWithParams(params);
    observer.Wait();
  }

  HostContentSettingsMap* GetSettingsMap() {
    return HostContentSettingsMapFactory::GetForProfile(
        Profile::FromBrowserContext(web_contents()->GetBrowserContext()));
  }

  content::RenderFrameHost* main_frame() const {
    return web_contents()->GetAllFrames()[0];
  }

  content::RenderFrameHost* first_child() const {
    return web_contents()->GetAllFrames()[1];
  }

 private:
  content::WebContents* web_contents() const {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(UnifiedAutoplaySettingBrowserTest, Allow) {
  GURL main_url(
      embedded_test_server()->GetURL("example.com", kFramedTestPagePath));
  GURL foo_url(embedded_test_server()->GetURL("foo.com", kFramedTestPagePath));

  GetSettingsMap()->SetContentSettingDefaultScope(
      main_url, main_url, CONTENT_SETTINGS_TYPE_SOUND, std::string(),
      CONTENT_SETTING_ALLOW);

  NavigateFrameAndWait(main_frame(), main_url);
  NavigateFrameAndWait(first_child(), foo_url);

  EXPECT_TRUE(AutoplayAllowed(main_frame()));
  EXPECT_TRUE(AutoplayAllowed(first_child()));

  // Simulate a same document navigation by navigating to #test.
  GURL::Replacements replace_ref;
  replace_ref.SetRefStr("test");
  ui_test_utils::NavigateToURL(browser(),
                               main_url.ReplaceComponents(replace_ref));

  EXPECT_TRUE(AutoplayAllowed(main_frame()));
  EXPECT_TRUE(AutoplayAllowed(first_child()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplaySettingBrowserTest, Allow_Wildcard) {
  GURL main_url(
      embedded_test_server()->GetURL("example.com", kFramedTestPagePath));
  GURL foo_url(embedded_test_server()->GetURL("foo.org", kFramedTestPagePath));
  GURL bar_url(embedded_test_server()->GetURL("bar.com", kFramedTestPagePath));

  // Set a wildcard allow sound setting for *.com.
  ContentSettingsPattern pattern(ContentSettingsPattern::FromString("[*.]com"));
  GetSettingsMap()->SetWebsiteSettingCustomScope(
      pattern, pattern, CONTENT_SETTINGS_TYPE_SOUND, std::string(),
      std::make_unique<base::Value>(CONTENT_SETTING_ALLOW));

  NavigateFrameAndWait(main_frame(), main_url);
  EXPECT_TRUE(AutoplayAllowed(main_frame()));

  NavigateFrameAndWait(main_frame(), foo_url);
  EXPECT_FALSE(AutoplayAllowed(main_frame()));

  NavigateFrameAndWait(main_frame(), bar_url);
  EXPECT_TRUE(AutoplayAllowed(main_frame()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplaySettingBrowserTest, Block) {
  GURL main_url(
      embedded_test_server()->GetURL("example.com", kFramedTestPagePath));
  GURL foo_url(embedded_test_server()->GetURL("foo.com", kFramedTestPagePath));

  GetSettingsMap()->SetContentSettingDefaultScope(
      main_url, main_url, CONTENT_SETTINGS_TYPE_SOUND, std::string(),
      CONTENT_SETTING_BLOCK);

  NavigateFrameAndWait(main_frame(), main_url);
  NavigateFrameAndWait(first_child(), foo_url);

  EXPECT_FALSE(AutoplayAllowed(main_frame()));
  EXPECT_FALSE(AutoplayAllowed(first_child()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplaySettingBrowserTest, Block_Wildcard) {
  GURL main_url(
      embedded_test_server()->GetURL("example.com", kFramedTestPagePath));
  GURL foo_url(embedded_test_server()->GetURL("foo.org", kFramedTestPagePath));
  GURL bar_url(embedded_test_server()->GetURL("bar.com", kFramedTestPagePath));

  // Set a wildcard block sound setting for *.com.
  ContentSettingsPattern pattern(ContentSettingsPattern::FromString("[*.]com"));
  GetSettingsMap()->SetWebsiteSettingCustomScope(
      pattern, pattern, CONTENT_SETTINGS_TYPE_SOUND, std::string(),
      std::make_unique<base::Value>(CONTENT_SETTING_BLOCK));

  GetSettingsMap()->SetContentSettingDefaultScope(
      foo_url, foo_url, CONTENT_SETTINGS_TYPE_SOUND, std::string(),
      CONTENT_SETTING_ALLOW);

  NavigateFrameAndWait(main_frame(), main_url);
  EXPECT_FALSE(AutoplayAllowed(main_frame()));

  NavigateFrameAndWait(main_frame(), foo_url);
  EXPECT_TRUE(AutoplayAllowed(main_frame()));

  NavigateFrameAndWait(main_frame(), bar_url);
  EXPECT_FALSE(AutoplayAllowed(main_frame()));
}

IN_PROC_BROWSER_TEST_F(UnifiedAutoplaySettingBrowserTest, DefaultAllow) {
  GURL main_url(
      embedded_test_server()->GetURL("example.com", kFramedTestPagePath));
  GURL foo_url(embedded_test_server()->GetURL("foo.com", kFramedTestPagePath));

  EXPECT_EQ(
      CONTENT_SETTING_ALLOW,
      GetSettingsMap()->GetContentSetting(
          main_url, main_url, CONTENT_SETTINGS_TYPE_SOUND, std::string()));

  NavigateFrameAndWait(main_frame(), main_url);
  NavigateFrameAndWait(first_child(), foo_url);

  EXPECT_FALSE(AutoplayAllowed(main_frame()));
  EXPECT_FALSE(AutoplayAllowed(first_child()));
}
