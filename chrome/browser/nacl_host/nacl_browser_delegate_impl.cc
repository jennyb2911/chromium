// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/nacl_host/nacl_browser_delegate_impl.h"

#include <stddef.h>

#include <vector>

#include "base/macros.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/task/post_task.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/component_updater/pnacl_component_installer.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/nacl_host/nacl_infobar_delegate.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/renderer_host/pepper/chrome_browser_pepper_host_factory.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/common/logging_chrome.h"
#include "chrome/common/pepper_permission_util.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/buildflags/buildflags.h"
#include "url/gurl.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "chrome/browser/extensions/extension_service.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/info_map.h"
#include "extensions/browser/process_manager.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/url_pattern.h"
#endif

namespace {

// These are temporarily needed for testing non-sfi mode on ChromeOS without
// passing command-line arguments to Chrome.
const char* const kAllowedNonSfiOrigins[] = {
    "6EAED1924DB611B6EEF2A664BD077BE7EAD33B8F",  // see http://crbug.com/355141
    "4EB74897CB187C7633357C2FE832E0AD6A44883A"   // see http://crbug.com/355141
};

}  // namespace

NaClBrowserDelegateImpl::NaClBrowserDelegateImpl(
    ProfileManager* profile_manager)
    : profile_manager_(profile_manager), inverse_debug_patterns_(false) {
  DCHECK(profile_manager_);
  for (size_t i = 0; i < arraysize(kAllowedNonSfiOrigins); ++i) {
    allowed_nonsfi_origins_.insert(kAllowedNonSfiOrigins[i]);
  }
}

NaClBrowserDelegateImpl::~NaClBrowserDelegateImpl() {
}

void NaClBrowserDelegateImpl::ShowMissingArchInfobar(int render_process_id,
                                                     int render_view_id) {
  base::PostTaskWithTraits(FROM_HERE, {content::BrowserThread::UI},
                           base::BindOnce(&CreateInfoBarOnUiThread,
                                          render_process_id, render_view_id));
}

bool NaClBrowserDelegateImpl::DialogsAreSuppressed() {
  return logging::DialogsAreSuppressed();
}

bool NaClBrowserDelegateImpl::GetCacheDirectory(base::FilePath* cache_dir) {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir))
    return false;
  chrome::GetUserCacheDirectory(user_data_dir, cache_dir);
  return true;
}

bool NaClBrowserDelegateImpl::GetPluginDirectory(base::FilePath* plugin_dir) {
  return base::PathService::Get(chrome::DIR_INTERNAL_PLUGINS, plugin_dir);
}

bool NaClBrowserDelegateImpl::GetPnaclDirectory(base::FilePath* pnacl_dir) {
  return base::PathService::Get(chrome::DIR_PNACL_COMPONENT, pnacl_dir);
}

bool NaClBrowserDelegateImpl::GetUserDirectory(base::FilePath* user_dir) {
  return base::PathService::Get(chrome::DIR_USER_DATA, user_dir);
}

std::string NaClBrowserDelegateImpl::GetVersionString() const {
  return chrome::GetVersionString();
}

ppapi::host::HostFactory* NaClBrowserDelegateImpl::CreatePpapiHostFactory(
    content::BrowserPpapiHost* ppapi_host) {
  return new ChromeBrowserPepperHostFactory(ppapi_host);
}

void NaClBrowserDelegateImpl::SetDebugPatterns(
    const std::string& debug_patterns) {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (debug_patterns.empty()) {
    return;
  }
  std::vector<std::string> patterns;
  if (debug_patterns[0] == '!') {
    std::string negated_patterns = debug_patterns;
    inverse_debug_patterns_ = true;
    negated_patterns.erase(0, 1);
    patterns = base::SplitString(
        negated_patterns, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  } else {
    patterns = base::SplitString(
        debug_patterns, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  }
  for (const std::string& pattern_str : patterns) {
    // Allow chrome:// schema, which is used to filter out the internal
    // PNaCl translator. Also allow chrome-extension:// schema (which
    // can have NaCl modules). The default is to disallow these schema
    // since they can be dangerous in the context of chrome extension
    // permissions, but they are okay here, for NaCl GDB avoidance.
    URLPattern pattern(URLPattern::SCHEME_ALL);
    if (pattern.Parse(pattern_str) == URLPattern::ParseResult::kSuccess) {
      // If URL pattern has scheme equal to *, Parse method resets valid
      // schemes mask to http and https only, so we need to reset it after
      // Parse to re-include chrome-extension and chrome schema.
      pattern.SetValidSchemes(URLPattern::SCHEME_ALL);
      debug_patterns_.push_back(pattern);
    }
  }
#endif  // BUILDFLAG(ENABLE_EXTENSIONS)
}

bool NaClBrowserDelegateImpl::URLMatchesDebugPatterns(
    const GURL& manifest_url) {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  // Empty patterns are forbidden so we ignore them.
  if (debug_patterns_.empty()) {
    return true;
  }
  bool matches = false;
  for (std::vector<URLPattern>::iterator iter = debug_patterns_.begin();
       iter != debug_patterns_.end(); ++iter) {
    if (iter->MatchesURL(manifest_url)) {
      matches = true;
      break;
    }
  }
  if (inverse_debug_patterns_) {
    return !matches;
  } else {
    return matches;
  }
#else
  return false;
#endif  // BUILDFLAG(ENABLE_EXTENSIONS)
}

// This function is security sensitive.  Be sure to check with a security
// person before you modify it.
bool NaClBrowserDelegateImpl::MapUrlToLocalFilePath(
    const GURL& file_url,
    bool use_blocking_api,
    const base::FilePath& profile_directory,
    base::FilePath* file_path) {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  scoped_refptr<extensions::InfoMap> extension_info_map =
      GetExtensionInfoMap(profile_directory);
  return extension_info_map->MapUrlToLocalFilePath(
      file_url, use_blocking_api, file_path);
#else
  return false;
#endif
}

bool NaClBrowserDelegateImpl::IsNonSfiModeAllowed(
    const base::FilePath& profile_directory,
    const GURL& manifest_url) {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  const extensions::ExtensionSet* extension_set =
      &GetExtensionInfoMap(profile_directory)->extensions();
  return IsExtensionOrSharedModuleWhitelisted(manifest_url, extension_set,
                                              allowed_nonsfi_origins_);
#else
  return false;
#endif
}

// static
void NaClBrowserDelegateImpl::CreateInfoBarOnUiThread(int render_process_id,
                                                      int render_view_id) {
  content::RenderViewHost* rvh =
      content::RenderViewHost::FromID(render_process_id, render_view_id);
  if (!rvh)
    return;
  content::WebContents* web_contents =
      content::WebContents::FromRenderViewHost(rvh);
  if (!web_contents)
    return;
  InfoBarService* infobar_service =
      InfoBarService::FromWebContents(web_contents);
  if (infobar_service)
    NaClInfoBarDelegate::Create(infobar_service);
}

#if BUILDFLAG(ENABLE_EXTENSIONS)
scoped_refptr<extensions::InfoMap> NaClBrowserDelegateImpl::GetExtensionInfoMap(
    const base::FilePath& profile_directory) {
  // Get the profile associated with the renderer.
  Profile* profile = profile_manager_->GetProfileByPath(profile_directory);
  DCHECK(profile);
  scoped_refptr<extensions::InfoMap> extension_info_map =
      extensions::ExtensionSystem::Get(profile)->info_map();
  DCHECK(extension_info_map.get());
  return extension_info_map;
}
#endif
