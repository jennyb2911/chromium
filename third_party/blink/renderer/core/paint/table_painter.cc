// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/paint/table_painter.h"

#include "third_party/blink/renderer/core/layout/collapsed_border_value.h"
#include "third_party/blink/renderer/core/layout/layout_table.h"
#include "third_party/blink/renderer/core/layout/layout_table_section.h"
#include "third_party/blink/renderer/core/paint/box_painter.h"
#include "third_party/blink/renderer/core/paint/object_painter.h"
#include "third_party/blink/renderer/core/paint/paint_info.h"
#include "third_party/blink/renderer/core/paint/table_section_painter.h"
#include "third_party/blink/renderer/platform/graphics/paint/drawing_recorder.h"

namespace blink {

void TablePainter::PaintObject(const PaintInfo& paint_info,
                               const LayoutPoint& paint_offset) {
  PaintPhase paint_phase = paint_info.phase;

  if (ShouldPaintSelfBlockBackground(paint_phase)) {
    PaintBoxDecorationBackground(paint_info, paint_offset);
    if (paint_phase == PaintPhase::kSelfBlockBackgroundOnly)
      return;
  }

  if (paint_phase == PaintPhase::kMask) {
    PaintMask(paint_info, paint_offset);
    return;
  }

  if (paint_phase != PaintPhase::kSelfOutlineOnly) {
    PaintInfo paint_info_for_descendants = paint_info.ForDescendants();

    for (LayoutObject* child = layout_table_.FirstChild(); child;
         child = child->NextSibling()) {
      if (child->IsBox() && !ToLayoutBox(child)->HasSelfPaintingLayer() &&
          (child->IsTableSection() || child->IsTableCaption())) {
        child->Paint(paint_info_for_descendants);
      }
    }

    if (layout_table_.HasCollapsedBorders() &&
        ShouldPaintDescendantBlockBackgrounds(paint_phase) &&
        layout_table_.StyleRef().Visibility() == EVisibility::kVisible) {
      PaintCollapsedBorders(paint_info_for_descendants);
    }
  }

  if (ShouldPaintSelfOutline(paint_phase))
    ObjectPainter(layout_table_).PaintOutline(paint_info, paint_offset);
}

void TablePainter::RecordHitTestData(const PaintInfo& paint_info,
                                     const LayoutPoint& paint_offset) {
  // Hit test display items are only needed for compositing. This flag is used
  // for for printing and drag images which do not need hit testing.
  if (paint_info.GetGlobalPaintFlags() & kGlobalPaintFlattenCompositingLayers)
    return;

  auto touch_action = layout_table_.EffectiveWhitelistedTouchAction();
  if (touch_action == TouchAction::kTouchActionAuto)
    return;

  auto rect = layout_table_.BorderBoxRect();
  rect.MoveBy(paint_offset);
  HitTestData::RecordTouchActionRect(paint_info.context, layout_table_,
                                     TouchActionRect(rect, touch_action));
}

void TablePainter::PaintBoxDecorationBackground(
    const PaintInfo& paint_info,
    const LayoutPoint& paint_offset) {
  if (RuntimeEnabledFeatures::PaintTouchActionRectsEnabled())
    RecordHitTestData(paint_info, paint_offset);

  if (!layout_table_.HasBoxDecorationBackground() ||
      layout_table_.StyleRef().Visibility() != EVisibility::kVisible)
    return;

  LayoutRect rect(paint_offset, layout_table_.Size());
  layout_table_.SubtractCaptionRect(rect);
  BoxPainter(layout_table_)
      .PaintBoxDecorationBackgroundWithRect(paint_info, rect);
}

void TablePainter::PaintMask(const PaintInfo& paint_info,
                             const LayoutPoint& paint_offset) {
  if (layout_table_.StyleRef().Visibility() != EVisibility::kVisible ||
      paint_info.phase != PaintPhase::kMask)
    return;

  if (DrawingRecorder::UseCachedDrawingIfPossible(
          paint_info.context, layout_table_, paint_info.phase))
    return;

  LayoutRect rect(paint_offset, layout_table_.Size());
  layout_table_.SubtractCaptionRect(rect);

  DrawingRecorder recorder(paint_info.context, layout_table_, paint_info.phase);
  BoxPainter(layout_table_).PaintMaskImages(paint_info, rect);
}

void TablePainter::PaintCollapsedBorders(const PaintInfo& paint_info) {
  base::Optional<DrawingRecorder> recorder;
  if (UNLIKELY(layout_table_.ShouldPaintAllCollapsedBorders())) {
    if (DrawingRecorder::UseCachedDrawingIfPossible(
            paint_info.context, layout_table_,
            DisplayItem::kTableCollapsedBorders))
      return;
    recorder.emplace(paint_info.context, layout_table_,
                     DisplayItem::kTableCollapsedBorders);
  }
  // Otherwise each rows will create its own recorder.

  for (LayoutTableSection* section = layout_table_.BottomSection(); section;
       section = layout_table_.SectionAbove(section)) {
    TableSectionPainter(*section).PaintCollapsedBorders(paint_info);
  }
}

}  // namespace blink
