// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PREVIEWS_CONTENT_PREVIEWS_HINTS_H_
#define COMPONENTS_PREVIEWS_CONTENT_PREVIEWS_HINTS_H_

#include <map>
#include <memory>
#include <set>

#include "base/macros.h"
#include "base/sequence_checker.h"
#include "components/optimization_guide/proto/hints.pb.h"
#include "components/previews/content/hint_cache.h"
#include "components/previews/content/previews_hints.h"
#include "components/previews/core/host_filter.h"
#include "components/previews/core/previews_user_data.h"
#include "components/url_matcher/url_matcher.h"

class GURL;

namespace optimization_guide {
struct ComponentInfo;
}

namespace previews {

// Holds previews hints extracted from the configuration.
class PreviewsHints {
 public:
  ~PreviewsHints();

  // Creates a Hints instance from the provided configuration.
  static std::unique_ptr<PreviewsHints> CreateFromConfig(
      const optimization_guide::proto::Configuration& config,
      const optimization_guide::ComponentInfo& info);

  static std::unique_ptr<PreviewsHints> CreateForTesting(
      std::unique_ptr<HostFilter> lite_page_redirect_blacklist);

  // Returns the matching PageHint for |document_url| if found in |hint|.
  // TODO(dougarnett): Consider moving to some hint_util file.
  static const optimization_guide::proto::PageHint* FindPageHint(
      const GURL& document_url,
      const optimization_guide::proto::Hint& hint);

  void Initialize();

  // Whether the URL is whitelisted for the given previews type. If so,
  // |out_inflation_percent| will be populated if meta data available for it.
  // This first checks the top-level whitelist and, if not whitelisted there,
  // it will check the HintCache for having a loaded, matching PageHint that
  // whitelists it.
  bool IsWhitelisted(const GURL& url,
                     PreviewsType type,
                     int* out_inflation_percent);

  // Whether the URL is blacklisted for the given previews type.
  bool IsBlacklisted(const GURL& url, PreviewsType type);

  // Returns whether |url| may have PageHints and triggers asynchronous load
  // of such hints are not currently available synchronously. |callback| is
  // called if any applicable hint data is loaded and available for |url|.
  bool MaybeLoadOptimizationHints(const GURL& url,
                                  HintLoadedCallback callback) const;

 private:
  friend class PreviewsHintsTest;

  PreviewsHints();

  // Parses optimization filters from |config| and populates corresponding
  // supported blacklists in this object.
  void ParseOptimizationFilters(
      const optimization_guide::proto::Configuration& config);

  // The URLMatcher used to match whether a URL has any hints associated with
  // it.
  url_matcher::URLMatcher url_matcher_;

  // Holds the hint cache (if any optimizations using it are enabled).
  std::unique_ptr<HintCache> hint_cache_;

  // A map from the condition set ID to associated whitelist Optimization
  // details.
  std::map<url_matcher::URLMatcherConditionSet::ID,
           std::set<std::pair<PreviewsType, int>>>
      whitelist_;

  std::vector<optimization_guide::proto::Hint> initial_hints_;

  // Blacklist of host suffixes for LITE_PAGE_REDIRECT Previews.
  std::unique_ptr<HostFilter> lite_page_redirect_blacklist_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(PreviewsHints);
};

}  // namespace previews

#endif  // COMPONENTS_PREVIEWS_CONTENT_PREVIEWS_HINTS_H_
