/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009 Apple Inc. All rights
 * reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "third_party/blink/renderer/core/editing/layout_selection.h"

#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/editing/ephemeral_range.h"
#include "third_party/blink/renderer/core/editing/frame_selection.h"
#include "third_party/blink/renderer/core/editing/selection_template.h"
#include "third_party/blink/renderer/core/editing/visible_position.h"
#include "third_party/blink/renderer/core/editing/visible_units.h"
#include "third_party/blink/renderer/core/html/forms/text_control_element.h"
#include "third_party/blink/renderer/core/layout/layout_text.h"
#include "third_party/blink/renderer/core/layout/layout_text_fragment.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_offset_mapping.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_physical_line_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_physical_text_fragment.h"
#include "third_party/blink/renderer/core/paint/ng/ng_paint_fragment.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"

namespace blink {

// The current selection to be painted is represented as 2 pairs of
// (Node, offset).
// Each offset represents text offsets on selection edge if it is text.
// For example, suppose we select "f^oo<br><img>|",
// |start_offset_| is 1 and |end_offset_| is nullopt.
// If on NG, offset is on text content offset rather than each text node.
class SelectionPaintRange : public GarbageCollected<SelectionPaintRange> {
 public:
  SelectionPaintRange() = default;
  SelectionPaintRange(const Node& passed_start_node,
                      base::Optional<unsigned> passed_start_offset,
                      const Node& passed_end_node,
                      base::Optional<unsigned> passed_end_offset)
      : start_node(passed_start_node),
        start_offset(passed_start_offset),
        end_node(passed_end_node),
        end_offset(passed_end_offset) {}
  void Trace(Visitor* visitor) {
    visitor->Trace(start_node);
    visitor->Trace(end_node);
  }

  bool IsNull() const { return !start_node; }
  void AssertSanity() const {
#if DCHECK_IS_ON()
    if (start_node) {
      DCHECK(end_node);
      DCHECK(start_node->GetLayoutObject()->GetSelectionState() ==
                 SelectionState::kStart ||
             start_node->GetLayoutObject()->GetSelectionState() ==
                 SelectionState::kStartAndEnd);
      DCHECK(end_node->GetLayoutObject()->GetSelectionState() ==
                 SelectionState::kEnd ||
             end_node->GetLayoutObject()->GetSelectionState() ==
                 SelectionState::kStartAndEnd);
      return;
    }
    DCHECK(!end_node);
    DCHECK(!start_offset.has_value());
    DCHECK(!end_offset.has_value());
#endif
  }

  Member<const Node> start_node;
  base::Optional<unsigned> start_offset;
  Member<const Node> end_node;
  base::Optional<unsigned> end_offset;
};

LayoutSelection::LayoutSelection(FrameSelection& frame_selection)
    : frame_selection_(&frame_selection),
      has_pending_selection_(false),
      paint_range_(new SelectionPaintRange) {}

enum class SelectionMode {
  kNone,
  kRange,
  kBlockCursor,
};

static SelectionMode ComputeSelectionMode(
    const FrameSelection& frame_selection) {
  const SelectionInDOMTree& selection_in_dom =
      frame_selection.GetSelectionInDOMTree();
  if (selection_in_dom.IsRange())
    return SelectionMode::kRange;
  DCHECK(selection_in_dom.IsCaret());
  if (!frame_selection.ShouldShowBlockCursor())
    return SelectionMode::kNone;
  if (IsLogicalEndOfLine(CreateVisiblePosition(selection_in_dom.Base())))
    return SelectionMode::kNone;
  return SelectionMode::kBlockCursor;
}

static EphemeralRangeInFlatTree CalcSelectionInFlatTree(
    const FrameSelection& frame_selection) {
  const SelectionInDOMTree& selection_in_dom =
      frame_selection.GetSelectionInDOMTree();
  switch (ComputeSelectionMode(frame_selection)) {
    case SelectionMode::kNone:
      return {};
    case SelectionMode::kRange: {
      const PositionInFlatTree& base =
          ToPositionInFlatTree(selection_in_dom.Base());
      const PositionInFlatTree& extent =
          ToPositionInFlatTree(selection_in_dom.Extent());
      if (base.IsNull() || extent.IsNull() || base == extent ||
          !base.IsValidFor(frame_selection.GetDocument()) ||
          !extent.IsValidFor(frame_selection.GetDocument()))
        return {};
      return base <= extent ? EphemeralRangeInFlatTree(base, extent)
                            : EphemeralRangeInFlatTree(extent, base);
    }
    case SelectionMode::kBlockCursor: {
      const PositionInFlatTree& base =
          CreateVisiblePosition(ToPositionInFlatTree(selection_in_dom.Base()))
              .DeepEquivalent();
      if (base.IsNull())
        return {};
      const PositionInFlatTree end_position =
          NextPositionOf(base, PositionMoveType::kGraphemeCluster);
      if (end_position.IsNull())
        return {};
      return base <= end_position
                 ? EphemeralRangeInFlatTree(base, end_position)
                 : EphemeralRangeInFlatTree(end_position, base);
    }
  }
  NOTREACHED();
  return {};
}

// OldSelectedNodes is current selected Nodes with
// current SelectionState which is kStart, kEnd, kStartAndEnd or kInside.
struct OldSelectedNodes {
  STACK_ALLOCATED();

 public:
  OldSelectedNodes() : paint_range(new SelectionPaintRange) {}
  OldSelectedNodes(OldSelectedNodes&& other) {
    paint_range = other.paint_range;
    selected_map = std::move(other.selected_map);
  }

  Member<SelectionPaintRange> paint_range;
  HeapHashMap<Member<const Node>, SelectionState> selected_map;

 private:
  DISALLOW_COPY_AND_ASSIGN(OldSelectedNodes);
};

std::ostream& operator<<(std::ostream&, const OldSelectedNodes&);

// This struct represents a selection range in layout tree and each
// Node is SelectionState-marked.
struct NewPaintRangeAndSelectedNodes {
  STACK_ALLOCATED();

 public:
  NewPaintRangeAndSelectedNodes() : paint_range(new SelectionPaintRange) {}
  NewPaintRangeAndSelectedNodes(
      SelectionPaintRange* passed_paint_range,
      HeapHashSet<Member<const Node>>&& passed_selected_objects)
      : paint_range(passed_paint_range),
        selected_objects(std::move(passed_selected_objects)) {}
  NewPaintRangeAndSelectedNodes(NewPaintRangeAndSelectedNodes&& other) {
    paint_range = other.paint_range;
    selected_objects = std::move(other.selected_objects);
  }

  void AssertSanity() const {
#if DCHECK_IS_ON()
    paint_range->AssertSanity();
    if (paint_range->start_node) {
      DCHECK(selected_objects.Contains(paint_range->start_node)) << this;
      DCHECK(selected_objects.Contains(paint_range->end_node)) << this;
      return;
    }
    DCHECK(selected_objects.IsEmpty()) << this;
#endif
  }

  Member<SelectionPaintRange> paint_range;
  HeapHashSet<Member<const Node>> selected_objects;

 private:
  DISALLOW_COPY_AND_ASSIGN(NewPaintRangeAndSelectedNodes);
};

std::ostream& operator<<(std::ostream&, const NewPaintRangeAndSelectedNodes&);

static void SetShouldInvalidateIfNeeded(LayoutObject* layout_object) {
  if (layout_object->ShouldInvalidateSelection())
    return;
  layout_object->SetShouldInvalidateSelection();

  // We should invalidate if ancestor of |layout_object| is LayoutSVGText
  // because SVGRootInlineBoxPainter::Paint() paints selection for
  // |layout_object| in/ LayoutSVGText and it is invoked when parent
  // LayoutSVGText is invalidated.
  // That is different from InlineTextBoxPainter::Paint() which paints
  // LayoutText selection when LayoutText is invalidated.
  if (!layout_object->IsSVG())
    return;
  for (LayoutObject* parent = layout_object->Parent(); parent;
       parent = parent->Parent()) {
    if (parent->IsSVGRoot())
      return;
    if (parent->IsSVGText()) {
      if (!parent->ShouldInvalidateSelection())
        parent->SetShouldInvalidateSelection();
      return;
    }
  }
}

static LayoutTextFragment* FirstLetterPartFor(
    const LayoutObject* layout_object) {
  // TODO(yoichio): LayoutText::GetFirstLetterPart() should be typed
  // LayoutTextFragment.
  if (const LayoutText* layout_text = ToLayoutTextOrNull(layout_object))
    return ToLayoutTextFragment(layout_text->GetFirstLetterPart());
  return nullptr;
}

static void SetShouldInvalidateIfNeeded(const Node& node) {
  LayoutObject* layout_object = node.GetLayoutObject();
  if (!layout_object)
    return;
  if (LayoutTextFragment* first_letter = FirstLetterPartFor(layout_object))
    SetShouldInvalidateIfNeeded(first_letter);
  SetShouldInvalidateIfNeeded(layout_object);
}

static void SetSelectionStateIfNeeded(const Node& node, SelectionState state) {
  DCHECK_NE(state, SelectionState::kContain) << node;
  DCHECK_NE(state, SelectionState::kNone) << node;
  LayoutObject* layout_object = node.GetLayoutObject();
  if (layout_object->GetSelectionState() == state)
    return;
  layout_object->SetSelectionState(state);

  // Set ancestors SelectionState kContain for CSS ::selection style.
  // See LayoutObject::InvalidateSelectedChildrenOnStyleChange().
  for (Node& ancestor : FlatTreeTraversal::AncestorsOf(node)) {
    LayoutObject* ancestor_layout = ancestor.GetLayoutObject();
    if (!ancestor_layout)
      continue;
    if (ancestor_layout->GetSelectionState() == SelectionState::kContain)
      return;
    ancestor_layout->LayoutObject::SetSelectionState(SelectionState::kContain);
  }
}

// Set ShouldInvalidateSelection flag of LayoutObjects
// comparing them in |new_range| and |old_range|.
static void SetShouldInvalidateSelection(
    const NewPaintRangeAndSelectedNodes& new_range,
    const OldSelectedNodes& old_selected_objects) {
  // We invalidate each LayoutObject in new SelectionPaintRange which
  // has SelectionState of kStart, kEnd, kStartAndEnd, or kInside
  // and is not in old SelectionPaintRange.
  for (const Node* node : new_range.selected_objects) {
    if (old_selected_objects.selected_map.Contains(node))
      continue;
    const SelectionState new_state =
        node->GetLayoutObject()->GetSelectionState();
    DCHECK_NE(new_state, SelectionState::kContain) << node;
    DCHECK_NE(new_state, SelectionState::kNone) << node;
    SetShouldInvalidateIfNeeded(*node);
  }
  // For LayoutObject in old SelectionPaintRange, we invalidate LayoutObjects
  // each of:
  // 1. LayoutObject was painted and would not be painted.
  // 2. LayoutObject was not painted and would be painted.
  for (const auto& key_value : old_selected_objects.selected_map) {
    const Node* const node = key_value.key;
    const SelectionState old_state = key_value.value;
    const SelectionState new_state =
        node->GetLayoutObject()->GetSelectionState();
    if (new_state == old_state)
      continue;
    DCHECK(new_state != SelectionState::kNone ||
           old_state != SelectionState::kNone)
        << node;
    DCHECK_NE(new_state, SelectionState::kContain) << node;
    DCHECK_NE(old_state, SelectionState::kContain) << node;
    SetShouldInvalidateIfNeeded(*node);
  }

  // Invalidate Selection start/end is moving on a same node.
  const SelectionPaintRange& new_paint_range = *new_range.paint_range;
  const SelectionPaintRange& old_paint_range =
      *old_selected_objects.paint_range;
  if (new_paint_range.IsNull())
    return;
  if (new_paint_range.start_node->IsTextNode() &&
      new_paint_range.start_node == old_paint_range.start_node &&
      new_paint_range.start_offset != old_paint_range.start_offset)
    SetShouldInvalidateIfNeeded(*new_paint_range.start_node);
  if (new_paint_range.end_node->IsTextNode() &&
      new_paint_range.end_node == old_paint_range.end_node &&
      new_paint_range.end_offset != old_paint_range.end_offset)
    SetShouldInvalidateIfNeeded(*new_paint_range.end_node);
}

static bool IsDisplayContentElement(const Node& node) {
  if (!node.IsElementNode())
    return false;
  const ComputedStyle* const style = node.GetComputedStyle();
  return style && style->Display() == EDisplay::kContents;
}

template <typename Visitor>
static void VisitSelectedInclusiveDescendantsOfInternal(const Node& node,
                                                        Visitor* visitor) {
  // Display:content element appears in a flat tree even it doesn't have
  // a LayoutObject but we need to visit its children.
  if (!IsDisplayContentElement(node)) {
    LayoutObject* layout_object = node.GetLayoutObject();
    if (!layout_object)
      return;
    if (layout_object->GetSelectionState() == SelectionState::kNone)
      return;
    visitor->Visit(node);
  }

  for (Node& child : FlatTreeTraversal::ChildrenOf(node))
    VisitSelectedInclusiveDescendantsOfInternal(child, visitor);
}

static inline bool IsFlatTreeClean(const Node& node) {
  return !node.GetDocument().IsSlotAssignmentOrLegacyDistributionDirty();
}

template <typename Visitor>
static void VisitSelectedInclusiveDescendantsOf(const Node& node,
                                                Visitor* visitor) {
  DCHECK(IsFlatTreeClean(node));
  return VisitSelectedInclusiveDescendantsOfInternal(node, visitor);
}

static OldSelectedNodes ResetOldSelectedNodes(
    const Node& root,
    base::Optional<unsigned> old_start_offset,
    base::Optional<unsigned> old_end_offset) {
  class OldSelectedVisitor {
    STACK_ALLOCATED();

   public:
    OldSelectedVisitor(base::Optional<unsigned> passed_old_start_offset,
                       base::Optional<unsigned> passed_old_end_offset)
        : old_start_offset(passed_old_start_offset),
          old_end_offset(passed_old_end_offset) {}

    void Visit(const Node& node) {
      LayoutObject* layout_object = node.GetLayoutObject();
      const SelectionState old_state = layout_object->GetSelectionState();
      DCHECK_NE(old_state, SelectionState::kNone) << node;
      layout_object->SetSelectionState(SelectionState::kNone);
      if (old_state == SelectionState::kContain)
        return;
      old_selected_objects.selected_map.insert(&node, old_state);
      if (old_state == SelectionState::kInside)
        return;
      switch (old_state) {
        case SelectionState::kStart: {
          DCHECK(!old_selected_objects.paint_range->start_node);
          old_selected_objects.paint_range->start_node = node;
          old_selected_objects.paint_range->start_offset = old_start_offset;
          break;
        }
        case SelectionState::kEnd: {
          DCHECK(!old_selected_objects.paint_range->end_node);
          old_selected_objects.paint_range->end_node = node;
          old_selected_objects.paint_range->end_offset = old_end_offset;
          break;
        }
        case SelectionState::kStartAndEnd: {
          DCHECK(!old_selected_objects.paint_range->start_node);
          DCHECK(!old_selected_objects.paint_range->end_node);
          old_selected_objects.paint_range->start_node = node;
          old_selected_objects.paint_range->start_offset = old_start_offset;
          old_selected_objects.paint_range->end_node = node;
          old_selected_objects.paint_range->end_offset = old_end_offset;
          break;
        }
        default: {
          NOTREACHED();
          break;
        }
      }
    }

    OldSelectedNodes old_selected_objects;
    const base::Optional<unsigned> old_start_offset;
    const base::Optional<unsigned> old_end_offset;
  } visitor(old_start_offset, old_end_offset);
  VisitSelectedInclusiveDescendantsOf(root, &visitor);
  return std::move(visitor.old_selected_objects);
}

static base::Optional<unsigned> ComputeStartOffset(
    const Node& node,
    const PositionInFlatTree& selection_start) {
  if (!node.IsTextNode())
    return base::nullopt;

  if (&node == selection_start.AnchorNode())
    return selection_start.OffsetInContainerNode();
  return 0;
}

static base::Optional<unsigned> ComputeEndOffset(
    const Node& node,
    const PositionInFlatTree& selection_end) {
  if (!node.IsTextNode())
    return base::nullopt;

  if (&node == selection_end.AnchorNode())
    return selection_end.OffsetInContainerNode();
  return ToText(node).length();
}

#if DCHECK_IS_ON()
// Position should be offset on text or before/after a break element.
static bool IsPositionValidText(const Position& position) {
  if (position.AnchorNode()->IsTextNode() && position.IsOffsetInAnchor())
    return true;
  if ((IsHTMLBRElement(position.AnchorNode()) ||
       IsHTMLWBRElement(position.AnchorNode())) &&
      (position.IsBeforeAnchor() || position.IsAfterAnchor()))
    return true;
  return false;
}
#endif

static base::Optional<unsigned> GetTextContentOffset(const Position& position) {
  if (position.IsNull())
    return base::nullopt;
#if DCHECK_IS_ON()
  DCHECK(IsPositionValidText(position));
#endif
  DCHECK(position.AnchorNode()->GetLayoutObject()->EnclosingNGBlockFlow());
  const NGOffsetMapping* const offset_mapping =
      NGOffsetMapping::GetFor(position);
  DCHECK(offset_mapping);
  const base::Optional<unsigned>& ng_offset =
      offset_mapping->GetTextContentOffset(position);
  return ng_offset;
}

// Computes text content offset of selection start if |layout_object| is
// LayoutText.
static base::Optional<unsigned> GetTextContentOffsetStart(
    const Node& node,
    base::Optional<unsigned> node_offset) {
  if (!node.GetLayoutObject()->IsText())
    return base::nullopt;
  if (node.IsTextNode()) {
    DCHECK(node_offset.has_value()) << node;
    return GetTextContentOffset(Position(node, node_offset.value()));
  }

  DCHECK(IsHTMLWBRElement(node) || IsHTMLBRElement(node)) << node;
  DCHECK(!node_offset.has_value()) << node;
  return GetTextContentOffset(Position::BeforeNode(node));
}

// Computes text content offset of selection end if |layout_object| is
// LayoutText.
static base::Optional<unsigned> GetTextContentOffsetEnd(
    const Node& node,
    base::Optional<unsigned> node_offset) {
  if (!node.GetLayoutObject()->IsText())
    return {};
  if (node.IsTextNode()) {
    DCHECK(node_offset.has_value()) << node;
    return GetTextContentOffset(Position(node, node_offset.value()));
  }

  DCHECK(IsHTMLWBRElement(node) || IsHTMLBRElement(node)) << node;
  DCHECK(!node_offset.has_value()) << node;
  return GetTextContentOffset(Position::AfterNode(node));
}

static SelectionPaintRange* ComputeNewPaintRange(
    const SelectionPaintRange& paint_range) {
  DCHECK(!paint_range.IsNull());

  const Node& start_node = *paint_range.start_node;
  // If LayoutObject is not in NG, use legacy offset.
  const base::Optional<unsigned> start_offset =
      start_node.GetLayoutObject()->EnclosingNGBlockFlow()
          ? GetTextContentOffsetStart(start_node, paint_range.start_offset)
          : paint_range.start_offset;

  const Node& end_node = *paint_range.end_node;
  const base::Optional<unsigned> end_offset =
      end_node.GetLayoutObject()->EnclosingNGBlockFlow()
          ? GetTextContentOffsetEnd(end_node, paint_range.end_offset)
          : paint_range.end_offset;

  return new SelectionPaintRange(*paint_range.start_node, start_offset,
                                 *paint_range.end_node, end_offset);
}

// ClampOffset modifies |offset| fixed in a range of |text_fragment| start/end
// offsets.
static unsigned ClampOffset(unsigned offset,
                            const NGPhysicalTextFragment& text_fragment) {
  return std::min(std::max(offset, text_fragment.StartOffset()),
                  text_fragment.EndOffset());
}

// We don't paint a line break the end of inline-block
// because if an inline-block is at the middle of line, we should not paint
// a line break.
// Old layout paints line break if the inline-block is at the end of line, but
// since its complex to determine if the inline-block is at the end of line on NG,
// we just cancels block-end line break painting for any inline-block.
static bool IsLastLineInInlineBlock(const NGPaintFragment& line) {
  DCHECK(line.PhysicalFragment().IsLineBox());
  NGPaintFragment* parent = line.Parent();
  if (!parent->PhysicalFragment().IsAtomicInline())
    return false;
  return parent->Children().back().get() == &line;
}

static bool IsBeforeSoftLineBreak(const NGPaintFragment& fragment) {
  if (ToNGPhysicalTextFragmentOrDie(fragment.PhysicalFragment()).IsLineBreak())
    return false;

  // TODO(yoichio): InlineBlock should not be container line box.
  // See paint/selection/text-selection-inline-block.html.
  const NGPaintFragment* container_line_box = fragment.ContainerLineBox();
  DCHECK(container_line_box);
  if (IsLastLineInInlineBlock(*container_line_box))
    return false;
  const NGPhysicalLineBoxFragment& physical_line_box =
      ToNGPhysicalLineBoxFragment(container_line_box->PhysicalFragment());
  const NGPhysicalFragment* last_leaf = physical_line_box.LastLogicalLeaf();
  DCHECK(last_leaf);
  if (&fragment.PhysicalFragment() != last_leaf)
    return false;
  // Even If |fragment| is before linebreak, if its direction differs to line
  // direction, we don't paint line break. See
  // paint/selection/text-selection-newline-mixed-ltr-rtl.html.
  const ShapeResult* shape_result =
      ToNGPhysicalTextFragment(fragment.PhysicalFragment()).TextShapeResult();
  return physical_line_box.BaseDirection() == shape_result->Direction();
}

static Text* AssociatedTextNode(const LayoutText& text) {
  if (const LayoutTextFragment* fragment = ToLayoutTextFragmentOrNull(text))
    return fragment->AssociatedTextNode();
  if (Node* node = text.GetNode())
    return ToTextOrNull(node);
  return nullptr;
}

static SelectionState GetSelectionStateFor(const LayoutText& layout_text) {
  if (const LayoutTextFragment* text_fragment =
          ToLayoutTextFragmentOrNull(layout_text)) {
    Node* node = text_fragment->AssociatedTextNode();
    if (!node)
      return SelectionState::kNone;
    return node->GetLayoutObject()->GetSelectionState();
  }
  return layout_text.GetSelectionState();
}

bool LayoutSelection::IsSelected(const LayoutObject& layout_object) {
  if (const LayoutText* layout_text = ToLayoutTextOrNull(layout_object))
    return GetSelectionStateFor(*layout_text) != SelectionState::kNone;
  return layout_object.GetSelectionState() != SelectionState::kNone;
}

static inline unsigned ClampOffset(unsigned node_offset,
                                   const LayoutTextFragment& fragment) {
  if (fragment.Start() > node_offset)
    return 0;
  return std::min(node_offset - fragment.Start(), fragment.FragmentLength());
}

static LayoutTextSelectionStatus ComputeSelectionStatusForNode(
    const Text& text,
    SelectionState selection_state,
    base::Optional<unsigned> start_offset,
    base::Optional<unsigned> end_offset) {
  switch (selection_state) {
    case SelectionState::kInside:
      return {0, text.length(), SelectionIncludeEnd::kInclude};
    case SelectionState::kStart:
      return {start_offset.value(), text.length(),
              SelectionIncludeEnd::kInclude};
    case SelectionState::kEnd:
      return {0, end_offset.value(), SelectionIncludeEnd::kNotInclude};
    case SelectionState::kStartAndEnd:
      return {start_offset.value(), end_offset.value(),
              SelectionIncludeEnd::kNotInclude};
    default:
      NOTREACHED();
      return {0, 0, SelectionIncludeEnd::kNotInclude};
  }
}

LayoutTextSelectionStatus LayoutSelection::ComputeSelectionStatus(
    const LayoutText& layout_text) const {
  DCHECK(!HasPendingSelection());
  const SelectionState selection_state = GetSelectionStateFor(layout_text);
  if (selection_state == SelectionState::kNone)
    return {0, 0, SelectionIncludeEnd::kNotInclude};
  if (Text* text = AssociatedTextNode(layout_text)) {
    const LayoutTextSelectionStatus text_status = ComputeSelectionStatusForNode(
        *text, selection_state, paint_range_->start_offset,
        paint_range_->end_offset);
    if (const LayoutTextFragment* text_fragment =
            ToLayoutTextFragmentOrNull(layout_text)) {
      return {ClampOffset(text_status.start, *text_fragment),
              ClampOffset(text_status.end, *text_fragment),
              text_status.include_end};
    }
    return text_status;
  }
  // TODO(yoichio): This is really weird legacy behavior. Remove this.
  if (layout_text.IsBR() && selection_state == SelectionState::kEnd)
    return {0, 0, SelectionIncludeEnd::kNotInclude};
  return {0, layout_text.TextLength(), SelectionIncludeEnd::kInclude};
}

LayoutTextSelectionStatus FrameSelection::ComputeLayoutSelectionStatus(
    const LayoutText& text) const {
  return layout_selection_->ComputeSelectionStatus(text);
}

// FrameSelection holds selection offsets in layout block flow at
// LayoutSelection::Commit() if selection starts/ends within Text that
// each LayoutObject::SelectionState indicates.
// These offset can be out of |text_fragment| because SelectionState is of each
// LayoutText and not of each NGPhysicalTextFragment for it.
LayoutSelectionStatus LayoutSelection::ComputeSelectionStatus(
    const NGPaintFragment& fragment) const {
  const NGPhysicalTextFragment& text_fragment =
      ToNGPhysicalTextFragmentOrDie(fragment.PhysicalFragment());
  // We don't paint selection on ellipsis.
  if (text_fragment.StyleVariant() == NGStyleVariant::kEllipsis)
    return {0, 0, SelectSoftLineBreak::kNotSelected};
  // Needs GetSelectionStateFor
  DCHECK(text_fragment.GetLayoutObject());
  switch (
      GetSelectionStateFor(ToLayoutText(*text_fragment.GetLayoutObject()))) {
    case SelectionState::kStart: {
      const unsigned start_in_block = paint_range_->start_offset.value();
      const bool is_continuous = start_in_block <= text_fragment.EndOffset();
      return {ClampOffset(start_in_block, text_fragment),
              text_fragment.EndOffset(),
              (is_continuous && IsBeforeSoftLineBreak(fragment))
                  ? SelectSoftLineBreak::kSelected
                  : SelectSoftLineBreak::kNotSelected};
    }
    case SelectionState::kEnd: {
      const unsigned end_in_block = paint_range_->end_offset.value();
      const unsigned end_in_fragment = ClampOffset(end_in_block, text_fragment);
      const bool is_continuous = text_fragment.EndOffset() < end_in_block;
      return {text_fragment.StartOffset(), end_in_fragment,
              (is_continuous && IsBeforeSoftLineBreak(fragment))
                  ? SelectSoftLineBreak::kSelected
                  : SelectSoftLineBreak::kNotSelected};
    }
    case SelectionState::kStartAndEnd: {
      const unsigned start_in_block = paint_range_->start_offset.value();
      const unsigned end_in_block = paint_range_->end_offset.value();
      const unsigned end_in_fragment = ClampOffset(end_in_block, text_fragment);
      const bool is_continuous = start_in_block <= text_fragment.EndOffset() &&
                                 text_fragment.EndOffset() < end_in_block;
      return {ClampOffset(start_in_block, text_fragment), end_in_fragment,
              (is_continuous && IsBeforeSoftLineBreak(fragment))
                  ? SelectSoftLineBreak::kSelected
                  : SelectSoftLineBreak::kNotSelected};
    }
    case SelectionState::kInside: {
      return {text_fragment.StartOffset(), text_fragment.EndOffset(),
              IsBeforeSoftLineBreak(fragment)
                  ? SelectSoftLineBreak::kSelected
                  : SelectSoftLineBreak::kNotSelected};
    }
    default:
      // This block is not included in selection.
      return {0, 0, SelectSoftLineBreak::kNotSelected};
  }
}

static NewPaintRangeAndSelectedNodes CalcSelectionRangeAndSetSelectionState(
    const FrameSelection& frame_selection) {
  const SelectionInDOMTree& selection_in_dom =
      frame_selection.GetSelectionInDOMTree();
  if (selection_in_dom.IsNone())
    return {};

  const EphemeralRangeInFlatTree& selection =
      CalcSelectionInFlatTree(frame_selection);
  if (selection.IsCollapsed() || frame_selection.IsHidden())
    return {};

  // Find first/last Node which has a visible LayoutObject while
  // marking SelectionState and collecting invalidation candidate LayoutObjects.
  const Node* start_node = nullptr;
  const Node* end_node = nullptr;
  HeapHashSet<Member<const Node>> selected_objects;
  for (Node& node : selection.Nodes()) {
    LayoutObject* const layout_object = node.GetLayoutObject();
    if (!layout_object || !layout_object->CanBeSelectionLeaf())
      continue;

    if (!start_node) {
      DCHECK(!end_node);
      start_node = end_node = &node;
      continue;
    }

    // In this loop, |end_node| is pointing current last candidate
    // LayoutObject and if it is not start and we find next, we mark the
    // current one as kInside.
    if (end_node != start_node) {
      SetSelectionStateIfNeeded(*end_node, SelectionState::kInside);
      selected_objects.insert(end_node);
    }
    end_node = &node;
  }

  // No valid LayOutObject found.
  if (!start_node) {
    DCHECK(!end_node);
    return {};
  }

  // Compute offset. It has value iff start/end is text.
  const base::Optional<unsigned> start_offset = ComputeStartOffset(
      *start_node, selection.StartPosition().ToOffsetInAnchor());
  const base::Optional<unsigned> end_offset =
      ComputeEndOffset(*end_node, selection.EndPosition().ToOffsetInAnchor());
  if (start_node == end_node) {
    SetSelectionStateIfNeeded(*start_node, SelectionState::kStartAndEnd);
    selected_objects.insert(start_node);
  } else {
    SetSelectionStateIfNeeded(*start_node, SelectionState::kStart);
    selected_objects.insert(start_node);
    SetSelectionStateIfNeeded(*end_node, SelectionState::kEnd);
    selected_objects.insert(end_node);
  }

  SelectionPaintRange* new_range =
      new SelectionPaintRange(*start_node, start_offset, *end_node, end_offset);
  if (!RuntimeEnabledFeatures::LayoutNGEnabled())
    return {new_range, std::move(selected_objects)};
  return {ComputeNewPaintRange(*new_range), std::move(selected_objects)};
}

void LayoutSelection::SetHasPendingSelection() {
  has_pending_selection_ = true;
}

void LayoutSelection::Commit() {
  if (!HasPendingSelection())
    return;
  has_pending_selection_ = false;

  DCHECK(!frame_selection_->GetDocument().NeedsLayoutTreeUpdate());
  DCHECK_GE(frame_selection_->GetDocument().Lifecycle().GetState(),
            DocumentLifecycle::kLayoutClean);
  DocumentLifecycle::DisallowTransitionScope disallow_transition(
      frame_selection_->GetDocument().Lifecycle());

  const OldSelectedNodes& old_selected_objects = ResetOldSelectedNodes(
      frame_selection_->GetDocument(), paint_range_->start_offset,
      paint_range_->end_offset);
  const NewPaintRangeAndSelectedNodes& new_range =
      CalcSelectionRangeAndSetSelectionState(*frame_selection_);
  new_range.AssertSanity();
  DCHECK(frame_selection_->GetDocument().GetLayoutView()->GetFrameView());
  SetShouldInvalidateSelection(new_range, old_selected_objects);

  paint_range_ = new_range.paint_range;
}

void LayoutSelection::OnDocumentShutdown() {
  has_pending_selection_ = false;
  paint_range_->start_node = nullptr;
  paint_range_->start_offset = base::nullopt;
  paint_range_->end_node = nullptr;
  paint_range_->end_offset = base::nullopt;
}

static LayoutRect SelectionRectForLayoutObject(const LayoutObject* object) {
  if (!object->IsRooted())
    return LayoutRect();

  if (!object->CanUpdateSelectionOnRootLineBoxes())
    return LayoutRect();

  return object->AbsoluteSelectionRect();
}

template <typename Visitor>
static void VisitLayoutObjectsOf(const Node& node, Visitor* visitor) {
  LayoutObject* layout_object = node.GetLayoutObject();
  if (!layout_object)
    return;
  if (layout_object->GetSelectionState() == SelectionState::kContain)
    return;
  if (LayoutTextFragment* first_letter = FirstLetterPartFor(layout_object))
    visitor->Visit(first_letter);
  visitor->Visit(layout_object);
}

IntRect LayoutSelection::AbsoluteSelectionBounds() {
  Commit();
  if (paint_range_->IsNull())
    return IntRect();

  // Create a single bounding box rect that encloses the whole selection.
  class SelectionBoundsVisitor {
    STACK_ALLOCATED();

   public:
    void Visit(const Node& node) { VisitLayoutObjectsOf(node, this); }
    void Visit(LayoutObject* layout_object) {
      selected_rect.Unite(SelectionRectForLayoutObject(layout_object));
    }
    LayoutRect selected_rect;
  } visitor;
  VisitSelectedInclusiveDescendantsOf(frame_selection_->GetDocument(),
                                      &visitor);
  return PixelSnappedIntRect(visitor.selected_rect);
}

void LayoutSelection::InvalidatePaintForSelection() {
  if (paint_range_->IsNull())
    return;

  class InvalidatingVisitor {
    STACK_ALLOCATED();

   public:
    void Visit(const Node& node) { VisitLayoutObjectsOf(node, this); }
    void Visit(LayoutObject* layout_object) {
      layout_object->SetShouldInvalidateSelection();
    }
  } visitor;
  VisitSelectedInclusiveDescendantsOf(frame_selection_->GetDocument(),
                                      &visitor);
}

void LayoutSelection::Trace(blink::Visitor* visitor) {
  visitor->Trace(frame_selection_);
  visitor->Trace(paint_range_);
}

void PrintSelectionStatus(std::ostream& ostream, const Node& node) {
  ostream << (void*)&node;
  if (node.IsTextNode())
    ostream << "#text";
  else if (const Element* element = ToElementOrNull(node))
    ostream << element->tagName().Utf8().data();
  LayoutObject* layout_object = node.GetLayoutObject();
  if (!layout_object) {
    ostream << " <null LayoutObject>";
    return;
  }
  ostream << ' ' << layout_object->GetSelectionState();
}

#if DCHECK_IS_ON()
std::ostream& operator<<(std::ostream& ostream,
                         const base::Optional<unsigned>& offset) {
  if (offset.has_value())
    ostream << offset.value();
  else
    ostream << "<nullopt>";
  return ostream;
}

std::ostream& operator<<(std::ostream& ostream,
                         const SelectionPaintRange& range) {
  ostream << range.start_node << ": " << range.start_offset << ", "
          << range.end_node << ": " << range.end_offset;
  return ostream;
}

std::ostream& operator<<(
    std::ostream& ostream,
    const HeapHashMap<Member<const Node>, SelectionState>& map) {
  ostream << "[";
  const char* comma = "";
  for (const auto& key_value : map) {
    const Node* const node = key_value.key;
    const SelectionState old_state = key_value.value;
    ostream << comma << node << "." << old_state;
    comma = ", ";
  }
  ostream << "]";
  return ostream;
}

std::ostream& operator<<(std::ostream& ostream,
                         const OldSelectedNodes& old_node) {
  ostream << old_node.paint_range << ". " << old_node.selected_map;
  return ostream;
}

void PrintOldSelectedNodes(const OldSelectedNodes& old_node) {
  std::stringstream stream;
  stream << std::endl << old_node;
  LOG(INFO) << stream.str();
}

std::ostream& operator<<(
    std::ostream& ostream,
    const HeapHashSet<Member<const Node>>& selected_objects) {
  ostream << "[";
  const char* comma = "";
  for (const Node* node : selected_objects) {
    ostream << comma;
    PrintSelectionStatus(ostream, *node);
    comma = ", ";
  }
  ostream << "]";
  return ostream;
}

std::ostream& operator<<(std::ostream& ostream,
                         const NewPaintRangeAndSelectedNodes& new_range) {
  ostream << new_range.paint_range << ". " << new_range.selected_objects;
  return ostream;
}

void PrintSelectedNodes(const NewPaintRangeAndSelectedNodes& new_range) {
  std::stringstream stream;
  stream << std::endl << new_range;
  LOG(INFO) << stream.str();
}

void PrintSelectionStateInDocument(const FrameSelection& selection) {
  class PrintVisitor {
    STACK_ALLOCATED();

   public:
    void Visit(const Node& node) { PrintSelectionStatus(stream, node); }
    std::stringstream stream;
  } visitor;
  VisitSelectedInclusiveDescendantsOf(selection.GetDocument(), &visitor);
  LOG(INFO) << std::endl << visitor.stream.str();
}
#endif

}  // namespace blink
