/*
 * Copyright (C) 2004, 2005, 2006, 2008 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009 Rob Buis <buis@kde.org>
 * Copyright (C) 2006 Alexander Kellett <lypanov@kde.org>
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

#include "third_party/blink/renderer/core/svg/svg_image_element.h"

#include "third_party/blink/renderer/core/css/style_change_reason.h"
#include "third_party/blink/renderer/core/css_property_names.h"
#include "third_party/blink/renderer/core/frame/use_counter.h"
#include "third_party/blink/renderer/core/html/media/media_element_parser_helpers.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/layout/layout_image_resource.h"
#include "third_party/blink/renderer/core/layout/layout_replaced.h"
#include "third_party/blink/renderer/core/layout/svg/layout_svg_image.h"
#include "third_party/blink/renderer/core/svg_names.h"

namespace blink {

inline SVGImageElement::SVGImageElement(Document& document)
    : SVGGraphicsElement(SVGNames::imageTag, document),
      SVGURIReference(this),
      is_default_overridden_intrinsic_size_(false),
      x_(SVGAnimatedLength::Create(this,
                                   SVGNames::xAttr,
                                   SVGLengthMode::kWidth,
                                   SVGLength::Initial::kUnitlessZero,
                                   CSSPropertyX)),
      y_(SVGAnimatedLength::Create(this,
                                   SVGNames::yAttr,
                                   SVGLengthMode::kHeight,
                                   SVGLength::Initial::kUnitlessZero,
                                   CSSPropertyY)),
      width_(SVGAnimatedLength::Create(this,
                                       SVGNames::widthAttr,
                                       SVGLengthMode::kWidth,
                                       SVGLength::Initial::kUnitlessZero,
                                       CSSPropertyWidth)),
      height_(SVGAnimatedLength::Create(this,
                                        SVGNames::heightAttr,
                                        SVGLengthMode::kHeight,
                                        SVGLength::Initial::kUnitlessZero,
                                        CSSPropertyHeight)),
      preserve_aspect_ratio_(SVGAnimatedPreserveAspectRatio::Create(
          this,
          SVGNames::preserveAspectRatioAttr)),
      image_loader_(SVGImageLoader::Create(this)) {
  AddToPropertyMap(x_);
  AddToPropertyMap(y_);
  AddToPropertyMap(width_);
  AddToPropertyMap(height_);
  AddToPropertyMap(preserve_aspect_ratio_);

  if (MediaElementParserHelpers::IsMediaElement(this) &&
      !MediaElementParserHelpers::IsUnsizedMediaEnabled(document)) {
    is_default_overridden_intrinsic_size_ = true;
    overridden_intrinsic_size_ =
        IntSize(LayoutReplaced::kDefaultWidth, LayoutReplaced::kDefaultHeight);
  }
}

DEFINE_NODE_FACTORY(SVGImageElement)

void SVGImageElement::Trace(blink::Visitor* visitor) {
  visitor->Trace(x_);
  visitor->Trace(y_);
  visitor->Trace(width_);
  visitor->Trace(height_);
  visitor->Trace(preserve_aspect_ratio_);
  visitor->Trace(image_loader_);
  SVGGraphicsElement::Trace(visitor);
  SVGURIReference::Trace(visitor);
}

bool SVGImageElement::CurrentFrameHasSingleSecurityOrigin() const {
  if (LayoutSVGImage* layout_svg_image = ToLayoutSVGImage(GetLayoutObject())) {
    LayoutImageResource* layout_image_resource =
        layout_svg_image->ImageResource();
    ImageResourceContent* image_content = layout_image_resource->CachedImage();
    if (image_content) {
      if (Image* image = image_content->GetImage())
        return image->CurrentFrameHasSingleSecurityOrigin();
    }
  }
  return true;
}

ScriptPromise SVGImageElement::decode(ScriptState* script_state,
                                      ExceptionState& exception_state) {
  return GetImageLoader().Decode(script_state, exception_state);
}

void SVGImageElement::CollectStyleForPresentationAttribute(
    const QualifiedName& name,
    const AtomicString& value,
    MutableCSSPropertyValueSet* style) {
  SVGAnimatedPropertyBase* property = PropertyFromAttribute(name);
  if (property == width_) {
    AddPropertyToPresentationAttributeStyle(style, property->CssPropertyId(),
                                            width_->CssValue());
  } else if (property == height_) {
    AddPropertyToPresentationAttributeStyle(style, property->CssPropertyId(),
                                            height_->CssValue());
  } else if (property == x_) {
    AddPropertyToPresentationAttributeStyle(style, property->CssPropertyId(),
                                            x_->CssValue());
  } else if (property == y_) {
    AddPropertyToPresentationAttributeStyle(style, property->CssPropertyId(),
                                            y_->CssValue());
  } else {
    SVGGraphicsElement::CollectStyleForPresentationAttribute(name, value,
                                                             style);
  }
}

void SVGImageElement::SvgAttributeChanged(const QualifiedName& attr_name) {
  bool is_length_attribute =
      attr_name == SVGNames::xAttr || attr_name == SVGNames::yAttr ||
      attr_name == SVGNames::widthAttr || attr_name == SVGNames::heightAttr;

  if (is_length_attribute || attr_name == SVGNames::preserveAspectRatioAttr) {
    SVGElement::InvalidationGuard invalidation_guard(this);

    if (is_length_attribute) {
      InvalidateSVGPresentationAttributeStyle();
      SetNeedsStyleRecalc(
          kLocalStyleChange,
          StyleChangeReasonForTracing::FromAttribute(attr_name));
      UpdateRelativeLengthsInformation();
    }

    LayoutObject* object = GetLayoutObject();
    if (!object)
      return;

    // FIXME: if isLengthAttribute then we should avoid this call if the
    // viewport didn't change, however since we don't have the computed
    // style yet we can't use updateBoundingBox/updateImageContainerSize.
    // See http://crbug.com/466200.
    MarkForLayoutAndParentResourceInvalidation(*object);
    return;
  }

  if (SVGURIReference::IsKnownAttribute(attr_name)) {
    SVGElement::InvalidationGuard invalidation_guard(this);
    GetImageLoader().UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
    return;
  }

  SVGGraphicsElement::SvgAttributeChanged(attr_name);
}

void SVGImageElement::ParseAttribute(
    const AttributeModificationParams& params) {
  if (params.name == SVGNames::decodingAttr) {
    UseCounter::Count(GetDocument(), WebFeature::kImageDecodingAttribute);
    decoding_mode_ = ParseImageDecodingMode(params.new_value);
  } else if (params.name == SVGNames::intrinsicsizeAttr &&
             RuntimeEnabledFeatures::
                 ExperimentalProductivityFeaturesEnabled()) {
    String message;
    bool intrinsic_size_changed =
        MediaElementParserHelpers::ParseIntrinsicSizeAttribute(
            params.new_value, this, &overridden_intrinsic_size_,
            &is_default_overridden_intrinsic_size_, &message);
    if (!message.IsEmpty()) {
      GetDocument().AddConsoleMessage(ConsoleMessage::Create(
          kOtherMessageSource, kWarningMessageLevel, message));
    }

    if (intrinsic_size_changed) {
      if (LayoutSVGImage* layout_obj = ToLayoutSVGImage(GetLayoutObject()))
        MarkForLayoutAndParentResourceInvalidation(*layout_obj);
    }
  } else {
    SVGElement::ParseAttribute(params);
  }
}

bool SVGImageElement::SelfHasRelativeLengths() const {
  return x_->CurrentValue()->IsRelative() || y_->CurrentValue()->IsRelative() ||
         width_->CurrentValue()->IsRelative() ||
         height_->CurrentValue()->IsRelative();
}

LayoutObject* SVGImageElement::CreateLayoutObject(const ComputedStyle&) {
  return new LayoutSVGImage(this);
}

bool SVGImageElement::HaveLoadedRequiredResources() {
  return !GetImageLoader().HasPendingActivity();
}

void SVGImageElement::AttachLayoutTree(AttachContext& context) {
  SVGGraphicsElement::AttachLayoutTree(context);

  if (LayoutSVGImage* image_obj = ToLayoutSVGImage(GetLayoutObject())) {
    LayoutImageResource* layout_image_resource = image_obj->ImageResource();
    if (layout_image_resource->HasImage())
      return;
    layout_image_resource->SetImageResource(GetImageLoader().GetContent());
  }
}

Node::InsertionNotificationRequest SVGImageElement::InsertedInto(
    ContainerNode& root_parent) {
  // A previous loader update may have failed to actually fetch the image if
  // the document was inactive. In that case, force a re-update (but don't
  // clear previous errors).
  if (root_parent.isConnected() && !GetImageLoader().GetContent())
    GetImageLoader().UpdateFromElement(ImageLoader::kUpdateNormal);

  return SVGGraphicsElement::InsertedInto(root_parent);
}

const AtomicString SVGImageElement::ImageSourceURL() const {
  return AtomicString(HrefString());
}

void SVGImageElement::DidMoveToNewDocument(Document& old_document) {
  GetImageLoader().UpdateFromElement(ImageLoader::kUpdateIgnorePreviousError);
  GetImageLoader().ElementDidMoveToNewDocument();
  SVGGraphicsElement::DidMoveToNewDocument(old_document);
}

}  // namespace blink
