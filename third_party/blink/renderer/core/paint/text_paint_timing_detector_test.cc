// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/paint/paint_tracker.h"
#include "third_party/blink/renderer/core/testing/core_unit_test_helper.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

namespace blink {

class TextPaintTimingDetectorTest : public RenderingTest {
 public:
  void SetUp() override {
    RenderingTest::SetUp();
    RuntimeEnabledFeatures::SetPaintTrackingEnabled(true);
  }

 protected:
  LocalFrameView& GetFrameView() { return *GetFrame().View(); }
  PaintTracker& GetPaintTracker() { return GetFrameView().GetPaintTracker(); }

  void UpdateAllLifecyclePhasesAndSimulateSwapTime() {
    GetFrameView().UpdateAllLifecyclePhases();
    TextPaintTimingDetector& detector =
        GetPaintTracker().GetTextPaintTimingDetector();
    if (detector.texts_to_record_swap_time_.size() > 0) {
      detector.ReportSwapTime(WebLayerTreeView::SwapResult::kDidSwap,
                              CurrentTimeTicks());
    }
  }
};

TEST_F(TextPaintTimingDetectorTest, LargestTextPaint_NoText) {
  SetBodyInnerHTML(R"HTML(
    <div></div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record = GetPaintTracker()
                           .GetTextPaintTimingDetector()
                           .FindLargestPaintCandidate();
  EXPECT_FALSE(record);
}

TEST_F(TextPaintTimingDetectorTest, LargestTextPaint_OneText) {
  SetBodyInnerHTML(R"HTML(
    <div>The only text</div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record = GetPaintTracker()
                           .GetTextPaintTimingDetector()
                           .FindLargestPaintCandidate();
  EXPECT_TRUE(record);
  EXPECT_EQ(record->text, "The only text");
}

TEST_F(TextPaintTimingDetectorTest, LargestTextPaint_LargestText) {
  SetBodyInnerHTML(R"HTML(
    <div>medium text</div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  Text* larger_text = GetDocument().createTextNode("a long-long-long text");
  GetDocument().body()->AppendChild(larger_text);
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  Text* tiny_text = GetDocument().createTextNode("small");
  GetDocument().body()->AppendChild(tiny_text);
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  TextRecord* record = GetPaintTracker()
                           .GetTextPaintTimingDetector()
                           .FindLargestPaintCandidate();
  EXPECT_EQ(record->text, "a long-long-long text");
}

TEST_F(TextPaintTimingDetectorTest, LargestTextPaint_ReportFirstPaintTime) {
  TimeTicks time1 = CurrentTimeTicks();
  SetBodyInnerHTML(R"HTML(
    <div>
      <div id='b'>size-changing block</div>
      <div>a long-long-long-long moving text</div>
    </div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  TimeTicks time2 = CurrentTimeTicks();
  GetDocument().getElementById("b")->setAttribute(HTMLNames::styleAttr,
                                                  AtomicString("height:50px"));
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  GetDocument().getElementById("b")->setAttribute(HTMLNames::styleAttr,
                                                  AtomicString("height:100px"));
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  TextRecord* record = GetPaintTracker()
                           .GetTextPaintTimingDetector()
                           .FindLargestPaintCandidate();
  EXPECT_EQ(record->text, "a long-long-long-long moving text");
  TimeTicks firing_time = record->first_paint_time;
  EXPECT_GE(firing_time, time1);
  EXPECT_GE(time2, firing_time);
}

TEST_F(TextPaintTimingDetectorTest,
       LargestTextPaint_IgnoreTextOutsideViewport) {
  SetBodyInnerHTML(R"HTML(
    <style>
      div.out {
        position: fixed;
        top: -100px;
      }
    </style>
    <div class='out'>text outside of viewport</div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  EXPECT_FALSE(GetPaintTracker()
                   .GetTextPaintTimingDetector()
                   .FindLargestPaintCandidate());
  EXPECT_FALSE(
      GetPaintTracker().GetTextPaintTimingDetector().FindLastPaintCandidate());
}

TEST_F(TextPaintTimingDetectorTest, LargestTextPaint_IgnoreRemovedText) {
  SetBodyInnerHTML(R"HTML(
    <div id='parent'>
      <div id='earlyLargeText'>(large text)(large text)(large text)(large text)(large text)(large text)</div>
      <div>small text</div>
    </div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  GetDocument().getElementById("parent")->RemoveChild(
      GetDocument().getElementById("earlyLargeText"));
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record = GetPaintTracker()
                           .GetTextPaintTimingDetector()
                           .FindLargestPaintCandidate();
  EXPECT_EQ(record->text, "small text");
}

TEST_F(TextPaintTimingDetectorTest,
       LargestTextPaint_CompareVisualSizeNotActualSize) {
  SetBodyInnerHTML(R"HTML(
    <div>
      <div>short</div>
      <div style="position:fixed;left:-10px">a long text</div>
    </div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record = GetPaintTracker()
                           .GetTextPaintTimingDetector()
                           .FindLargestPaintCandidate();
  EXPECT_EQ(record->text, "short");
}

TEST_F(TextPaintTimingDetectorTest, LargestTextPaint_CompareSizesAtFirstPaint) {
  SetBodyInnerHTML(R"HTML(
    <div>
      <div id="shorteningText">large-to-small text</div>
      <div>a medium text</div>
    </div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  // The visual size becomes smaller when less portion intersecting with
  // viewport.
  GetDocument()
      .getElementById("shorteningText")
      ->setAttribute(HTMLNames::styleAttr,
                     AtomicString("position:fixed;left:-10px"));
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record = GetPaintTracker()
                           .GetTextPaintTimingDetector()
                           .FindLargestPaintCandidate();
  EXPECT_EQ(record->text, "large-to-small text");
}

TEST_F(TextPaintTimingDetectorTest, LastTextPaint_NoText) {
  SetBodyInnerHTML(R"HTML(
    <div></div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record =
      GetPaintTracker().GetTextPaintTimingDetector().FindLastPaintCandidate();
  EXPECT_FALSE(record);
}

TEST_F(TextPaintTimingDetectorTest, LastTextPaint_OneText) {
  SetBodyInnerHTML(R"HTML(
    <div>The only text</div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record =
      GetPaintTracker().GetTextPaintTimingDetector().FindLastPaintCandidate();
  EXPECT_EQ(record->text, "The only text");
}

TEST_F(TextPaintTimingDetectorTest, LastTextPaint_LastText) {
  SetBodyInnerHTML(R"HTML(
    <div>1st text</div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  Text* larger_text = GetDocument().createTextNode("2nd text");
  GetDocument().body()->AppendChild(larger_text);
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  Text* tiny_text = GetDocument().createTextNode("3rd text");
  GetDocument().body()->AppendChild(tiny_text);
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  TextRecord* record =
      GetPaintTracker().GetTextPaintTimingDetector().FindLastPaintCandidate();
  EXPECT_EQ(record->text, "3rd text");
}

TEST_F(TextPaintTimingDetectorTest, LastTextPaint_ReportFirstPaintTime) {
  SetBodyInnerHTML(R"HTML(
    <div>
      <div id='b'>size-changing block</div>
    </div>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  TimeTicks time1 = CurrentTimeTicks();
  Text* tiny_text = GetDocument().createTextNode("latest text");
  GetDocument().body()->AppendChild(tiny_text);
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  TimeTicks time2 = CurrentTimeTicks();
  GetDocument().getElementById("b")->setAttribute(HTMLNames::styleAttr,
                                                  AtomicString("height:50px"));
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  GetDocument().getElementById("b")->setAttribute(HTMLNames::styleAttr,
                                                  AtomicString("height:100px"));
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  TextRecord* record =
      GetPaintTracker().GetTextPaintTimingDetector().FindLastPaintCandidate();
  EXPECT_EQ(record->text, "latest text");
  TimeTicks firing_time = record->first_paint_time;
  EXPECT_GE(firing_time, time1);
  EXPECT_GE(time2, firing_time);
}

TEST_F(TextPaintTimingDetectorTest, LastTextPaint_IgnoreRemovedText) {
  SetBodyInnerHTML(R"HTML(
    <body>
      <div>earliest text</div>
    </body>
  )HTML");
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  Text* tiny_text = GetDocument().createTextNode("latest text");
  GetDocument().body()->AppendChild(tiny_text);
  UpdateAllLifecyclePhasesAndSimulateSwapTime();

  GetDocument().body()->RemoveChild(GetDocument().body()->lastChild());
  UpdateAllLifecyclePhasesAndSimulateSwapTime();
  TextRecord* record =
      GetPaintTracker().GetTextPaintTimingDetector().FindLastPaintCandidate();
  EXPECT_EQ(record->text, "earliest text");
}

}  // namespace blink
