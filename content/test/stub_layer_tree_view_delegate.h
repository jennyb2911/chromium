// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_TEST_STUB_LAYER_TREE_VIEW_DELEGATE_H_
#define CONTENT_TEST_STUB_LAYER_TREE_VIEW_DELEGATE_H_

#include "content/renderer/gpu/layer_tree_view_delegate.h"

namespace content {

class StubLayerTreeViewDelegate : public LayerTreeViewDelegate {
 public:
  // LayerTreeViewDelegate implementation.
  void ApplyViewportDeltas(const gfx::Vector2dF& inner_delta,
                           const gfx::Vector2dF& outer_delta,
                           const gfx::Vector2dF& elastic_overscroll_delta,
                           float page_scale,
                           float top_controls_delta) override {}
  void RecordWheelAndTouchScrollingCount(bool has_scrolled_by_wheel,
                                         bool has_scrolled_by_touch) override {}
  void BeginMainFrame(base::TimeTicks frame_time) override {}
  void RecordEndOfFrameMetrics(base::TimeTicks) override {}
  void RequestNewLayerTreeFrameSink(
      LayerTreeFrameSinkCallback callback) override;
  void DidCommitAndDrawCompositorFrame() override {}
  void DidCommitCompositorFrame() override {}
  void DidCompletePageScaleAnimation() override {}
  void DidReceiveCompositorFrameAck() override {}
  bool IsClosing() const override;
  void RequestScheduleAnimation() override {}
  void UpdateVisualState() override {}
  void WillBeginCompositorFrame() override {}
  std::unique_ptr<cc::SwapPromise> RequestCopyOfOutputForLayoutTest(
      std::unique_ptr<viz::CopyOutputRequest> request) override;
};

}  // namespace content

#endif  // CONTENT_TEST_STUB_LAYER_TREE_VIEW_DELEGATE_H_
