// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/chrome_cleaner/srt_chrome_prompt_impl.h"

#include <algorithm>
#include <utility>

#include "base/files/file_path.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/extensions/extension_service.h"
#include "components/crx_file/id_util.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/disable_reason.h"

namespace safe_browsing {

using chrome_cleaner::mojom::ChromePrompt;
using chrome_cleaner::mojom::ChromePromptRequest;
using chrome_cleaner::mojom::PromptAcceptance;
using content::BrowserThread;

ChromePromptImpl::ChromePromptImpl(
    extensions::ExtensionService* extension_service,
    ChromePromptRequest request,
    base::RepeatingClosure on_connection_closed,
    OnPromptUser on_prompt_user)
    : binding_(this, std::move(request)),
      extension_service_(extension_service),
      on_prompt_user_(std::move(on_prompt_user)) {
  DCHECK(on_prompt_user_);
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  binding_.set_connection_error_handler(std::move(on_connection_closed));
}

ChromePromptImpl::~ChromePromptImpl() {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
}

void ChromePromptImpl::PromptUser(
    const std::vector<base::FilePath>& files_to_delete,
    const base::Optional<std::vector<base::string16>>& registry_keys,
    const base::Optional<std::vector<base::string16>>& extension_ids,
    ChromePrompt::PromptUserCallback callback) {
  using FileCollection = ChromeCleanerScannerResults::FileCollection;
  using RegistryKeyCollection =
      ChromeCleanerScannerResults::RegistryKeyCollection;
  using ExtensionCollection = ChromeCleanerScannerResults::ExtensionCollection;

  if (on_prompt_user_) {
    ChromeCleanerScannerResults scanner_results(
        FileCollection(files_to_delete.begin(), files_to_delete.end()),
        registry_keys ? RegistryKeyCollection(registry_keys->begin(),
                                              registry_keys->end())
                      : RegistryKeyCollection(),
        extension_ids
            ? ExtensionCollection(extension_ids->begin(), extension_ids->end())
            : ExtensionCollection());
    std::move(on_prompt_user_)
        .Run(std::move(scanner_results), std::move(callback));
  }
}

void ChromePromptImpl::DisableExtensions(
    const std::vector<base::string16>& extension_ids,
    ChromePrompt::DisableExtensionsCallback callback) {
  if (extension_service_ == nullptr) {
    std::move(callback).Run(false);
    return;
  }
  bool ids_are_valid = std::all_of(
      extension_ids.begin(), extension_ids.end(), [](const base::string16& id) {
        return crx_file::id_util::IdIsValid(base::UTF16ToUTF8(id));
      });
  if (!ids_are_valid) {
    std::move(callback).Run(false);
    return;
  }

  int reason = extensions::disable_reason::DISABLE_EXTERNAL_EXTENSION;
  for (const base::string16& extension_id : extension_ids) {
    extension_service_->DisableExtension(base::UTF16ToUTF8(extension_id),
                                         reason);
  }
  std::move(callback).Run(true);
}

}  // namespace safe_browsing
