// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SUBRESOURCE_FILTER_CONTENT_RENDERER_SUBRESOURCE_FILTER_AGENT_H_
#define COMPONENTS_SUBRESOURCE_FILTER_CONTENT_RENDERER_SUBRESOURCE_FILTER_AGENT_H_

#include <memory>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "components/subresource_filter/content/renderer/ad_resource_tracker.h"
#include "components/subresource_filter/mojom/subresource_filter.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "url/gurl.h"

namespace blink {
class WebDocumentSubresourceFilter;
}  // namespace blink

namespace subresource_filter {

struct DocumentLoadStatistics;
class UnverifiedRulesetDealer;
class WebDocumentSubresourceFilterImpl;

// The renderer-side agent of ContentSubresourceFilterThrottleManager. There is
// one instance per RenderFrame, responsible for setting up the subresource
// filter for the ongoing provisional document load in the frame when instructed
// to do so by the manager.
class SubresourceFilterAgent
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<SubresourceFilterAgent>,
      public base::SupportsWeakPtr<SubresourceFilterAgent> {
 public:
  // The |ruleset_dealer| must not be null and must outlive this instance. The
  // |render_frame| may be null in unittests. The |ad_resource_tracker| may be
  // null.
  explicit SubresourceFilterAgent(
      content::RenderFrame* render_frame,
      UnverifiedRulesetDealer* ruleset_dealer,
      std::unique_ptr<AdResourceTracker> ad_resource_tracker);
  ~SubresourceFilterAgent() override;

 protected:
  // Below methods are protected virtual so they can be mocked out in tests.

  // Returns the URL of the currently committed document.
  virtual GURL GetDocumentURL();

  virtual bool IsMainFrame();

  // Injects the provided subresource |filter| into the DocumentLoader
  // orchestrating the most recently committed load.
  virtual void SetSubresourceFilterForCommittedLoad(
      std::unique_ptr<blink::WebDocumentSubresourceFilter> filter);

  // Informs the browser that the first subresource load has been disallowed for
  // the most recently committed load. Not called if all resources are allowed.
  virtual void SignalFirstSubresourceDisallowedForCommittedLoad();

  // Sends statistics about the DocumentSubresourceFilter's work to the browser.
  virtual void SendDocumentLoadStatistics(
      const DocumentLoadStatistics& statistics);

  // Tells the browser that the frame is an ad subframe.
  virtual void SendFrameIsAdSubframe();

  // True if the frame has been heuristically determined to be an ad subframe.
  virtual bool IsAdSubframe();
  virtual void SetIsAdSubframe();

 private:
  // Assumes that the parent will be in a local frame relative to this one, upon
  // construction.
  static mojom::ActivationState GetParentActivationState(
      content::RenderFrame* render_frame);

  void OnActivateForNextCommittedLoad(
      const mojom::ActivationState& activation_state,
      bool is_ad_subframe);
  void RecordHistogramsOnLoadCommitted(
      const mojom::ActivationState& activation_state);
  void RecordHistogramsOnLoadFinished();
  void ResetInfoForNextCommit();

  const mojom::SubresourceFilterHostAssociatedPtr& GetSubresourceFilterHost();

  // content::RenderFrameObserver:
  void OnDestruct() override;
  void DidCreateNewDocument() override;
  void DidCommitProvisionalLoad(bool is_same_document_navigation,
                                ui::PageTransition transition) override;
  void DidFailProvisionalLoad(const blink::WebURLError& error) override;
  void DidFinishLoad() override;
  bool OnMessageReceived(const IPC::Message& message) override;
  void WillCreateWorkerFetchContext(blink::WebWorkerFetchContext*) override;

  // Owned by the ChromeContentRendererClient and outlives us.
  UnverifiedRulesetDealer* ruleset_dealer_;

  mojom::ActivationState activation_state_for_next_commit_;

  // Tracks all ad resource observers.
  std::unique_ptr<AdResourceTracker> ad_resource_tracker_;

  // Use associated interface to make sure mojo messages are ordered with regard
  // to legacy IPC messages.
  mojom::SubresourceFilterHostAssociatedPtr subresource_filter_host_;

  // If a document has been created for this frame before. The first document
  // for a new local subframe should be about:blank.
  bool first_document_ = true;

  base::WeakPtr<WebDocumentSubresourceFilterImpl>
      filter_for_last_committed_load_;

  DISALLOW_COPY_AND_ASSIGN(SubresourceFilterAgent);
};

}  // namespace subresource_filter

#endif  // COMPONENTS_SUBRESOURCE_FILTER_CONTENT_RENDERER_SUBRESOURCE_FILTER_AGENT_H_
