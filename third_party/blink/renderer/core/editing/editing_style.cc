/*
 * Copyright (C) 2007, 2008, 2009 Apple Computer, Inc.
 * Copyright (C) 2010, 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/editing/editing_style.h"

#include "third_party/blink/renderer/core/css/css_color_value.h"
#include "third_party/blink/renderer/core/css/css_computed_style_declaration.h"
#include "third_party/blink/renderer/core/css/css_identifier_value.h"
#include "third_party/blink/renderer/core/css/css_primitive_value.h"
#include "third_party/blink/renderer/core/css/css_primitive_value_mappings.h"
#include "third_party/blink/renderer/core/css/css_property_value_set.h"
#include "third_party/blink/renderer/core/css/css_rule_list.h"
#include "third_party/blink/renderer/core/css/css_style_rule.h"
#include "third_party/blink/renderer/core/css/css_value_list.h"
#include "third_party/blink/renderer/core/css/font_size_functions.h"
#include "third_party/blink/renderer/core/css/parser/css_parser.h"
#include "third_party/blink/renderer/core/css/properties/css_property.h"
#include "third_party/blink/renderer/core/css/resolver/style_resolver.h"
#include "third_party/blink/renderer/core/css/style_rule.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/node.h"
#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/dom/node_traversal.h"
#include "third_party/blink/renderer/core/dom/qualified_name.h"
#include "third_party/blink/renderer/core/editing/commands/apply_style_command.h"
#include "third_party/blink/renderer/core/editing/editing_style_utilities.h"
#include "third_party/blink/renderer/core/editing/editing_tri_state.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/editing/editor.h"
#include "third_party/blink/renderer/core/editing/frame_selection.h"
#include "third_party/blink/renderer/core/editing/position.h"
#include "third_party/blink/renderer/core/editing/serializers/html_interchange.h"
#include "third_party/blink/renderer/core/editing/visible_selection.h"
#include "third_party/blink/renderer/core/editing/writing_direction.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_font_element.h"
#include "third_party/blink/renderer/core/html/html_span_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"

namespace blink {

using namespace cssvalue;

// Editing style properties must be preserved during editing operation.
// e.g. when a user inserts a new paragraph, all properties listed here must be
// copied to the new paragraph.
// NOTE: Use either allEditingProperties() or inheritableEditingProperties() to
// respect runtime enabling of properties.
static const CSSPropertyID kStaticEditingProperties[] = {
    CSSPropertyBackgroundColor, CSSPropertyColor, CSSPropertyFontFamily,
    CSSPropertyFontSize, CSSPropertyFontStyle, CSSPropertyFontVariantLigatures,
    CSSPropertyFontVariantCaps, CSSPropertyFontWeight, CSSPropertyLetterSpacing,
    CSSPropertyOrphans, CSSPropertyTextAlign,
    // FIXME: CSSPropertyTextDecoration needs to be removed when CSS3 Text
    // Decoration feature is no longer experimental.
    CSSPropertyTextDecoration, CSSPropertyTextDecorationLine,
    CSSPropertyTextIndent, CSSPropertyTextTransform, CSSPropertyWhiteSpace,
    CSSPropertyWidows, CSSPropertyWordSpacing,
    CSSPropertyWebkitTextDecorationsInEffect, CSSPropertyWebkitTextFillColor,
    CSSPropertyWebkitTextStrokeColor, CSSPropertyWebkitTextStrokeWidth,
    CSSPropertyCaretColor};

enum EditingPropertiesType {
  kOnlyInheritableEditingProperties,
  kAllEditingProperties
};

static const Vector<const CSSProperty*>& AllEditingProperties() {
  DEFINE_STATIC_LOCAL(Vector<const CSSProperty*>, properties, ());
  if (properties.IsEmpty()) {
    CSSProperty::FilterEnabledCSSPropertiesIntoVector(
        kStaticEditingProperties, arraysize(kStaticEditingProperties),
        properties);
    for (wtf_size_t index = 0; index < properties.size(); index++) {
      if (properties[index]->IDEquals(CSSPropertyTextDecoration)) {
        properties.EraseAt(index);
        break;
      }
    }
  }
  return properties;
}

static const Vector<const CSSProperty*>& InheritableEditingProperties() {
  DEFINE_STATIC_LOCAL(Vector<const CSSProperty*>, properties, ());
  if (properties.IsEmpty()) {
    CSSProperty::FilterEnabledCSSPropertiesIntoVector(
        kStaticEditingProperties, arraysize(kStaticEditingProperties),
        properties);
    for (wtf_size_t index = 0; index < properties.size();) {
      if (!properties[index]->IsInherited()) {
        properties.EraseAt(index);
        continue;
      }
      ++index;
    }
  }
  return properties;
}

template <class StyleDeclarationType>
static MutableCSSPropertyValueSet* CopyEditingProperties(
    StyleDeclarationType* style,
    EditingPropertiesType type = kOnlyInheritableEditingProperties) {
  if (type == kAllEditingProperties)
    return style->CopyPropertiesInSet(AllEditingProperties());
  return style->CopyPropertiesInSet(InheritableEditingProperties());
}

static inline bool IsEditingProperty(CSSPropertyID id) {
  static const Vector<const CSSProperty*>& properties = AllEditingProperties();
  for (wtf_size_t index = 0; index < properties.size(); index++) {
    if (properties[index]->IDEquals(id))
      return true;
  }
  return false;
}

static CSSComputedStyleDeclaration* EnsureComputedStyle(
    const Position& position) {
  Element* elem = AssociatedElementOf(position);
  if (!elem)
    return nullptr;
  return CSSComputedStyleDeclaration::Create(elem);
}

static MutableCSSPropertyValueSet* GetPropertiesNotIn(
    CSSPropertyValueSet* style_with_redundant_properties,
    CSSStyleDeclaration* base_style,
    SecureContextMode);
enum LegacyFontSizeMode {
  kAlwaysUseLegacyFontSize,
  kUseLegacyFontSizeOnlyIfPixelValuesMatch
};
static int LegacyFontSizeFromCSSValue(Document*,
                                      const CSSValue*,
                                      bool,
                                      LegacyFontSizeMode);

class HTMLElementEquivalent : public GarbageCollected<HTMLElementEquivalent> {
 public:
  static HTMLElementEquivalent* Create(CSSPropertyID property_id,
                                       CSSValueID primitive_value,
                                       const HTMLQualifiedName& tag_name) {
    return new HTMLElementEquivalent(property_id, primitive_value, tag_name);
  }

  virtual bool Matches(const Element* element) const {
    return !tag_name_ || element->HasTagName(*tag_name_);
  }
  virtual bool HasAttribute() const { return false; }
  virtual bool PropertyExistsInStyle(const CSSPropertyValueSet* style) const {
    return style->GetPropertyCSSValue(property_id_);
  }
  virtual bool ValueIsPresentInStyle(HTMLElement*, CSSPropertyValueSet*) const;
  virtual void AddToStyle(Element*, EditingStyle*) const;

  virtual void Trace(blink::Visitor* visitor) {
    visitor->Trace(identifier_value_);
  }

 protected:
  HTMLElementEquivalent(CSSPropertyID);
  HTMLElementEquivalent(CSSPropertyID, const HTMLQualifiedName& tag_name);
  HTMLElementEquivalent(CSSPropertyID,
                        CSSValueID primitive_value,
                        const HTMLQualifiedName& tag_name);
  const CSSPropertyID property_id_;
  const Member<CSSIdentifierValue> identifier_value_;
  // We can store a pointer because HTML tag names are const global.
  const HTMLQualifiedName* tag_name_;
};

HTMLElementEquivalent::HTMLElementEquivalent(CSSPropertyID id)
    : property_id_(id), tag_name_(nullptr) {}

HTMLElementEquivalent::HTMLElementEquivalent(CSSPropertyID id,
                                             const HTMLQualifiedName& tag_name)
    : property_id_(id), tag_name_(&tag_name) {}

HTMLElementEquivalent::HTMLElementEquivalent(CSSPropertyID id,
                                             CSSValueID value_id,
                                             const HTMLQualifiedName& tag_name)
    : property_id_(id),
      identifier_value_(CSSIdentifierValue::Create(value_id)),
      tag_name_(&tag_name) {
  DCHECK_NE(value_id, CSSValueInvalid);
}

bool HTMLElementEquivalent::ValueIsPresentInStyle(
    HTMLElement* element,
    CSSPropertyValueSet* style) const {
  const CSSValue* value = style->GetPropertyCSSValue(property_id_);

  // TODO: Does this work on style or computed style? The code here, but we
  // might need to do something here to match CSSPrimitiveValues. if
  // (property_id_ == CSSPropertyFontWeight &&
  //     identifier_value_->GetValueID() == CSSValueBold) {
  //   if (value->IsPrimitiveValue() &&
  //       ToCSSPrimitiveValue(value)->GetFloatValue() >= BoldThreshold()) {
  //     LOG(INFO) << "weight match in HTMLElementEquivalent for primitive
  //     value"; return true;
  //   } else {
  //     LOG(INFO) << "weight match in HTMLElementEquivalent for identifier
  //     value";
  //   }
  // }

  return Matches(element) && value && value->IsIdentifierValue() &&
         ToCSSIdentifierValue(value)->GetValueID() ==
             identifier_value_->GetValueID();
}

void HTMLElementEquivalent::AddToStyle(Element* element,
                                       EditingStyle* style) const {
  style->SetProperty(property_id_, identifier_value_->CssText(),
                     /* important */ false,
                     element->GetDocument().GetSecureContextMode());
}

class HTMLTextDecorationEquivalent final : public HTMLElementEquivalent {
 public:
  static HTMLElementEquivalent* Create(CSSValueID primitive_value,
                                       const HTMLQualifiedName& tag_name) {
    return new HTMLTextDecorationEquivalent(primitive_value, tag_name);
  }
  bool PropertyExistsInStyle(const CSSPropertyValueSet*) const override;
  bool ValueIsPresentInStyle(HTMLElement*, CSSPropertyValueSet*) const override;

  void Trace(blink::Visitor* visitor) override {
    HTMLElementEquivalent::Trace(visitor);
  }

 private:
  HTMLTextDecorationEquivalent(CSSValueID primitive_value,
                               const HTMLQualifiedName& tag_name);
};

HTMLTextDecorationEquivalent::HTMLTextDecorationEquivalent(
    CSSValueID primitive_value,
    const HTMLQualifiedName& tag_name)
    : HTMLElementEquivalent(CSSPropertyTextDecorationLine,
                            primitive_value,
                            tag_name)
// m_propertyID is used in HTMLElementEquivalent::addToStyle
{}

bool HTMLTextDecorationEquivalent::PropertyExistsInStyle(
    const CSSPropertyValueSet* style) const {
  return style->GetPropertyCSSValue(CSSPropertyWebkitTextDecorationsInEffect) ||
         style->GetPropertyCSSValue(CSSPropertyTextDecorationLine);
}

bool HTMLTextDecorationEquivalent::ValueIsPresentInStyle(
    HTMLElement* element,
    CSSPropertyValueSet* style) const {
  const CSSValue* style_value =
      style->GetPropertyCSSValue(CSSPropertyWebkitTextDecorationsInEffect);
  if (!style_value)
    style_value = style->GetPropertyCSSValue(CSSPropertyTextDecorationLine);
  return Matches(element) && style_value && style_value->IsValueList() &&
         ToCSSValueList(style_value)->HasValue(*identifier_value_);
}

class HTMLAttributeEquivalent : public HTMLElementEquivalent {
 public:
  static HTMLAttributeEquivalent* Create(CSSPropertyID property_id,
                                         const HTMLQualifiedName& tag_name,
                                         const QualifiedName& attr_name) {
    return new HTMLAttributeEquivalent(property_id, tag_name, attr_name);
  }
  static HTMLAttributeEquivalent* Create(CSSPropertyID property_id,
                                         const QualifiedName& attr_name) {
    return new HTMLAttributeEquivalent(property_id, attr_name);
  }

  bool Matches(const Element* element) const override {
    return HTMLElementEquivalent::Matches(element) &&
           element->hasAttribute(attr_name_);
  }
  bool HasAttribute() const override { return true; }
  bool ValueIsPresentInStyle(HTMLElement*, CSSPropertyValueSet*) const override;
  void AddToStyle(Element*, EditingStyle*) const override;
  virtual const CSSValue* AttributeValueAsCSSValue(Element*) const;
  inline const QualifiedName& AttributeName() const { return attr_name_; }

  void Trace(blink::Visitor* visitor) override {
    HTMLElementEquivalent::Trace(visitor);
  }

 protected:
  HTMLAttributeEquivalent(CSSPropertyID,
                          const HTMLQualifiedName& tag_name,
                          const QualifiedName& attr_name);
  HTMLAttributeEquivalent(CSSPropertyID, const QualifiedName& attr_name);
  // We can store a reference because HTML attribute names are const global.
  const QualifiedName& attr_name_;
};

HTMLAttributeEquivalent::HTMLAttributeEquivalent(
    CSSPropertyID id,
    const HTMLQualifiedName& tag_name,
    const QualifiedName& attr_name)
    : HTMLElementEquivalent(id, tag_name), attr_name_(attr_name) {}

HTMLAttributeEquivalent::HTMLAttributeEquivalent(CSSPropertyID id,
                                                 const QualifiedName& attr_name)
    : HTMLElementEquivalent(id), attr_name_(attr_name) {}

bool HTMLAttributeEquivalent::ValueIsPresentInStyle(
    HTMLElement* element,
    CSSPropertyValueSet* style) const {
  const CSSValue* value = AttributeValueAsCSSValue(element);
  const CSSValue* style_value = style->GetPropertyCSSValue(property_id_);

  return DataEquivalent(value, style_value);
}

void HTMLAttributeEquivalent::AddToStyle(Element* element,
                                         EditingStyle* style) const {
  if (const CSSValue* value = AttributeValueAsCSSValue(element)) {
    style->SetProperty(property_id_, value->CssText(), /* important */ false,
                       element->GetDocument().GetSecureContextMode());
  }
}

const CSSValue* HTMLAttributeEquivalent::AttributeValueAsCSSValue(
    Element* element) const {
  DCHECK(element);
  const AtomicString& value = element->getAttribute(attr_name_);
  if (value.IsNull())
    return nullptr;

  MutableCSSPropertyValueSet* dummy_style = nullptr;
  dummy_style = MutableCSSPropertyValueSet::Create(kHTMLQuirksMode);
  dummy_style->SetProperty(property_id_, value, /* important */ false,
                           element->GetDocument().GetSecureContextMode());
  return dummy_style->GetPropertyCSSValue(property_id_);
}

class HTMLFontSizeEquivalent final : public HTMLAttributeEquivalent {
 public:
  static HTMLFontSizeEquivalent* Create() {
    return new HTMLFontSizeEquivalent();
  }
  const CSSValue* AttributeValueAsCSSValue(Element*) const override;

  void Trace(blink::Visitor* visitor) override {
    HTMLAttributeEquivalent::Trace(visitor);
  }

 private:
  HTMLFontSizeEquivalent();
};

HTMLFontSizeEquivalent::HTMLFontSizeEquivalent()
    : HTMLAttributeEquivalent(CSSPropertyFontSize,
                              HTMLNames::fontTag,
                              HTMLNames::sizeAttr) {}

const CSSValue* HTMLFontSizeEquivalent::AttributeValueAsCSSValue(
    Element* element) const {
  DCHECK(element);
  const AtomicString& value = element->getAttribute(attr_name_);
  if (value.IsNull())
    return nullptr;
  CSSValueID size;
  if (!HTMLFontElement::CssValueFromFontSizeNumber(value, size))
    return nullptr;
  return CSSIdentifierValue::Create(size);
}

float EditingStyle::no_font_delta_ = 0.0f;

EditingStyle::EditingStyle(ContainerNode* node,
                           PropertiesToInclude properties_to_include) {
  Init(node, properties_to_include);
}

EditingStyle::EditingStyle(const Position& position,
                           PropertiesToInclude properties_to_include) {
  Init(position.AnchorNode(), properties_to_include);
}

EditingStyle::EditingStyle(const CSSPropertyValueSet* style)
    : mutable_style_(style ? style->MutableCopy() : nullptr) {
  ExtractFontSizeDelta();
}

EditingStyle::EditingStyle(CSSPropertyID property_id,
                           const String& value,
                           SecureContextMode secure_context_mode)
    : mutable_style_(nullptr) {
  SetProperty(property_id, value, /* important */ false, secure_context_mode);
  is_vertical_align_ = property_id == CSSPropertyVerticalAlign &&
                       (value == "sub" || value == "super");
}

static Color CssValueToColor(const CSSValue* color_value) {
  if (!color_value ||
      (!color_value->IsColorValue() && !color_value->IsPrimitiveValue() &&
       !color_value->IsIdentifierValue()))
    return Color::kTransparent;

  if (color_value->IsColorValue())
    return ToCSSColorValue(color_value)->Value();

  Color color = 0;
  // FIXME: Why ignore the return value?
  CSSParser::ParseColor(color, color_value->CssText());
  return color;
}

static inline Color GetFontColor(CSSStyleDeclaration* style) {
  return CssValueToColor(style->GetPropertyCSSValueInternal(CSSPropertyColor));
}

static inline Color GetFontColor(CSSPropertyValueSet* style) {
  return CssValueToColor(style->GetPropertyCSSValue(CSSPropertyColor));
}

static inline Color GetBackgroundColor(CSSStyleDeclaration* style) {
  return CssValueToColor(
      style->GetPropertyCSSValueInternal(CSSPropertyBackgroundColor));
}

static inline Color GetBackgroundColor(CSSPropertyValueSet* style) {
  return CssValueToColor(
      style->GetPropertyCSSValue(CSSPropertyBackgroundColor));
}

static inline Color BackgroundColorInEffect(Node* node) {
  return CssValueToColor(
      EditingStyleUtilities::BackgroundColorValueInEffect(node));
}

static int TextAlignResolvingStartAndEnd(int text_align, int direction) {
  switch (text_align) {
    case CSSValueCenter:
    case CSSValueWebkitCenter:
      return CSSValueCenter;
    case CSSValueJustify:
      return CSSValueJustify;
    case CSSValueLeft:
    case CSSValueWebkitLeft:
      return CSSValueLeft;
    case CSSValueRight:
    case CSSValueWebkitRight:
      return CSSValueRight;
    case CSSValueStart:
      return direction != CSSValueRtl ? CSSValueLeft : CSSValueRight;
    case CSSValueEnd:
      return direction == CSSValueRtl ? CSSValueRight : CSSValueLeft;
  }
  return CSSValueInvalid;
}

template <typename T>
static int TextAlignResolvingStartAndEnd(T* style) {
  return TextAlignResolvingStartAndEnd(
      GetIdentifierValue(style, CSSPropertyTextAlign),
      GetIdentifierValue(style, CSSPropertyDirection));
}

void EditingStyle::Init(Node* node, PropertiesToInclude properties_to_include) {
  if (IsTabHTMLSpanElementTextNode(node))
    node = TabSpanElement(node)->parentNode();
  else if (IsTabHTMLSpanElement(node))
    node = node->parentNode();

  CSSComputedStyleDeclaration* computed_style_at_position =
      CSSComputedStyleDeclaration::Create(node);
  mutable_style_ =
      properties_to_include == kAllProperties && computed_style_at_position
          ? computed_style_at_position->CopyProperties()
          : CopyEditingProperties(computed_style_at_position);

  if (properties_to_include == kEditingPropertiesInEffect) {
    if (const CSSValue* value =
            EditingStyleUtilities::BackgroundColorValueInEffect(node)) {
      mutable_style_->SetProperty(CSSPropertyBackgroundColor, value->CssText(),
                                  /* important */ false,
                                  node->GetDocument().GetSecureContextMode());
    }
    if (const CSSValue* value = computed_style_at_position->GetPropertyCSSValue(
            GetCSSPropertyWebkitTextDecorationsInEffect())) {
      mutable_style_->SetProperty(CSSPropertyTextDecoration, value->CssText(),
                                  /* important */ false,
                                  node->GetDocument().GetSecureContextMode());
    }
  }

  if (node && node->EnsureComputedStyle()) {
    const ComputedStyle* computed_style = node->EnsureComputedStyle();

    // Fix for crbug.com/768261: due to text-autosizing, reading the current
    // computed font size and re-writing it to an element may actually cause the
    // font size to become larger (since the autosizer will run again on the new
    // computed size). The fix is to toss out the computed size property here
    // and use ComputedStyle::SpecifiedFontSize().
    if (computed_style->ComputedFontSize() !=
        computed_style->SpecifiedFontSize()) {
      // ReplaceSelectionCommandTest_TextAutosizingDoesntInflateText gets here.
      mutable_style_->SetProperty(
          CSSPropertyFontSize,
          CSSPrimitiveValue::Create(computed_style->SpecifiedFontSize(),
                                    CSSPrimitiveValue::UnitType::kPixels)
              ->CssText(),
          /* important */ false, node->GetDocument().GetSecureContextMode());
    }

    RemoveInheritedColorsIfNeeded(computed_style);
    ReplaceFontSizeByKeywordIfPossible(
        computed_style, node->GetDocument().GetSecureContextMode(),
        computed_style_at_position);
  }

  is_monospace_font_ = computed_style_at_position->IsMonospaceFont();
  ExtractFontSizeDelta();
}

void EditingStyle::RemoveInheritedColorsIfNeeded(
    const ComputedStyle* computed_style) {
  // If a node's text fill color is currentColor, then its children use
  // their font-color as their text fill color (they don't
  // inherit it).  Likewise for stroke color.
  // Similar thing happens for caret-color if it's auto or currentColor.
  if (computed_style->TextFillColor().IsCurrentColor())
    mutable_style_->RemoveProperty(CSSPropertyWebkitTextFillColor);
  if (computed_style->TextStrokeColor().IsCurrentColor())
    mutable_style_->RemoveProperty(CSSPropertyWebkitTextStrokeColor);
  if (computed_style->CaretColor().IsAutoColor() ||
      computed_style->CaretColor().IsCurrentColor())
    mutable_style_->RemoveProperty(CSSPropertyCaretColor);
}

void EditingStyle::SetProperty(CSSPropertyID property_id,
                               const String& value,
                               bool important,
                               SecureContextMode secure_context_mode) {
  if (!mutable_style_)
    mutable_style_ = MutableCSSPropertyValueSet::Create(kHTMLQuirksMode);

  mutable_style_->SetProperty(property_id, value, important,
                              secure_context_mode);
}

void EditingStyle::ReplaceFontSizeByKeywordIfPossible(
    const ComputedStyle* computed_style,
    SecureContextMode secure_context_mode,
    CSSComputedStyleDeclaration* css_computed_style) {
  DCHECK(computed_style);
  if (computed_style->GetFontDescription().KeywordSize()) {
    mutable_style_->SetProperty(
        CSSPropertyFontSize,
        css_computed_style->GetFontSizeCSSValuePreferringKeyword()->CssText(),
        /* important */ false, secure_context_mode);
  }
}

void EditingStyle::ExtractFontSizeDelta() {
  if (!mutable_style_)
    return;

  if (mutable_style_->GetPropertyCSSValue(CSSPropertyFontSize)) {
    // Explicit font size overrides any delta.
    mutable_style_->RemoveProperty(CSSPropertyWebkitFontSizeDelta);
    return;
  }

  // Get the adjustment amount out of the style.
  const CSSValue* value =
      mutable_style_->GetPropertyCSSValue(CSSPropertyWebkitFontSizeDelta);
  if (!value || !value->IsPrimitiveValue())
    return;

  const CSSPrimitiveValue* primitive_value = ToCSSPrimitiveValue(value);

  // Only PX handled now. If we handle more types in the future, perhaps
  // a switch statement here would be more appropriate.
  if (!primitive_value->IsPx())
    return;

  font_size_delta_ = primitive_value->GetFloatValue();
  mutable_style_->RemoveProperty(CSSPropertyWebkitFontSizeDelta);
}

bool EditingStyle::IsEmpty() const {
  return (!mutable_style_ || mutable_style_->IsEmpty()) &&
         font_size_delta_ == no_font_delta_;
}

bool EditingStyle::GetTextDirection(WritingDirection& writing_direction) const {
  if (!mutable_style_)
    return false;

  const CSSValue* unicode_bidi =
      mutable_style_->GetPropertyCSSValue(CSSPropertyUnicodeBidi);
  if (!unicode_bidi || !unicode_bidi->IsIdentifierValue())
    return false;

  CSSValueID unicode_bidi_value =
      ToCSSIdentifierValue(unicode_bidi)->GetValueID();
  if (EditingStyleUtilities::IsEmbedOrIsolate(unicode_bidi_value)) {
    const CSSValue* direction =
        mutable_style_->GetPropertyCSSValue(CSSPropertyDirection);
    if (!direction || !direction->IsIdentifierValue())
      return false;

    writing_direction =
        ToCSSIdentifierValue(direction)->GetValueID() == CSSValueLtr
            ? WritingDirection::kLeftToRight
            : WritingDirection::kRightToLeft;

    return true;
  }

  if (unicode_bidi_value == CSSValueNormal) {
    writing_direction = WritingDirection::kNatural;
    return true;
  }

  return false;
}

void EditingStyle::OverrideWithStyle(const CSSPropertyValueSet* style) {
  if (!style || style->IsEmpty())
    return;
  if (!mutable_style_)
    mutable_style_ = MutableCSSPropertyValueSet::Create(kHTMLQuirksMode);
  mutable_style_->MergeAndOverrideOnConflict(style);
  ExtractFontSizeDelta();
}

void EditingStyle::Clear() {
  mutable_style_.Clear();
  is_monospace_font_ = false;
  font_size_delta_ = no_font_delta_;
}

EditingStyle* EditingStyle::Copy() const {
  EditingStyle* copy = EditingStyle::Create();
  if (mutable_style_)
    copy->mutable_style_ = mutable_style_->MutableCopy();
  copy->is_monospace_font_ = is_monospace_font_;
  copy->font_size_delta_ = font_size_delta_;
  return copy;
}

// This is the list of CSS properties that apply specially to block-level
// elements.
static const CSSPropertyID kStaticBlockProperties[] = {
    CSSPropertyBreakAfter,
    CSSPropertyBreakBefore,
    CSSPropertyBreakInside,
    CSSPropertyOrphans,
    CSSPropertyOverflow,  // This can be also be applied to replaced elements
    CSSPropertyColumnCount,
    CSSPropertyColumnGap,
    CSSPropertyColumnRuleColor,
    CSSPropertyColumnRuleStyle,
    CSSPropertyColumnRuleWidth,
    CSSPropertyWebkitColumnBreakBefore,
    CSSPropertyWebkitColumnBreakAfter,
    CSSPropertyWebkitColumnBreakInside,
    CSSPropertyColumnWidth,
    CSSPropertyPageBreakAfter,
    CSSPropertyPageBreakBefore,
    CSSPropertyPageBreakInside,
    CSSPropertyTextAlign,
    CSSPropertyTextAlignLast,
    CSSPropertyTextIndent,
    CSSPropertyTextJustify,
    CSSPropertyWidows};

static Vector<const CSSProperty*>& BlockPropertiesVector() {
  DEFINE_STATIC_LOCAL(Vector<const CSSProperty*>, properties, ());
  if (properties.IsEmpty()) {
    CSSProperty::FilterEnabledCSSPropertiesIntoVector(
        kStaticBlockProperties, arraysize(kStaticBlockProperties), properties);
  }
  return properties;
}

EditingStyle* EditingStyle::ExtractAndRemoveBlockProperties() {
  EditingStyle* block_properties = EditingStyle::Create();
  if (!mutable_style_)
    return block_properties;

  block_properties->mutable_style_ =
      mutable_style_->CopyPropertiesInSet(BlockPropertiesVector());
  RemoveBlockProperties();

  return block_properties;
}

EditingStyle* EditingStyle::ExtractAndRemoveTextDirection(
    SecureContextMode secure_context_mode) {
  EditingStyle* text_direction = EditingStyle::Create();
  text_direction->mutable_style_ =
      MutableCSSPropertyValueSet::Create(kHTMLQuirksMode);
  text_direction->mutable_style_->SetProperty(
      CSSPropertyUnicodeBidi, CSSValueIsolate,
      mutable_style_->PropertyIsImportant(CSSPropertyUnicodeBidi));

  text_direction->mutable_style_->SetProperty(
      CSSPropertyDirection,
      mutable_style_->GetPropertyValue(CSSPropertyDirection),
      mutable_style_->PropertyIsImportant(CSSPropertyDirection),
      secure_context_mode);

  mutable_style_->RemoveProperty(CSSPropertyUnicodeBidi);
  mutable_style_->RemoveProperty(CSSPropertyDirection);

  return text_direction;
}

void EditingStyle::RemoveBlockProperties() {
  if (!mutable_style_)
    return;

  mutable_style_->RemovePropertiesInSet(BlockPropertiesVector().data(),
                                        BlockPropertiesVector().size());
}

void EditingStyle::RemoveStyleAddedByElement(Element* element) {
  if (!element || !element->parentNode())
    return;
  MutableCSSPropertyValueSet* parent_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element->parentNode()),
      kAllEditingProperties);
  MutableCSSPropertyValueSet* node_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element), kAllEditingProperties);
  node_style->RemoveEquivalentProperties(parent_style);
  mutable_style_->RemoveEquivalentProperties(node_style);
}

void EditingStyle::RemoveStyleConflictingWithStyleOfElement(Element* element) {
  if (!element || !element->parentNode() || !mutable_style_)
    return;

  MutableCSSPropertyValueSet* parent_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element->parentNode()),
      kAllEditingProperties);
  MutableCSSPropertyValueSet* node_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element), kAllEditingProperties);
  node_style->RemoveEquivalentProperties(parent_style);

  unsigned property_count = node_style->PropertyCount();
  for (unsigned i = 0; i < property_count; ++i)
    mutable_style_->RemoveProperty(node_style->PropertyAt(i).Id());
}

void EditingStyle::CollapseTextDecorationProperties(
    SecureContextMode secure_context_mode) {
  if (!mutable_style_)
    return;

  const CSSValue* text_decorations_in_effect =
      mutable_style_->GetPropertyCSSValue(
          CSSPropertyWebkitTextDecorationsInEffect);
  if (!text_decorations_in_effect)
    return;

  if (text_decorations_in_effect->IsValueList()) {
    mutable_style_->SetProperty(
        CSSPropertyTextDecorationLine, text_decorations_in_effect->CssText(),
        mutable_style_->PropertyIsImportant(CSSPropertyTextDecorationLine),
        secure_context_mode);
  } else {
    mutable_style_->RemoveProperty(CSSPropertyTextDecorationLine);
  }
  mutable_style_->RemoveProperty(CSSPropertyWebkitTextDecorationsInEffect);
}

EditingTriState EditingStyle::TriStateOfStyle(
    EditingStyle* style,
    SecureContextMode secure_context_mode) const {
  if (!style || !style->mutable_style_)
    return EditingTriState::kFalse;
  return TriStateOfStyle(style->mutable_style_->EnsureCSSStyleDeclaration(),
                         kDoNotIgnoreTextOnlyProperties, secure_context_mode);
}

EditingTriState EditingStyle::TriStateOfStyle(
    CSSStyleDeclaration* style_to_compare,
    ShouldIgnoreTextOnlyProperties should_ignore_text_only_properties,
    SecureContextMode secure_context_mode) const {
  MutableCSSPropertyValueSet* difference = GetPropertiesNotIn(
      mutable_style_.Get(), style_to_compare, secure_context_mode);

  // CSS properties that create a visual difference only when applied to text.
  static const CSSProperty* kTextOnlyProperties[] = {
      // FIXME: CSSPropertyTextDecoration needs to be removed when CSS3 Text
      // Decoration feature is no longer experimental.
      &GetCSSPropertyTextDecoration(),
      &GetCSSPropertyTextDecorationLine(),
      &GetCSSPropertyWebkitTextDecorationsInEffect(),
      &GetCSSPropertyFontStyle(),
      &GetCSSPropertyFontWeight(),
      &GetCSSPropertyColor(),
  };
  if (should_ignore_text_only_properties == kIgnoreTextOnlyProperties) {
    difference->RemovePropertiesInSet(kTextOnlyProperties,
                                      arraysize(kTextOnlyProperties));
  }

  if (difference->IsEmpty())
    return EditingTriState::kTrue;
  if (difference->PropertyCount() == mutable_style_->PropertyCount())
    return EditingTriState::kFalse;

  return EditingTriState::kMixed;
}

EditingTriState EditingStyle::TriStateOfStyle(
    const VisibleSelection& selection,
    SecureContextMode secure_context_mode) const {
  if (selection.IsNone())
    return EditingTriState::kFalse;

  if (selection.IsCaret()) {
    return TriStateOfStyle(
        EditingStyleUtilities::CreateStyleAtSelectionStart(selection),
        secure_context_mode);
  }

  EditingTriState state = EditingTriState::kFalse;
  bool node_is_start = true;
  for (Node& node : NodeTraversal::StartsAt(*selection.Start().AnchorNode())) {
    if (node.GetLayoutObject() && HasEditableStyle(node)) {
      CSSComputedStyleDeclaration* node_style =
          CSSComputedStyleDeclaration::Create(&node);
      if (node_style) {
        // If the selected element has <sub> or <sup> ancestor element, apply
        // the corresponding style(vertical-align) to it so that
        // document.queryCommandState() works with the style. See bug
        // http://crbug.com/582225.
        if (is_vertical_align_ &&
            GetIdentifierValue(node_style, CSSPropertyVerticalAlign) ==
                CSSValueBaseline) {
          const CSSIdentifierValue* vertical_align = ToCSSIdentifierValue(
              mutable_style_->GetPropertyCSSValue(CSSPropertyVerticalAlign));
          if (EditingStyleUtilities::HasAncestorVerticalAlignStyle(
                  node, vertical_align->GetValueID()))
            node.MutableComputedStyle()->SetVerticalAlign(
                vertical_align->ConvertTo<EVerticalAlign>());
        }

        // Pass EditingStyle::DoNotIgnoreTextOnlyProperties without checking if
        // node.isTextNode() because the node can be an element node. See bug
        // http://crbug.com/584939.
        EditingTriState node_state = TriStateOfStyle(
            node_style, EditingStyle::kDoNotIgnoreTextOnlyProperties,
            secure_context_mode);
        if (node_is_start) {
          state = node_state;
          node_is_start = false;
        } else if (state != node_state && node.IsTextNode()) {
          state = EditingTriState::kMixed;
          break;
        }
      }
    }
    if (&node == selection.End().AnchorNode())
      break;
  }

  return state;
}

bool EditingStyle::ConflictsWithInlineStyleOfElement(
    HTMLElement* element,
    EditingStyle* extracted_style,
    Vector<CSSPropertyID>* conflicting_properties) const {
  DCHECK(element);
  DCHECK(!conflicting_properties || conflicting_properties->IsEmpty());

  const CSSPropertyValueSet* inline_style = element->InlineStyle();
  if (!mutable_style_ || !inline_style)
    return false;

  unsigned property_count = mutable_style_->PropertyCount();
  for (unsigned i = 0; i < property_count; ++i) {
    CSSPropertyID property_id = mutable_style_->PropertyAt(i).Id();

    // We don't override whitespace property of a tab span because that would
    // collapse the tab into a space.
    if (property_id == CSSPropertyWhiteSpace && IsTabHTMLSpanElement(element))
      continue;

    if (property_id == CSSPropertyWebkitTextDecorationsInEffect &&
        inline_style->GetPropertyCSSValue(CSSPropertyTextDecorationLine)) {
      if (!conflicting_properties)
        return true;
      conflicting_properties->push_back(CSSPropertyTextDecoration);
      // Because text-decoration expands to text-decoration-line,
      // we also state it as conflicting.
      conflicting_properties->push_back(CSSPropertyTextDecorationLine);
      if (extracted_style) {
        extracted_style->SetProperty(
            CSSPropertyTextDecorationLine,
            inline_style->GetPropertyValue(CSSPropertyTextDecorationLine),
            inline_style->PropertyIsImportant(CSSPropertyTextDecorationLine),
            element->GetDocument().GetSecureContextMode());
      }
      continue;
    }

    if (!inline_style->GetPropertyCSSValue(property_id))
      continue;

    if (property_id == CSSPropertyUnicodeBidi &&
        inline_style->GetPropertyCSSValue(CSSPropertyDirection)) {
      if (!conflicting_properties)
        return true;
      conflicting_properties->push_back(CSSPropertyDirection);
      if (extracted_style) {
        extracted_style->SetProperty(
            property_id, inline_style->GetPropertyValue(property_id),
            inline_style->PropertyIsImportant(property_id),
            element->GetDocument().GetSecureContextMode());
      }
    }

    if (!conflicting_properties)
      return true;

    conflicting_properties->push_back(property_id);

    if (extracted_style) {
      extracted_style->SetProperty(
          property_id, inline_style->GetPropertyValue(property_id),
          inline_style->PropertyIsImportant(property_id),
          element->GetDocument().GetSecureContextMode());
    }
  }

  return conflicting_properties && !conflicting_properties->IsEmpty();
}

static const HeapVector<Member<HTMLElementEquivalent>>&
HtmlElementEquivalents() {
  DEFINE_STATIC_LOCAL(Persistent<HeapVector<Member<HTMLElementEquivalent>>>,
                      html_element_equivalents,
                      (new HeapVector<Member<HTMLElementEquivalent>>));
  if (!html_element_equivalents->size()) {
    html_element_equivalents->push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontWeight, CSSValueBold, HTMLNames::bTag));
    html_element_equivalents->push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontWeight, CSSValueBold, HTMLNames::strongTag));
    html_element_equivalents->push_back(HTMLElementEquivalent::Create(
        CSSPropertyVerticalAlign, CSSValueSub, HTMLNames::subTag));
    html_element_equivalents->push_back(HTMLElementEquivalent::Create(
        CSSPropertyVerticalAlign, CSSValueSuper, HTMLNames::supTag));
    html_element_equivalents->push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontStyle, CSSValueItalic, HTMLNames::iTag));
    html_element_equivalents->push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontStyle, CSSValueItalic, HTMLNames::emTag));

    html_element_equivalents->push_back(HTMLTextDecorationEquivalent::Create(
        CSSValueUnderline, HTMLNames::uTag));
    html_element_equivalents->push_back(HTMLTextDecorationEquivalent::Create(
        CSSValueLineThrough, HTMLNames::sTag));
    html_element_equivalents->push_back(HTMLTextDecorationEquivalent::Create(
        CSSValueLineThrough, HTMLNames::strikeTag));
  }

  return *html_element_equivalents;
}

bool EditingStyle::ConflictsWithImplicitStyleOfElement(
    HTMLElement* element,
    EditingStyle* extracted_style,
    ShouldExtractMatchingStyle should_extract_matching_style) const {
  if (!mutable_style_)
    return false;

  const HeapVector<Member<HTMLElementEquivalent>>& html_element_equivalents =
      HtmlElementEquivalents();
  for (wtf_size_t i = 0; i < html_element_equivalents.size(); ++i) {
    const HTMLElementEquivalent* equivalent = html_element_equivalents[i].Get();
    if (equivalent->Matches(element) &&
        equivalent->PropertyExistsInStyle(mutable_style_.Get()) &&
        (should_extract_matching_style == kExtractMatchingStyle ||
         !equivalent->ValueIsPresentInStyle(element, mutable_style_.Get()))) {
      if (extracted_style)
        equivalent->AddToStyle(element, extracted_style);
      return true;
    }
  }
  return false;
}

static const HeapVector<Member<HTMLAttributeEquivalent>>&
HtmlAttributeEquivalents() {
  DEFINE_STATIC_LOCAL(Persistent<HeapVector<Member<HTMLAttributeEquivalent>>>,
                      html_attribute_equivalents,
                      (new HeapVector<Member<HTMLAttributeEquivalent>>));
  if (!html_attribute_equivalents->size()) {
    // elementIsStyledSpanOrHTMLEquivalent depends on the fact each
    // HTMLAttriuteEquivalent matches exactly one attribute of exactly one
    // element except dirAttr.
    html_attribute_equivalents->push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyColor, HTMLNames::fontTag, HTMLNames::colorAttr));
    html_attribute_equivalents->push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyFontFamily, HTMLNames::fontTag, HTMLNames::faceAttr));
    html_attribute_equivalents->push_back(HTMLFontSizeEquivalent::Create());

    html_attribute_equivalents->push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyDirection, HTMLNames::dirAttr));
    html_attribute_equivalents->push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyUnicodeBidi, HTMLNames::dirAttr));
  }

  return *html_attribute_equivalents;
}

bool EditingStyle::ConflictsWithImplicitStyleOfAttributes(
    HTMLElement* element) const {
  DCHECK(element);
  if (!mutable_style_)
    return false;

  const HeapVector<Member<HTMLAttributeEquivalent>>&
      html_attribute_equivalents = HtmlAttributeEquivalents();
  for (const auto& equivalent : html_attribute_equivalents) {
    if (equivalent->Matches(element) &&
        equivalent->PropertyExistsInStyle(mutable_style_.Get()) &&
        !equivalent->ValueIsPresentInStyle(element, mutable_style_.Get()))
      return true;
  }

  return false;
}

bool EditingStyle::ExtractConflictingImplicitStyleOfAttributes(
    HTMLElement* element,
    ShouldPreserveWritingDirection should_preserve_writing_direction,
    EditingStyle* extracted_style,
    Vector<QualifiedName>& conflicting_attributes,
    ShouldExtractMatchingStyle should_extract_matching_style) const {
  DCHECK(element);
  // HTMLAttributeEquivalent::addToStyle doesn't support unicode-bidi and
  // direction properties
  if (extracted_style)
    DCHECK_EQ(should_preserve_writing_direction, kPreserveWritingDirection);
  if (!mutable_style_)
    return false;

  const HeapVector<Member<HTMLAttributeEquivalent>>&
      html_attribute_equivalents = HtmlAttributeEquivalents();
  bool removed = false;
  for (const auto& attribute : html_attribute_equivalents) {
    const HTMLAttributeEquivalent* equivalent = attribute.Get();

    // unicode-bidi and direction are pushed down separately so don't push down
    // with other styles.
    if (should_preserve_writing_direction == kPreserveWritingDirection &&
        equivalent->AttributeName() == HTMLNames::dirAttr)
      continue;

    if (!equivalent->Matches(element) ||
        !equivalent->PropertyExistsInStyle(mutable_style_.Get()) ||
        (should_extract_matching_style == kDoNotExtractMatchingStyle &&
         equivalent->ValueIsPresentInStyle(element, mutable_style_.Get())))
      continue;

    if (extracted_style)
      equivalent->AddToStyle(element, extracted_style);
    conflicting_attributes.push_back(equivalent->AttributeName());
    removed = true;
  }

  return removed;
}

bool EditingStyle::StyleIsPresentInComputedStyleOfNode(Node* node) const {
  return !mutable_style_ ||
         GetPropertiesNotIn(mutable_style_.Get(),
                            CSSComputedStyleDeclaration::Create(node),
                            node->GetDocument().GetSecureContextMode())
             ->IsEmpty();
}

bool EditingStyle::ElementIsStyledSpanOrHTMLEquivalent(
    const HTMLElement* element) {
  DCHECK(element);
  bool element_is_span_or_element_equivalent = false;
  if (IsHTMLSpanElement(*element)) {
    element_is_span_or_element_equivalent = true;
  } else {
    const HeapVector<Member<HTMLElementEquivalent>>& html_element_equivalents =
        HtmlElementEquivalents();
    wtf_size_t i;
    for (i = 0; i < html_element_equivalents.size(); ++i) {
      if (html_element_equivalents[i]->Matches(element)) {
        element_is_span_or_element_equivalent = true;
        break;
      }
    }
  }

  AttributeCollection attributes = element->Attributes();
  if (attributes.IsEmpty()) {
    // span, b, etc... without any attributes
    return element_is_span_or_element_equivalent;
  }

  unsigned matched_attributes = 0;
  const HeapVector<Member<HTMLAttributeEquivalent>>&
      html_attribute_equivalents = HtmlAttributeEquivalents();
  for (const auto& equivalent : html_attribute_equivalents) {
    if (equivalent->Matches(element) &&
        equivalent->AttributeName() != HTMLNames::dirAttr)
      matched_attributes++;
  }

  if (!element_is_span_or_element_equivalent && !matched_attributes) {
    // element is not a span, a html element equivalent, or font element.
    return false;
  }

  if (element->hasAttribute(HTMLNames::styleAttr)) {
    if (const CSSPropertyValueSet* style = element->InlineStyle()) {
      unsigned property_count = style->PropertyCount();
      for (unsigned i = 0; i < property_count; ++i) {
        if (!IsEditingProperty(style->PropertyAt(i).Id()))
          return false;
      }
    }
    matched_attributes++;
  }

  // font with color attribute, span with style attribute, etc...
  DCHECK_LE(matched_attributes, attributes.size());
  return matched_attributes >= attributes.size();
}

void EditingStyle::PrepareToApplyAt(
    const Position& position,
    ShouldPreserveWritingDirection should_preserve_writing_direction) {
  if (!mutable_style_)
    return;

  // ReplaceSelectionCommand::handleStyleSpans() requires that this function
  // only removes the editing style. If this function was modified in the future
  // to delete all redundant properties, then add a boolean value to indicate
  // which one of editingStyleAtPosition or computedStyle is called.
  EditingStyle* editing_style_at_position =
      EditingStyle::Create(position, kEditingPropertiesInEffect);
  CSSPropertyValueSet* style_at_position =
      editing_style_at_position->mutable_style_.Get();

  const CSSValue* unicode_bidi = nullptr;
  const CSSValue* direction = nullptr;
  if (should_preserve_writing_direction == kPreserveWritingDirection) {
    unicode_bidi = mutable_style_->GetPropertyCSSValue(CSSPropertyUnicodeBidi);
    direction = mutable_style_->GetPropertyCSSValue(CSSPropertyDirection);
  }

  mutable_style_->RemoveEquivalentProperties(style_at_position);

  if (TextAlignResolvingStartAndEnd(mutable_style_.Get()) ==
      TextAlignResolvingStartAndEnd(style_at_position))
    mutable_style_->RemoveProperty(CSSPropertyTextAlign);

  if (GetFontColor(mutable_style_.Get()) == GetFontColor(style_at_position))
    mutable_style_->RemoveProperty(CSSPropertyColor);

  if (EditingStyleUtilities::HasTransparentBackgroundColor(
          mutable_style_.Get()) ||
      CssValueToColor(
          mutable_style_->GetPropertyCSSValue(CSSPropertyBackgroundColor)) ==
          BackgroundColorInEffect(position.ComputeContainerNode()))
    mutable_style_->RemoveProperty(CSSPropertyBackgroundColor);

  if (unicode_bidi && unicode_bidi->IsIdentifierValue()) {
    mutable_style_->SetProperty(
        CSSPropertyUnicodeBidi,
        ToCSSIdentifierValue(unicode_bidi)->GetValueID());
    if (direction && direction->IsIdentifierValue()) {
      mutable_style_->SetProperty(
          CSSPropertyDirection, ToCSSIdentifierValue(direction)->GetValueID());
    }
  }
}

void EditingStyle::MergeTypingStyle(Document* document) {
  DCHECK(document);

  EditingStyle* typing_style = document->GetFrame()->GetEditor().TypingStyle();
  if (!typing_style || typing_style == this)
    return;

  MergeStyle(typing_style->Style(), kOverrideValues);
}

void EditingStyle::MergeInlineStyleOfElement(
    HTMLElement* element,
    CSSPropertyOverrideMode mode,
    PropertiesToInclude properties_to_include) {
  DCHECK(element);
  if (!element->InlineStyle())
    return;

  switch (properties_to_include) {
    case kAllProperties:
      MergeStyle(element->InlineStyle(), mode);
      return;
    case kOnlyEditingInheritableProperties:
      MergeStyle(CopyEditingProperties(element->InlineStyle(),
                                       kOnlyInheritableEditingProperties),
                 mode);
      return;
    case kEditingPropertiesInEffect:
      MergeStyle(
          CopyEditingProperties(element->InlineStyle(), kAllEditingProperties),
          mode);
      return;
  }
}

static inline bool ElementMatchesAndPropertyIsNotInInlineStyleDecl(
    const HTMLElementEquivalent* equivalent,
    const Element* element,
    EditingStyle::CSSPropertyOverrideMode mode,
    CSSPropertyValueSet* style) {
  return equivalent->Matches(element) &&
         (!element->InlineStyle() ||
          !equivalent->PropertyExistsInStyle(element->InlineStyle())) &&
         (mode == EditingStyle::kOverrideValues ||
          !equivalent->PropertyExistsInStyle(style));
}

static MutableCSSPropertyValueSet* ExtractEditingProperties(
    const CSSPropertyValueSet* style,
    EditingStyle::PropertiesToInclude properties_to_include) {
  if (!style)
    return nullptr;

  switch (properties_to_include) {
    case EditingStyle::kAllProperties:
    case EditingStyle::kEditingPropertiesInEffect:
      return CopyEditingProperties(style, kAllEditingProperties);
    case EditingStyle::kOnlyEditingInheritableProperties:
      return CopyEditingProperties(style, kOnlyInheritableEditingProperties);
  }

  NOTREACHED();
  return nullptr;
}

void EditingStyle::MergeInlineAndImplicitStyleOfElement(
    Element* element,
    CSSPropertyOverrideMode mode,
    PropertiesToInclude properties_to_include) {
  EditingStyle* style_from_rules = EditingStyle::Create();
  style_from_rules->MergeStyleFromRulesForSerialization(element);

  if (element->InlineStyle())
    style_from_rules->mutable_style_->MergeAndOverrideOnConflict(
        element->InlineStyle());

  style_from_rules->mutable_style_ = ExtractEditingProperties(
      style_from_rules->mutable_style_.Get(), properties_to_include);
  MergeStyle(style_from_rules->mutable_style_.Get(), mode);

  const HeapVector<Member<HTMLElementEquivalent>>& element_equivalents =
      HtmlElementEquivalents();
  for (const auto& equivalent : element_equivalents) {
    if (ElementMatchesAndPropertyIsNotInInlineStyleDecl(
            equivalent.Get(), element, mode, mutable_style_.Get()))
      equivalent->AddToStyle(element, this);
  }

  const HeapVector<Member<HTMLAttributeEquivalent>>& attribute_equivalents =
      HtmlAttributeEquivalents();
  for (const auto& attribute : attribute_equivalents) {
    if (attribute->AttributeName() == HTMLNames::dirAttr)
      continue;  // We don't want to include directionality
    if (ElementMatchesAndPropertyIsNotInInlineStyleDecl(
            attribute.Get(), element, mode, mutable_style_.Get()))
      attribute->AddToStyle(element, this);
  }
}

static const CSSValueList& MergeTextDecorationValues(
    const CSSValueList& merged_value,
    const CSSValueList& value_to_merge) {
  DEFINE_STATIC_LOCAL(Persistent<CSSIdentifierValue>, underline,
                      (CSSIdentifierValue::Create(CSSValueUnderline)));
  DEFINE_STATIC_LOCAL(Persistent<CSSIdentifierValue>, line_through,
                      (CSSIdentifierValue::Create(CSSValueLineThrough)));
  CSSValueList& result = *merged_value.Copy();
  if (value_to_merge.HasValue(*underline) && !merged_value.HasValue(*underline))
    result.Append(*underline);

  if (value_to_merge.HasValue(*line_through) &&
      !merged_value.HasValue(*line_through))
    result.Append(*line_through);

  return result;
}

void EditingStyle::MergeStyle(const CSSPropertyValueSet* style,
                              CSSPropertyOverrideMode mode) {
  if (!style)
    return;

  if (!mutable_style_) {
    mutable_style_ = style->MutableCopy();
    return;
  }

  unsigned property_count = style->PropertyCount();
  for (unsigned i = 0; i < property_count; ++i) {
    CSSPropertyValueSet::PropertyReference property = style->PropertyAt(i);
    const CSSValue* value = mutable_style_->GetPropertyCSSValue(property.Id());

    // text decorations never override values
    if ((property.Id() == CSSPropertyTextDecorationLine ||
         property.Id() == CSSPropertyWebkitTextDecorationsInEffect) &&
        property.Value().IsValueList() && value) {
      if (value->IsValueList()) {
        const CSSValueList& result = MergeTextDecorationValues(
            *ToCSSValueList(value), ToCSSValueList(property.Value()));
        mutable_style_->SetProperty(property.Id(), result,
                                    property.IsImportant());
        continue;
      }
      // text-decoration: none is equivalent to not having the property
      value = nullptr;
    }

    if (mode == kOverrideValues || (mode == kDoNotOverrideValues && !value)) {
      mutable_style_->SetProperty(
          CSSPropertyValue(property.PropertyMetadata(), property.Value()));
    }
  }
}

static MutableCSSPropertyValueSet* StyleFromMatchedRulesForElement(
    Element* element,
    unsigned rules_to_include) {
  MutableCSSPropertyValueSet* style =
      MutableCSSPropertyValueSet::Create(kHTMLQuirksMode);
  StyleRuleList* matched_rules =
      element->GetDocument().EnsureStyleResolver().StyleRulesForElement(
          element, rules_to_include);
  if (matched_rules) {
    for (unsigned i = 0; i < matched_rules->size(); ++i)
      style->MergeAndOverrideOnConflict(&matched_rules->at(i)->Properties());
  }
  return style;
}

void EditingStyle::MergeStyleFromRules(Element* element) {
  MutableCSSPropertyValueSet* style_from_matched_rules =
      StyleFromMatchedRulesForElement(
          element,
          StyleResolver::kAuthorCSSRules | StyleResolver::kCrossOriginCSSRules);
  // Styles from the inline style declaration, held in the variable "style",
  // take precedence over those from matched rules.
  if (mutable_style_)
    style_from_matched_rules->MergeAndOverrideOnConflict(mutable_style_.Get());

  Clear();
  mutable_style_ = style_from_matched_rules;
}

void EditingStyle::MergeStyleFromRulesForSerialization(Element* element) {
  MergeStyleFromRules(element);

  // The property value, if it's a percentage, may not reflect the actual
  // computed value.
  // For example: style="height: 1%; overflow: visible;" in quirksmode
  // FIXME: There are others like this, see <rdar://problem/5195123> Slashdot
  // copy/paste fidelity problem
  CSSComputedStyleDeclaration* computed_style_for_element =
      CSSComputedStyleDeclaration::Create(element);
  MutableCSSPropertyValueSet* from_computed_style =
      MutableCSSPropertyValueSet::Create(kHTMLQuirksMode);
  {
    unsigned property_count = mutable_style_->PropertyCount();
    for (unsigned i = 0; i < property_count; ++i) {
      CSSPropertyValueSet::PropertyReference property =
          mutable_style_->PropertyAt(i);
      const CSSProperty& css_property = property.Property();
      const CSSValue& value = property.Value();
      if (!value.IsPrimitiveValue())
        continue;
      if (ToCSSPrimitiveValue(value).IsPercentage()) {
        if (const CSSValue* computed_property_value =
                computed_style_for_element->GetPropertyCSSValue(css_property)) {
          from_computed_style->AddRespectingCascade(
              CSSPropertyValue(css_property, *computed_property_value));
        }
      }
    }
  }
  mutable_style_->MergeAndOverrideOnConflict(from_computed_style);
}

static void RemovePropertiesInStyle(
    MutableCSSPropertyValueSet* style_to_remove_properties_from,
    CSSPropertyValueSet* style) {
  unsigned property_count = style->PropertyCount();
  Vector<const CSSProperty*> properties_to_remove(property_count);
  for (unsigned i = 0; i < property_count; ++i)
    properties_to_remove[i] = &style->PropertyAt(i).Property();

  style_to_remove_properties_from->RemovePropertiesInSet(
      properties_to_remove.data(), properties_to_remove.size());
}

void EditingStyle::RemoveStyleFromRulesAndContext(Element* element,
                                                  ContainerNode* context) {
  DCHECK(element);
  if (!mutable_style_)
    return;

  // StyleResolver requires clean style.
  DCHECK_GE(element->GetDocument().Lifecycle().GetState(),
            DocumentLifecycle::kStyleClean);
  DCHECK(element->GetDocument().IsActive());

  SecureContextMode secure_context_mode =
      element->GetDocument().GetSecureContextMode();

  // 1. Remove style from matched rules because style remain without repeating
  // it in inline style declaration
  MutableCSSPropertyValueSet* style_from_matched_rules =
      StyleFromMatchedRulesForElement(element,
                                      StyleResolver::kAllButEmptyCSSRules);
  if (style_from_matched_rules && !style_from_matched_rules->IsEmpty()) {
    mutable_style_ = GetPropertiesNotIn(
        mutable_style_.Get(),
        style_from_matched_rules->EnsureCSSStyleDeclaration(),
        secure_context_mode);
  }

  // 2. Remove style present in context and not overriden by matched rules.
  EditingStyle* computed_style =
      EditingStyle::Create(context, kEditingPropertiesInEffect);
  if (computed_style->mutable_style_) {
    if (!computed_style->mutable_style_->GetPropertyCSSValue(
            CSSPropertyBackgroundColor))
      computed_style->mutable_style_->SetProperty(CSSPropertyBackgroundColor,
                                                  CSSValueTransparent);

    RemovePropertiesInStyle(computed_style->mutable_style_.Get(),
                            style_from_matched_rules);
    mutable_style_ = GetPropertiesNotIn(
        mutable_style_.Get(),
        computed_style->mutable_style_->EnsureCSSStyleDeclaration(),
        secure_context_mode);
  }

  // 3. If this element is a span and has display: inline or float: none, remove
  // them unless they are overriden by rules. These rules are added by
  // serialization code to wrap text nodes.
  if (IsStyleSpanOrSpanWithOnlyStyleAttribute(element)) {
    if (!style_from_matched_rules->GetPropertyCSSValue(CSSPropertyDisplay) &&
        GetIdentifierValue(mutable_style_.Get(), CSSPropertyDisplay) ==
            CSSValueInline)
      mutable_style_->RemoveProperty(CSSPropertyDisplay);
    if (!style_from_matched_rules->GetPropertyCSSValue(CSSPropertyFloat) &&
        GetIdentifierValue(mutable_style_.Get(), CSSPropertyFloat) ==
            CSSValueNone)
      mutable_style_->RemoveProperty(CSSPropertyFloat);
  }
}

void EditingStyle::RemovePropertiesInElementDefaultStyle(Element* element) {
  if (!mutable_style_ || mutable_style_->IsEmpty())
    return;

  CSSPropertyValueSet* default_style = StyleFromMatchedRulesForElement(
      element, StyleResolver::kUAAndUserCSSRules);

  RemovePropertiesInStyle(mutable_style_.Get(), default_style);
}

void EditingStyle::ForceInline() {
  if (!mutable_style_)
    mutable_style_ = MutableCSSPropertyValueSet::Create(kHTMLQuirksMode);
  const bool kPropertyIsImportant = true;
  mutable_style_->SetProperty(CSSPropertyDisplay, CSSValueInline,
                              kPropertyIsImportant);
}

int EditingStyle::LegacyFontSize(Document* document) const {
  const CSSValue* css_value =
      mutable_style_->GetPropertyCSSValue(CSSPropertyFontSize);
  if (!css_value ||
      !(css_value->IsPrimitiveValue() || css_value->IsIdentifierValue()))
    return 0;
  return LegacyFontSizeFromCSSValue(document, css_value, is_monospace_font_,
                                    kAlwaysUseLegacyFontSize);
}

void EditingStyle::Trace(blink::Visitor* visitor) {
  visitor->Trace(mutable_style_);
}

static void ReconcileTextDecorationProperties(
    MutableCSSPropertyValueSet* style,
    SecureContextMode secure_context_mode) {
  const CSSValue* text_decorations_in_effect =
      style->GetPropertyCSSValue(CSSPropertyWebkitTextDecorationsInEffect);
  const CSSValue* text_decoration =
      style->GetPropertyCSSValue(CSSPropertyTextDecorationLine);
  // "LayoutTests/editing/execCommand/insert-list-and-strikethrough.html" makes
  // both |textDecorationsInEffect| and |textDecoration| non-null.
  if (text_decorations_in_effect) {
    style->SetProperty(CSSPropertyTextDecorationLine,
                       text_decorations_in_effect->CssText(),
                       /* important */ false, secure_context_mode);
    style->RemoveProperty(CSSPropertyWebkitTextDecorationsInEffect);
    text_decoration = text_decorations_in_effect;
  }

  // If text-decoration is set to "none", remove the property because we don't
  // want to add redundant "text-decoration: none".
  if (text_decoration && !text_decoration->IsValueList())
    style->RemoveProperty(CSSPropertyTextDecorationLine);
}

StyleChange::StyleChange(EditingStyle* style, const Position& position)
    : apply_bold_(false),
      apply_italic_(false),
      apply_underline_(false),
      apply_line_through_(false),
      apply_subscript_(false),
      apply_superscript_(false) {
  Document* document = position.GetDocument();
  if (!style || !style->Style() || !document || !document->GetFrame() ||
      !AssociatedElementOf(position))
    return;

  CSSComputedStyleDeclaration* computed_style = EnsureComputedStyle(position);
  // FIXME: take care of background-color in effect
  MutableCSSPropertyValueSet* mutable_style = GetPropertiesNotIn(
      style->Style(), computed_style, document->GetSecureContextMode());
  DCHECK(mutable_style);

  ReconcileTextDecorationProperties(mutable_style,
                                    document->GetSecureContextMode());
  if (!document->GetFrame()->GetEditor().ShouldStyleWithCSS())
    ExtractTextStyles(document, mutable_style,
                      computed_style->IsMonospaceFont());

  // Changing the whitespace style in a tab span would collapse the tab into a
  // space.
  if (IsTabHTMLSpanElementTextNode(position.AnchorNode()) ||
      IsTabHTMLSpanElement((position.AnchorNode())))
    mutable_style->RemoveProperty(CSSPropertyWhiteSpace);

  // If unicode-bidi is present in mutableStyle and direction is not, then add
  // direction to mutableStyle.
  // FIXME: Shouldn't this be done in getPropertiesNotIn?
  if (mutable_style->GetPropertyCSSValue(CSSPropertyUnicodeBidi) &&
      !style->Style()->GetPropertyCSSValue(CSSPropertyDirection)) {
    mutable_style->SetProperty(
        CSSPropertyDirection,
        style->Style()->GetPropertyValue(CSSPropertyDirection),
        /* important */ false, document->GetSecureContextMode());
  }

  // Save the result for later
  css_style_ = mutable_style->AsText().StripWhiteSpace();
}

static void SetTextDecorationProperty(MutableCSSPropertyValueSet* style,
                                      const CSSValueList* new_text_decoration,
                                      CSSPropertyID property_id,
                                      SecureContextMode secure_context_mode) {
  if (new_text_decoration->length()) {
    style->SetProperty(property_id, new_text_decoration->CssText(),
                       style->PropertyIsImportant(property_id),
                       secure_context_mode);
  } else {
    // text-decoration: none is redundant since it does not remove any text
    // decorations.
    style->RemoveProperty(property_id);
  }
}

static bool GetPrimitiveValueNumber(CSSPropertyValueSet* style,
                                    CSSPropertyID property_id,
                                    float& number) {
  if (!style)
    return false;
  const CSSValue* value = style->GetPropertyCSSValue(property_id);
  if (!value || !value->IsPrimitiveValue())
    return false;
  number = ToCSSPrimitiveValue(value)->GetFloatValue();
  return true;
}

void StyleChange::ExtractTextStyles(Document* document,
                                    MutableCSSPropertyValueSet* style,
                                    bool is_monospace_font) {
  DCHECK(style);

  float weight = 0;
  bool is_number =
      GetPrimitiveValueNumber(style, CSSPropertyFontWeight, weight);
  if (GetIdentifierValue(style, CSSPropertyFontWeight) == CSSValueBold ||
      (is_number && weight >= BoldThreshold())) {
    style->RemoveProperty(CSSPropertyFontWeight);
    apply_bold_ = true;
  }

  int font_style = GetIdentifierValue(style, CSSPropertyFontStyle);
  if (font_style == CSSValueItalic || font_style == CSSValueOblique) {
    style->RemoveProperty(CSSPropertyFontStyle);
    apply_italic_ = true;
  }

  // Assuming reconcileTextDecorationProperties has been called, there should
  // not be -webkit-text-decorations-in-effect
  // Furthermore, text-decoration: none has been trimmed so that text-decoration
  // property is always a CSSValueList.
  const CSSValue* text_decoration =
      style->GetPropertyCSSValue(CSSPropertyTextDecorationLine);
  if (text_decoration && text_decoration->IsValueList()) {
    DEFINE_STATIC_LOCAL(Persistent<CSSIdentifierValue>, underline,
                        (CSSIdentifierValue::Create(CSSValueUnderline)));
    DEFINE_STATIC_LOCAL(Persistent<CSSIdentifierValue>, line_through,
                        (CSSIdentifierValue::Create(CSSValueLineThrough)));
    CSSValueList* new_text_decoration = ToCSSValueList(text_decoration)->Copy();
    if (new_text_decoration->RemoveAll(*underline))
      apply_underline_ = true;
    if (new_text_decoration->RemoveAll(*line_through))
      apply_line_through_ = true;

    // If trimTextDecorations, delete underline and line-through
    SetTextDecorationProperty(style, new_text_decoration,
                              CSSPropertyTextDecorationLine,
                              document->GetSecureContextMode());
  }

  int vertical_align = GetIdentifierValue(style, CSSPropertyVerticalAlign);
  switch (vertical_align) {
    case CSSValueSub:
      style->RemoveProperty(CSSPropertyVerticalAlign);
      apply_subscript_ = true;
      break;
    case CSSValueSuper:
      style->RemoveProperty(CSSPropertyVerticalAlign);
      apply_superscript_ = true;
      break;
  }

  if (style->GetPropertyCSSValue(CSSPropertyColor)) {
    apply_font_color_ = GetFontColor(style).Serialized();
    style->RemoveProperty(CSSPropertyColor);
  }

  apply_font_face_ = style->GetPropertyValue(CSSPropertyFontFamily);
  // Remove double quotes for Outlook 2007 compatibility. See
  // https://bugs.webkit.org/show_bug.cgi?id=79448
  apply_font_face_.Replace('"', "");
  style->RemoveProperty(CSSPropertyFontFamily);

  if (const CSSValue* font_size =
          style->GetPropertyCSSValue(CSSPropertyFontSize)) {
    if (!font_size->IsPrimitiveValue() && !font_size->IsIdentifierValue()) {
      // Can't make sense of the number. Put no font size.
      style->RemoveProperty(CSSPropertyFontSize);
    } else if (int legacy_font_size = LegacyFontSizeFromCSSValue(
                   document, font_size, is_monospace_font,
                   kUseLegacyFontSizeOnlyIfPixelValuesMatch)) {
      apply_font_size_ = String::Number(legacy_font_size);
      style->RemoveProperty(CSSPropertyFontSize);
    }
  }
}

static void DiffTextDecorations(MutableCSSPropertyValueSet* style,
                                CSSPropertyID property_id,
                                const CSSValue* ref_text_decoration,
                                SecureContextMode secure_context_mode) {
  const CSSValue* text_decoration = style->GetPropertyCSSValue(property_id);
  if (!text_decoration || !text_decoration->IsValueList() ||
      !ref_text_decoration || !ref_text_decoration->IsValueList())
    return;

  CSSValueList* new_text_decoration = ToCSSValueList(text_decoration)->Copy();
  const CSSValueList* values_in_ref_text_decoration =
      ToCSSValueList(ref_text_decoration);

  for (wtf_size_t i = 0; i < values_in_ref_text_decoration->length(); i++)
    new_text_decoration->RemoveAll(values_in_ref_text_decoration->Item(i));

  SetTextDecorationProperty(style, new_text_decoration, property_id,
                            secure_context_mode);
}

static bool FontWeightIsBold(const CSSValue* font_weight) {
  if (font_weight->IsIdentifierValue()) {
    // Because b tag can only bold text, there are only two states in plain
    // html: bold and not bold. Collapse all other values to either one of these
    // two states for editing purposes.

    switch (ToCSSIdentifierValue(font_weight)->GetValueID()) {
      case CSSValueNormal:
        return false;
      case CSSValueBold:
        return true;
      default:
        break;
    }
  }

  CHECK(font_weight->IsPrimitiveValue());
  CHECK(ToCSSPrimitiveValue(font_weight)->IsNumber());
  return ToCSSPrimitiveValue(font_weight)->GetFloatValue() >= BoldThreshold();
}

static bool FontWeightNeedsResolving(const CSSValue* font_weight) {
  if (font_weight->IsPrimitiveValue())
    return false;
  if (!font_weight->IsIdentifierValue())
    return true;
  const CSSValueID value = ToCSSIdentifierValue(font_weight)->GetValueID();
  return value == CSSValueLighter || value == CSSValueBolder;
}

MutableCSSPropertyValueSet* GetPropertiesNotIn(
    CSSPropertyValueSet* style_with_redundant_properties,
    CSSStyleDeclaration* base_style,
    SecureContextMode secure_context_mode) {
  DCHECK(style_with_redundant_properties);
  DCHECK(base_style);
  MutableCSSPropertyValueSet* result =
      style_with_redundant_properties->MutableCopy();

  result->RemoveEquivalentProperties(base_style);

  const CSSValue* base_text_decorations_in_effect =
      base_style->GetPropertyCSSValueInternal(
          CSSPropertyWebkitTextDecorationsInEffect);
  DiffTextDecorations(result, CSSPropertyTextDecorationLine,
                      base_text_decorations_in_effect, secure_context_mode);
  DiffTextDecorations(result, CSSPropertyWebkitTextDecorationsInEffect,
                      base_text_decorations_in_effect, secure_context_mode);

  if (const CSSValue* base_font_weight =
          base_style->GetPropertyCSSValueInternal(CSSPropertyFontWeight)) {
    if (const CSSValue* font_weight =
            result->GetPropertyCSSValue(CSSPropertyFontWeight)) {
      if (!FontWeightNeedsResolving(font_weight) &&
          !FontWeightNeedsResolving(base_font_weight) &&
          (FontWeightIsBold(font_weight) == FontWeightIsBold(base_font_weight)))
        result->RemoveProperty(CSSPropertyFontWeight);
    }
  }

  if (base_style->GetPropertyCSSValueInternal(CSSPropertyColor) &&
      GetFontColor(result) == GetFontColor(base_style))
    result->RemoveProperty(CSSPropertyColor);

  if (base_style->GetPropertyCSSValueInternal(CSSPropertyTextAlign) &&
      TextAlignResolvingStartAndEnd(result) ==
          TextAlignResolvingStartAndEnd(base_style))
    result->RemoveProperty(CSSPropertyTextAlign);

  if (base_style->GetPropertyCSSValueInternal(CSSPropertyBackgroundColor) &&
      GetBackgroundColor(result) == GetBackgroundColor(base_style))
    result->RemoveProperty(CSSPropertyBackgroundColor);

  return result;
}

CSSValueID GetIdentifierValue(CSSPropertyValueSet* style,
                              CSSPropertyID property_id) {
  if (!style)
    return CSSValueInvalid;
  const CSSValue* value = style->GetPropertyCSSValue(property_id);
  if (!value || !value->IsIdentifierValue())
    return CSSValueInvalid;
  return ToCSSIdentifierValue(value)->GetValueID();
}

CSSValueID GetIdentifierValue(CSSStyleDeclaration* style,
                              CSSPropertyID property_id) {
  if (!style)
    return CSSValueInvalid;
  const CSSValue* value = style->GetPropertyCSSValueInternal(property_id);
  if (!value || !value->IsIdentifierValue())
    return CSSValueInvalid;
  return ToCSSIdentifierValue(value)->GetValueID();
}

int LegacyFontSizeFromCSSValue(Document* document,
                               const CSSValue* value,
                               bool is_monospace_font,
                               LegacyFontSizeMode mode) {
  if (value->IsPrimitiveValue()) {
    const CSSPrimitiveValue& primitive_value = ToCSSPrimitiveValue(*value);
    CSSPrimitiveValue::LengthUnitType length_type;
    if (CSSPrimitiveValue::UnitTypeToLengthUnitType(
            primitive_value.TypeWithCalcResolved(), length_type) &&
        length_type == CSSPrimitiveValue::kUnitTypePixels) {
      double conversion =
          CSSPrimitiveValue::ConversionToCanonicalUnitsScaleFactor(
              primitive_value.TypeWithCalcResolved());
      int pixel_font_size =
          clampTo<int>(primitive_value.GetDoubleValue() * conversion);
      int legacy_font_size = FontSizeFunctions::LegacyFontSize(
          document, pixel_font_size, is_monospace_font);
      // Use legacy font size only if pixel value matches exactly to that of
      // legacy font size.
      if (mode == kAlwaysUseLegacyFontSize ||
          FontSizeFunctions::FontSizeForKeyword(
              document, legacy_font_size, is_monospace_font) == pixel_font_size)
        return legacy_font_size;

      return 0;
    }
  }

  if (value->IsIdentifierValue()) {
    const CSSIdentifierValue& identifier_value = ToCSSIdentifierValue(*value);
    if (CSSValueXSmall <= identifier_value.GetValueID() &&
        identifier_value.GetValueID() <= CSSValueWebkitXxxLarge)
      return identifier_value.GetValueID() - CSSValueXSmall + 1;
  }

  return 0;
}

EditingTriState EditingStyle::SelectionHasStyle(const LocalFrame& frame,
                                                CSSPropertyID property_id,
                                                const String& value) {
  const SecureContextMode secure_context_mode =
      frame.GetDocument()->GetSecureContextMode();

  return Create(property_id, value, secure_context_mode)
      ->TriStateOfStyle(
          frame.Selection().ComputeVisibleSelectionInDOMTreeDeprecated(),
          secure_context_mode);
}

}  // namespace blink
