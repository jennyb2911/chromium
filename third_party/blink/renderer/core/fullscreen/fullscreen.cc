/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 *           (C) 2006 Alexey Proskuryakov (ap@webkit.org)
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2012 Apple Inc. All
 * rights reserved.
 * Copyright (C) 2008, 2009 Torch Mobile Inc. All rights reserved.
 * (http://www.torchmobile.com/)
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2013 Google Inc. All rights reserved.
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
 *
 */

#include "third_party/blink/renderer/core/fullscreen/fullscreen.h"

#include "base/macros.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/css/style_change_reason.h"
#include "third_party/blink/renderer/core/css/style_engine.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element_traversal.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/dom/shadow_root.h"
#include "third_party/blink/renderer/core/frame/hosts_using_features.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/frame/use_counter.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/fullscreen/fullscreen_options.h"
#include "third_party/blink/renderer/core/fullscreen/scoped_allow_fullscreen.h"
#include "third_party/blink/renderer/core/html/html_iframe_element.h"
#include "third_party/blink/renderer/core/html_element_type_helpers.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/svg/svg_svg_element.h"
#include "third_party/blink/renderer/platform/bindings/microtask.h"
#include "third_party/blink/renderer/platform/feature_policy/feature_policy.h"

namespace blink {

namespace {

void FullscreenElementChanged(Document& document,
                              Element* old_element,
                              Element* new_element,
                              Fullscreen::RequestType new_request_type) {
  DCHECK_NE(old_element, new_element);

  document.GetStyleEngine().EnsureUAStyleForFullscreen();

  if (old_element) {
    DCHECK_NE(old_element, Fullscreen::FullscreenElementFrom(document));

    old_element->PseudoStateChanged(CSSSelector::kPseudoFullScreen);

    old_element->SetContainsFullScreenElement(false);
    old_element->SetContainsFullScreenElementOnAncestorsCrossingFrameBoundaries(
        false);
  }

  if (new_element) {
    DCHECK_EQ(new_element, Fullscreen::FullscreenElementFrom(document));

    new_element->PseudoStateChanged(CSSSelector::kPseudoFullScreen);

    // OOPIF: For RequestType::PrefixedForCrossProcessDescendant, |new_element|
    // is the iframe element for the out-of-process frame that contains the
    // fullscreen element. Hence, it must match :-webkit-full-screen-ancestor.
    if (new_request_type ==
        Fullscreen::RequestType::kPrefixedForCrossProcessDescendant) {
      DCHECK(IsHTMLIFrameElement(new_element));
      new_element->SetContainsFullScreenElement(true);
    }
    new_element->SetContainsFullScreenElementOnAncestorsCrossingFrameBoundaries(
        true);
  }

  if (document.GetFrame()) {
    // SetIsInert recurses through subframes to propagate the inert bit as
    // needed.
    document.GetFrame()->SetIsInert(document.LocalOwner() &&
                                    document.LocalOwner()->IsInert());
  }

  // Any element not contained by the fullscreen element is inert (see
  // |Node::IsInert()|), so changing the fullscreen element will typically
  // change the inertness of most elements. Clear the entire cache.
  document.ClearAXObjectCache();

  if (LocalFrame* frame = document.GetFrame()) {
    // TODO(foolip): Synchronize hover state changes with animation frames.
    // https://crbug.com/668758
    frame->GetEventHandler().ScheduleHoverStateUpdate();
    frame->GetChromeClient().FullscreenElementChanged(old_element, new_element);

    // Update paint properties on the visual viewport since
    // user-input-scrollable bits will change based on fullscreen state.
    if (Page* page = frame->GetPage())
      page->GetVisualViewport().SetNeedsPaintPropertiesUpdate();
  }
}

using ElementRequestTypeMap =
    HeapHashMap<WeakMember<Element>, Fullscreen::RequestType>;

ElementRequestTypeMap& FullscreenFlagMap() {
  DEFINE_STATIC_LOCAL(Persistent<ElementRequestTypeMap>, map,
                      (new ElementRequestTypeMap));
  return *map;
}

bool HasFullscreenFlag(Element& element) {
  return FullscreenFlagMap().Contains(&element);
}

void SetFullscreenFlag(Element& element, Fullscreen::RequestType request_type) {
  FullscreenFlagMap().insert(&element, request_type);
}

void UnsetFullscreenFlag(Element& element) {
  FullscreenFlagMap().erase(&element);
}

Fullscreen::RequestType GetRequestType(Element& element) {
  return FullscreenFlagMap().find(&element)->value;
}

// https://fullscreen.spec.whatwg.org/#fullscreen-an-element
void GoFullscreen(Element& element, Fullscreen::RequestType request_type) {
  Document& document = element.GetDocument();
  Element* old_element = Fullscreen::FullscreenElementFrom(document);

  // If |element| is already in top layer remove it so it will
  // be appended to the end.
  if (element.IsInTopLayer())
    document.RemoveFromTopLayer(&element);
  else
    DCHECK(!HasFullscreenFlag(element));

  // To fullscreen an |element| within a |document|, set the |element|'s
  // fullscreen flag and add it to |document|'s top layer.
  SetFullscreenFlag(element, request_type);
  document.AddToTopLayer(&element);

  DCHECK_EQ(&element, Fullscreen::FullscreenElementFrom(document));
  FullscreenElementChanged(document, old_element, &element, request_type);
}

// https://fullscreen.spec.whatwg.org/#unfullscreen-an-element
void Unfullscreen(Element& element) {
  Document& document = element.GetDocument();
  Element* old_element = Fullscreen::FullscreenElementFrom(document);

  // To unfullscreen an |element| within a |document|, unset the element's
  // fullscreen flag and iframe fullscreen flag (if any), and remove it from
  // |document|'s top layer.
  DCHECK(element.IsInTopLayer());
  DCHECK(HasFullscreenFlag(element));
  UnsetFullscreenFlag(element);
  document.RemoveFromTopLayer(&element);

  Element* new_element = Fullscreen::FullscreenElementFrom(document);
  if (old_element != new_element) {
    Fullscreen::RequestType new_request_type =
        new_element ? GetRequestType(*new_element)
                    : Fullscreen::RequestType::kUnprefixed;
    FullscreenElementChanged(document, old_element, new_element,
                             new_request_type);
  }
}

// https://fullscreen.spec.whatwg.org/#unfullscreen-a-document
void Unfullscreen(Document& document) {
  // To unfullscreen a |document|, unfullscreen all elements, within
  // |document|'s top layer, whose fullscreen flag is set.

  HeapVector<Member<Element>> fullscreen_elements;
  for (Element* element : document.TopLayerElements()) {
    if (HasFullscreenFlag(*element))
      fullscreen_elements.push_back(element);
  }

  for (Element* element : fullscreen_elements)
    Unfullscreen(*element);
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#allowed-to-use
bool AllowedToUseFullscreen(const Frame* frame,
                            ReportOptions report_on_failure) {
  // To determine whether a Document object |document| is allowed to use the
  // feature indicated by attribute name |allowattribute|, run these steps:

  // 1. If |document| has no browsing context, then return false.
  if (!frame)
    return false;

  // 2. If Feature Policy is enabled, return the policy for "fullscreen"
  // feature.
  return frame->DeprecatedIsFeatureEnabled(
      mojom::FeaturePolicyFeature::kFullscreen, report_on_failure);
}

bool AllowedToRequestFullscreen(Document& document) {
  // An algorithm is allowed to request fullscreen if one of the following is
  // true:

  //  The algorithm is triggered by a user activation.
  if (Frame::HasTransientUserActivation(document.GetFrame()))
    return true;

  //  The algorithm is triggered by a user generated orientation change.
  if (ScopedAllowFullscreen::FullscreenAllowedReason() ==
      ScopedAllowFullscreen::kOrientationChange) {
    UseCounter::Count(document,
                      WebFeature::kFullscreenAllowedByOrientationChange);
    return true;
  }

  String message = ExceptionMessages::FailedToExecute(
      "requestFullscreen", "Element",
      "API can only be initiated by a user gesture.");
  document.AddConsoleMessage(
      ConsoleMessage::Create(kJSMessageSource, kWarningMessageLevel, message));

  return false;
}

// https://fullscreen.spec.whatwg.org/#fullscreen-is-supported
bool FullscreenIsSupported(const Document& document) {
  LocalFrame* frame = document.GetFrame();
  if (!frame)
    return false;

  // Fullscreen is supported if there is no previously-established user
  // preference, security risk, or platform limitation.
  return !document.GetSettings() ||
         document.GetSettings()->GetFullscreenSupported();
}

// https://fullscreen.spec.whatwg.org/#fullscreen-element-ready-check
bool FullscreenElementReady(const Element& element,
                            ReportOptions report_on_failure) {
  // A fullscreen element ready check for an element |element| returns true if
  // all of the following are true, and false otherwise:

  // |element| is in a document.
  if (!element.isConnected())
    return false;

  // |element|'s node document is allowed to use the feature indicated by
  // attribute name allowfullscreen.
  if (!AllowedToUseFullscreen(element.GetDocument().GetFrame(),
                              report_on_failure))
    return false;

  return true;
}

// https://fullscreen.spec.whatwg.org/#dom-element-requestfullscreen step 4:
bool RequestFullscreenConditionsMet(Element& pending, Document& document) {
  // |pending|'s namespace is the HTML namespace or |pending| is an SVG svg or
  // MathML math element. Note: MathML is not supported.
  if (!pending.IsHTMLElement() && !IsSVGSVGElement(pending))
    return false;

  // |pending| is not a dialog element.
  if (IsHTMLDialogElement(pending))
    return false;

  // The fullscreen element ready check for |pending| returns false.
  if (!FullscreenElementReady(pending, ReportOptions::kReportOnFailure))
    return false;

  // Fullscreen is supported.
  if (!FullscreenIsSupported(document))
    return false;

  // This algorithm is allowed to request fullscreen.
  if (!AllowedToRequestFullscreen(document))
    return false;

  return true;
}

// RequestFullscreenScope is allocated at the top of |RequestFullscreen()| and
// used to avoid synchronously changing any state within that method, by
// deferring changes in |DidEnterFullscreen()|.
class RequestFullscreenScope {
  STACK_ALLOCATED();

 public:
  RequestFullscreenScope() {
    DCHECK(!running_request_fullscreen_);
    running_request_fullscreen_ = true;
  }

  ~RequestFullscreenScope() {
    DCHECK(running_request_fullscreen_);
    running_request_fullscreen_ = false;
  }

  static bool RunningRequestFullscreen() { return running_request_fullscreen_; }

 private:
  static bool running_request_fullscreen_;
  DISALLOW_COPY_AND_ASSIGN(RequestFullscreenScope);
};

bool RequestFullscreenScope::running_request_fullscreen_ = false;

// Walks the frame tree and returns the first local ancestor frame, if any.
LocalFrame* NextLocalAncestor(Frame& frame) {
  Frame* parent = frame.Tree().Parent();
  if (!parent)
    return nullptr;
  if (parent->IsLocalFrame())
    return ToLocalFrame(parent);
  return NextLocalAncestor(*parent);
}

// Walks the document's frame tree and returns the document of the first local
// ancestor frame, if any.
Document* NextLocalAncestor(Document& document) {
  LocalFrame* frame = document.GetFrame();
  if (!frame)
    return nullptr;
  LocalFrame* next = NextLocalAncestor(*frame);
  if (!next)
    return nullptr;
  DCHECK(next->GetDocument());
  return next->GetDocument();
}

// Helper to walk the ancestor chain and return the Document of the topmost
// local ancestor frame. Note that this is not the same as the topmost frame's
// Document, which might be unavailable in OOPIF scenarios. For example, with
// OOPIFs, when called on the bottom frame's Document in a A-B-C-B hierarchy in
// process B, this will skip remote frame C and return this frame: A-[B]-C-B.
Document& TopmostLocalAncestor(Document& document) {
  if (Document* next = NextLocalAncestor(document))
    return TopmostLocalAncestor(*next);
  return document;
}

size_t CountFullscreenInTopLayer(const Document& document) {
  size_t count = 0;
  for (Element* element : document.TopLayerElements()) {
    if (HasFullscreenFlag(*element))
      ++count;
  }
  return count;
}

// https://fullscreen.spec.whatwg.org/#simple-fullscreen-document
bool IsSimpleFullscreenDocument(const Document& document) {
  return CountFullscreenInTopLayer(document) == 1;
}

// https://fullscreen.spec.whatwg.org/#collect-documents-to-unfullscreen
HeapVector<Member<Document>> CollectDocumentsToUnfullscreen(Document& doc) {
  // 1. Let |docs| be an ordered set consisting of |doc|.
  HeapVector<Member<Document>> docs;
  docs.push_back(&doc);

  // 2. While true:
  for (Document* lastDoc = &doc;;) {
    // 2.1. Let |lastDoc| be |docs|'s last document.

    // 2.2. Assert: |lastDoc|'s fullscreen element is not null.
    DCHECK(Fullscreen::FullscreenElementFrom(*lastDoc));

    // 2.3. If |lastDoc| is not a simple fullscreen document, break.
    if (!IsSimpleFullscreenDocument(*lastDoc))
      break;

    // 2.4. Let |container| be |lastDoc|'s browsing context container, if any,
    // and otherwise break.
    //
    // OOPIF: Skip over remote frames, assuming that they have exactly one
    // element in their fullscreen element stacks, thereby erring on the side of
    // exiting fullscreen. TODO(alexmos): Deal with nested fullscreen cases, see
    // https://crbug.com/617369.
    lastDoc = NextLocalAncestor(*lastDoc);
    if (!lastDoc)
      break;

    // 2.5. If |container|'s iframe fullscreen flag is set, break.
    // TODO(foolip): Support the iframe fullscreen flag.
    // https://crbug.com/644695

    // 2.6. Append |container|'s node document to |docs|.
    docs.push_back(lastDoc);
  }

  // 3. Return |docs|.
  return docs;
}

// https://fullscreen.spec.whatwg.org/#run-the-fullscreen-rendering-steps
void FireEvent(const AtomicString& type, Element* element, Document* document) {
  DCHECK(document);
  DCHECK(element);

  // |Document::EnqueueAnimationFrameTask()| is used instead of a "list of
  // pending fullscreen events", so only the body of the "run the fullscreen
  // rendering steps" loop appears here:

  // 3.1. Let |target| be |element| if |element| is connected and its node
  // document is |document|, and otherwise let |target| be |document|.
  EventTarget* target =
      element->isConnected() && &element->GetDocument() == document
          ? static_cast<EventTarget*>(element)
          : static_cast<EventTarget*>(document);

  // 3.2. Fire an event named |type|, with its bubbles and composed attributes
  // set to true, at |target|.
  Event* event = Event::CreateBubble(type);
  event->SetComposed(true);
  target->DispatchEvent(*event);
}

const AtomicString& AdjustEventType(const AtomicString& type,
                                    Fullscreen::RequestType request_type) {
  DCHECK(type == EventTypeNames::fullscreenchange ||
         type == EventTypeNames::fullscreenerror);

  if (request_type == Fullscreen::RequestType::kUnprefixed)
    return type;
  return type == EventTypeNames::fullscreenchange
             ? EventTypeNames::webkitfullscreenchange
             : EventTypeNames::webkitfullscreenerror;
}

void EnqueueEvent(const AtomicString& type,
                  Element& element,
                  Document& document,
                  Fullscreen::RequestType request_type) {
  const AtomicString& adjusted_type = AdjustEventType(type, request_type);
  document.EnqueueAnimationFrameTask(WTF::Bind(FireEvent, adjusted_type,
                                               WrapPersistent(&element),
                                               WrapPersistent(&document)));
}

void DidEnterFullscreenTask(Document* document) {
  DCHECK(document);
  Fullscreen::DidEnterFullscreen(*document);
}

}  // anonymous namespace

const char Fullscreen::kSupplementName[] = "Fullscreen";

Fullscreen& Fullscreen::From(Document& document) {
  Fullscreen* fullscreen = FromIfExists(document);
  if (!fullscreen) {
    fullscreen = new Fullscreen(document);
    ProvideTo(document, fullscreen);
  }
  return *fullscreen;
}

Fullscreen* Fullscreen::FromIfExists(Document& document) {
  if (!document.HasFullscreenSupplement())
    return nullptr;
  return Supplement<Document>::From<Fullscreen>(document);
}

Element* Fullscreen::FullscreenElementFrom(Document& document) {
  // The fullscreen element is the topmost element in the document's top layer
  // whose fullscreen flag is set, if any, and null otherwise.

  const auto& elements = document.TopLayerElements();
  for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
    Element* element = (*it).Get();
    if (HasFullscreenFlag(*element))
      return element;
  }

  return nullptr;
}

// https://fullscreen.spec.whatwg.org/#fullscreen-element
Element* Fullscreen::FullscreenElementForBindingFrom(TreeScope& scope) {
  Element* element = FullscreenElementFrom(scope.GetDocument());
  if (!element || !RuntimeEnabledFeatures::FullscreenUnprefixedEnabled())
    return element;

  // TODO(kochi): Once V0 code is removed, we can use the same logic for
  // Document and ShadowRoot.
  if (!scope.RootNode().IsShadowRoot()) {
    // For Shadow DOM V0 compatibility: We allow returning an element in V0
    // shadow tree, even though it leaks the Shadow DOM.
    if (element->IsInV0ShadowTree()) {
      UseCounter::Count(scope.GetDocument(),
                        WebFeature::kDocumentFullscreenElementInV0Shadow);
      return element;
    }
  } else if (!ToShadowRoot(scope.RootNode()).IsV1()) {
    return nullptr;
  }
  return scope.AdjustedElement(*element);
}

bool Fullscreen::IsInFullscreenElementStack(const Element& element) {
  return HasFullscreenFlag(const_cast<Element&>(element));
}

Fullscreen::Fullscreen(Document& document)
    : Supplement<Document>(document), ContextLifecycleObserver(&document) {
  document.SetHasFullscreenSupplement();
}

Fullscreen::~Fullscreen() = default;

Document* Fullscreen::GetDocument() {
  return ToDocument(LifecycleContext());
}

void Fullscreen::ContextDestroyed(ExecutionContext*) {
  pending_requests_.clear();
  pending_exits_.clear();
}

// https://fullscreen.spec.whatwg.org/#dom-element-requestfullscreen
void Fullscreen::RequestFullscreen(Element& pending) {
  // TODO(foolip): Make RequestType::Unprefixed the default when the unprefixed
  // API is enabled. https://crbug.com/383813
  FullscreenOptions options;
  options.setNavigationUI("hide");
  RequestFullscreen(pending, options, RequestType::kPrefixed);
}

ScriptPromise Fullscreen::RequestFullscreen(Element& pending,
                                            const FullscreenOptions& options,
                                            RequestType request_type,
                                            ScriptState* script_state) {
  RequestFullscreenScope scope;

  // 1. Let |pending| be the context object.

  // 2. Let |pendingDoc| be |pending|'s node document.
  Document& document = pending.GetDocument();

  // 3. Let |promise| be a new promise.
  // For optimization allocate the ScriptPromiseResolver just after step 4.
  ScriptPromiseResolver* resolver = nullptr;

  // 4. If |pendingDoc| is not fully active, then reject |promise| with a
  // TypeError exception and return |promise|.
  if (!document.IsActive() || !document.GetFrame()) {
    if (!script_state)
      return ScriptPromise();
    return ScriptPromise::Reject(
        script_state, V8ThrowException::CreateTypeError(
                          script_state->GetIsolate(), "Document not active"));
  }

  if (script_state) {
    // We should only be creating promises for unprefixed variants.
    DCHECK_EQ(Fullscreen::RequestType::kUnprefixed, request_type);
    resolver = ScriptPromiseResolver::Create(script_state);
  }

  bool for_cross_process_descendant =
      request_type == RequestType::kPrefixedForCrossProcessDescendant;

  // Use counters only need to be incremented in the process of the actual
  // fullscreen element.
  if (!for_cross_process_descendant) {
    if (document.IsSecureContext()) {
      UseCounter::Count(document, WebFeature::kFullscreenSecureOrigin);
    } else {
      UseCounter::Count(document, WebFeature::kFullscreenInsecureOrigin);
      HostsUsingFeatures::CountAnyWorld(
          document, HostsUsingFeatures::Feature::kFullscreenInsecureHost);
    }
  }

  // 5. Let |error| be false.
  bool error = false;

  // 6. If any of the following conditions are false, then set |error| to true:
  //
  // OOPIF: If |RequestFullscreen()| was already called in a descendant frame
  // and passed the checks, do not check again here.
  if (!for_cross_process_descendant &&
      !RequestFullscreenConditionsMet(pending, document))
    error = true;

  // 7. Return |promise|, and run the remaining steps in parallel.
  ScriptPromise promise = resolver ? resolver->Promise() : ScriptPromise();

  // 8. If |error| is false: Resize |pendingDoc|'s top-level browsing context's
  // document's viewport's dimensions to match the dimensions of the screen of
  // the output device. Optionally display a message how the end user can
  // revert this.
  if (!error) {
    if (From(document).pending_requests_.size()) {
      UseCounter::Count(document,
                        WebFeature::kFullscreenRequestWithPendingElement);
    }

    From(document).pending_requests_.push_back(
        new PendingRequest(&pending, request_type, resolver));
    LocalFrame& frame = *document.GetFrame();
    frame.GetChromeClient().EnterFullscreen(frame, options);
  } else {
    // Note: Although we are past the "in parallel" point, it's OK to continue
    // synchronously because when |error| is true, |ContinueRequestFullscreen()|
    // will only queue a task and return. This is indistinguishable from, e.g.,
    // enqueueing a microtask to continue at step 9.
    ContinueRequestFullscreen(document, pending, request_type, resolver,
                              true /* error */);
  }

  return promise;
}

void Fullscreen::DidEnterFullscreen(Document& document) {
  // We may be called synchronously from within
  // |FullscreenController::EnterFullscreen()| if we were already fullscreen,
  // but must still not synchronously change the fullscreen element. Instead
  // enqueue a microtask to continue.
  if (RequestFullscreenScope::RunningRequestFullscreen()) {
    Microtask::EnqueueMicrotask(
        WTF::Bind(DidEnterFullscreenTask, WrapPersistent(&document)));
    return;
  }

  PendingRequests requests;
  requests.swap(From(document).pending_requests_);
  for (const Member<PendingRequest>& request : requests) {
    ContinueRequestFullscreen(document, *request->element(), request->type(),
                              request->resolver(), false /* error */);
  }
}

void Fullscreen::ContinueRequestFullscreen(Document& document,
                                           Element& pending,
                                           RequestType request_type,
                                           ScriptPromiseResolver* resolver,
                                           bool error) {
  DCHECK(document.IsActive());
  DCHECK(document.GetFrame());

  // 9. If any of the following conditions are false, then set |error| to true:
  //     * |pending|'s node document is |pendingDoc|.
  //     * The fullscreen element ready check for |pending| returns true.
  if (pending.GetDocument() != document ||
      !FullscreenElementReady(pending, ReportOptions::kDoNotReport))
    error = true;

  // 10. If |error| is true:
  if (error) {
    // 10.1. Append (fullscreenerror, |pending|) to |pendingDoc|'s list of
    // pending fullscreen events.
    EnqueueEvent(EventTypeNames::fullscreenerror, pending, document,
                 request_type);

    // 10.2. Reject |promise| with a TypeError exception and terminate these
    // steps.
    if (resolver) {
      ScriptState::Scope scope(resolver->GetScriptState());
      // TODO(dtapuska): Change error to be something useful instead of just a
      // boolean and return this to the user.
      resolver->Reject(V8ThrowException::CreateTypeError(
          resolver->GetScriptState()->GetIsolate(), "fullscreen error"));
    }
    return;
  }

  // 11. Let |fullscreenElements| be an ordered set initially consisting of
  // |pending|.
  HeapVector<Member<Element>> fullscreen_elements;
  fullscreen_elements.push_back(pending);

  // 12. While the first element in |fullscreenElements| is in a nested browsing
  // context: append its browsing context container to |fullscreenElements|.
  //
  // OOPIF: |fullscreenElements| will only contain elements for local ancestors,
  // and remote ancestors will be processed in their respective processes. This
  // preserves the spec's event firing order for local ancestors, but not for
  // remote ancestors. However, that difference shouldn't be observable in
  // practice: a fullscreenchange event handler would need to postMessage a
  // frame in another renderer process, where the message should be queued up
  // and processed after the IPC that dispatches fullscreenchange.
  for (Frame* frame = pending.GetDocument().GetFrame(); frame;
       frame = frame->Tree().Parent()) {
    if (!frame->Owner() || !frame->Owner()->IsLocal())
      continue;
    Element* element = ToHTMLFrameOwnerElement(frame->Owner());
    fullscreen_elements.push_back(element);
  }

  // 13. For each |element| in |fullscreenElements|:
  for (Element* element : fullscreen_elements) {
    // 13.1. Let |doc| be |element|'s node document.
    Document& doc = element->GetDocument();

    // 13.2. If |element| is |doc|'s fullscreen element, continue.
    if (element == FullscreenElementFrom(doc))
      continue;

    // 13.3. If |element| is |pending| and |pending| is an iframe element, set
    // |element|'s iframe fullscreen flag.
    // TODO(foolip): Support the iframe fullscreen flag.
    // https://crbug.com/644695

    // 13.4. Fullscreen |element| within |doc|.
    GoFullscreen(*element, request_type);

    // 13.5. Append (fullscreenchange, |element|) to |doc|'s list of pending
    // fullscreen events.
    EnqueueEvent(EventTypeNames::fullscreenchange, *element, doc, request_type);
  }

  // 14. Resolve |promise| with undefined.
  if (resolver) {
    ScriptState::Scope scope(resolver->GetScriptState());
    resolver->Resolve();
  }
}

// https://fullscreen.spec.whatwg.org/#fully-exit-fullscreen
void Fullscreen::FullyExitFullscreen(Document& document, bool ua_originated) {
  // TODO(foolip): The spec used to have a first step saying "Let |doc| be the
  // top-level browsing context's document" which was removed in
  // https://github.com/whatwg/fullscreen/commit/3243119d027a8ff5b80998eb1f17f8eba148a346.
  // Remove it here as well.
  Document& doc = TopmostLocalAncestor(document);

  // 1. If |document|'s fullscreen element is null, terminate these steps.
  Element* fullscreen_element = FullscreenElementFrom(doc);
  if (!fullscreen_element)
    return;

  // 2. Unfullscreen elements whose fullscreen flag is set, within
  // |document|'s top layer, except for |document|'s fullscreen element.
  HeapVector<Member<Element>> unfullscreen_elements;
  for (Element* element : doc.TopLayerElements()) {
    if (HasFullscreenFlag(*element) && element != fullscreen_element)
      unfullscreen_elements.push_back(element);
  }
  for (Element* element : unfullscreen_elements)
    Unfullscreen(*element);
  DCHECK(IsSimpleFullscreenDocument(doc));

  // 3. Exit fullscreen |document|.
  ExitFullscreen(doc, nullptr, ua_originated);
}

// https://fullscreen.spec.whatwg.org/#exit-fullscreen
ScriptPromise Fullscreen::ExitFullscreen(Document& doc,
                                         ScriptState* script_state,
                                         bool ua_originated) {
  // 1. Let |promise| be a new promise.
  // ScriptPromiseResolver is allocated after step 2.
  ScriptPromiseResolver* resolver = nullptr;

  // 2. If |doc| is not fully active or |doc|'s fullscreen element is null, then
  // reject |promise| with a TypeError exception and return |promise|.
  if (!doc.IsActive() || !doc.GetFrame() || !FullscreenElementFrom(doc)) {
    if (!script_state)
      return ScriptPromise();
    return ScriptPromise::Reject(
        script_state, V8ThrowException::CreateTypeError(
                          script_state->GetIsolate(), "Document not active"));
  }

  if (script_state)
    resolver = ScriptPromiseResolver::Create(script_state);

  // 3. Let |resize| be false.
  bool resize = false;

  // 4. Let |docs| be the result of collecting documents to unfullscreen given
  // |doc|.
  HeapVector<Member<Document>> docs = CollectDocumentsToUnfullscreen(doc);

  // 5. Let |topLevelDoc| be |doc|'s top-level browsing context's active
  // document.
  //
  // OOPIF: Let |topLevelDoc| be the topmost local ancestor instead. If the main
  // frame is in another process, we will still fully exit fullscreen even
  // though that's wrong if the main frame was in nested fullscreen.
  // TODO(alexmos): Deal with nested fullscreen cases, see
  // https://crbug.com/617369.
  Document& top_level_doc = TopmostLocalAncestor(doc);

  // 6. If |topLevelDoc| is in |docs|, and it is a simple fullscreen document,
  // then set |doc| to |topLevelDoc| and |resize| to true.
  //
  // Note: |doc| is not set here, but |doc| will be the topmost local ancestor
  // in |Fullscreen::ContinueExitFullscreen| if |resize| is true.
  if (!docs.IsEmpty() && docs.back() == &top_level_doc &&
      IsSimpleFullscreenDocument(top_level_doc)) {
    resize = true;
  }

  // 7. If |doc|'s fullscreen element is not connected.
  Element* element = FullscreenElementFrom(doc);
  if (!element->isConnected()) {
    RequestType request_type = GetRequestType(*element);

    // 7.1. Append (fullscreenchange, |doc|'s fullscreen element) to
    // |doc|'s list of pending fullscreen events.
    EnqueueEvent(EventTypeNames::fullscreenchange, *element, doc, request_type);

    // 7.2. Unfullscreen |element|.
    Unfullscreen(*element);
  }

  // 7. Return |promise|, and run the remaining steps in parallel.
  ScriptPromise promise = resolver ? resolver->Promise() : ScriptPromise();

  // 8. If |resize| is true, resize |doc|'s viewport to its "normal" dimensions.
  if (resize) {
    if (ua_originated) {
      ContinueExitFullscreen(&doc, resolver, true /* resize */);
    } else {
      From(top_level_doc).pending_exits_.push_back(resolver);
      LocalFrame& frame = *doc.GetFrame();
      frame.GetChromeClient().ExitFullscreen(frame);
    }
  } else {
    DCHECK(!ua_originated);
    // Note: We are past the "in parallel" point, and |ContinueExitFullscreen()|
    // will change script-observable state (document.fullscreenElement)
    // synchronously, so we have to continue asynchronously.
    Microtask::EnqueueMicrotask(
        WTF::Bind(ContinueExitFullscreen, WrapPersistent(&doc),
                  WrapPersistent(resolver), false /* resize */));
  }
  return promise;
}

void Fullscreen::DidExitFullscreen(Document& document) {
  // If this is a response to an ExitFullscreen call then
  // continue exiting. Otherwise call FullyExitFullscreen.
  Fullscreen& fullscreen = From(document);
  PendingExits exits;
  exits.swap(fullscreen.pending_exits_);
  if (exits.IsEmpty()) {
    FullyExitFullscreen(document, true /* ua_originated */);
  } else {
    for (const Member<PendingExit>& exit : exits)
      ContinueExitFullscreen(&document, exit, true /* resize */);
  }
}

void Fullscreen::ContinueExitFullscreen(Document* doc,
                                        ScriptPromiseResolver* resolver,
                                        bool resize) {
  if (!doc || !doc->IsActive() || !doc->GetFrame()) {
    if (resolver) {
      ScriptState::Scope scope(resolver->GetScriptState());
      resolver->Reject(V8ThrowException::CreateTypeError(
          resolver->GetScriptState()->GetIsolate(), "Document is not active"));
    }
    return;
  }

  if (resize) {
    // See comment for step 6.
    DCHECK_EQ(nullptr, NextLocalAncestor(*doc));
  }

  // 9. If |doc|'s fullscreen element is null, then resolve |promise| with
  // undefined and terminate these steps.
  if (!FullscreenElementFrom(*doc)) {
    if (resolver) {
      ScriptState::Scope scope(resolver->GetScriptState());
      resolver->Resolve();
    }
    return;
  }

  // 10. Let |exitDocs| be the result of collecting documents to unfullscreen
  // given |doc|.
  HeapVector<Member<Document>> exit_docs = CollectDocumentsToUnfullscreen(*doc);

  // 11. Let |descendantDocs| be an ordered set consisting of |doc|'s
  // descendant browsing contexts' documents whose fullscreen element is
  // non-null, if any, in tree order.
  HeapVector<Member<Document>> descendant_docs;
  for (Frame* descendant = doc->GetFrame()->Tree().FirstChild(); descendant;
       descendant = descendant->Tree().TraverseNext(doc->GetFrame())) {
    if (!descendant->IsLocalFrame())
      continue;
    DCHECK(ToLocalFrame(descendant)->GetDocument());
    if (FullscreenElementFrom(*ToLocalFrame(descendant)->GetDocument()))
      descendant_docs.push_back(ToLocalFrame(descendant)->GetDocument());
  }

  // 12. For each |exitDoc| in |exitDocs|:
  for (Document* exit_doc : exit_docs) {
    Element* exit_element = FullscreenElementFrom(*exit_doc);
    DCHECK(exit_element);
    RequestType request_type = GetRequestType(*exit_element);

    // 12.1. Append (fullscreenchange, |exitDoc|'s fullscreen element) to
    // |exitDoc|'s list of pending fullscreen events.
    EnqueueEvent(EventTypeNames::fullscreenchange, *exit_element, *exit_doc,
                 request_type);

    // 12.2. If |resize| is true, unfullscreen |exitDoc|.
    // 12.3. Otherwise, unfullscreen |exitDoc|'s fullscreen element.
    if (resize)
      Unfullscreen(*exit_doc);
    else
      Unfullscreen(*exit_element);
  }

  // 13. For each |descendantDoc| in |descendantDocs|:
  for (Document* descendant_doc : descendant_docs) {
    Element* descendant_element = FullscreenElementFrom(*descendant_doc);
    DCHECK(descendant_element);
    RequestType request_type = GetRequestType(*descendant_element);

    // 13.1. Append (fullscreenchange, |descendantDoc|'s fullscreen element) to
    // |descendantDoc|'s list of pending fullscreen events.
    EnqueueEvent(EventTypeNames::fullscreenchange, *descendant_element,
                 *descendant_doc, request_type);

    // 13.2. Unfullscreen |descendantDoc|.
    Unfullscreen(*descendant_doc);
  }

  // 14. Resolve |promise| with undefined.
  if (resolver) {
    ScriptState::Scope scope(resolver->GetScriptState());
    resolver->Resolve();
  }
}

// https://fullscreen.spec.whatwg.org/#dom-document-fullscreenenabled
bool Fullscreen::FullscreenEnabled(Document& document) {
  // The fullscreenEnabled attribute's getter must return true if the context
  // object is allowed to use the feature indicated by attribute name
  // allowfullscreen and fullscreen is supported, and false otherwise.
  return AllowedToUseFullscreen(document.GetFrame(),
                                ReportOptions::kDoNotReport) &&
         FullscreenIsSupported(document);
}

void Fullscreen::DidUpdateSize(Element& element) {
  // StyleAdjuster will set the size so we need to do a style recalc.
  // Normally changing size means layout so just doing a style recalc is a
  // bit surprising.
  element.SetNeedsStyleRecalc(
      kLocalStyleChange,
      StyleChangeReasonForTracing::Create(StyleChangeReason::kFullscreen));
}

void Fullscreen::ElementRemoved(Element& node) {
  DCHECK(node.IsInTopLayer());
  if (!HasFullscreenFlag(node))
    return;

  // 1. Let |document| be removedNode's node document.
  Document& document = node.GetDocument();

  // |Fullscreen::ElementRemoved()| is called for each removed element, so only
  // the body of the spec "removing steps" loop appears here:

  // 3.1. If |node| is its node document's fullscreen element, exit fullscreen
  // that document.
  if (IsFullscreenElement(node)) {
    ExitFullscreen(document, nullptr, false);
  } else {
    // 3.2. Otherwise, unfullscreen |node| within its node document.
    Unfullscreen(node);
  }

  // 3.3 If document's top layer contains node, remove node from document's top
  // layer. This is done in Element::RemovedFrom.
}

void Fullscreen::Trace(blink::Visitor* visitor) {
  visitor->Trace(pending_requests_);
  visitor->Trace(pending_exits_);
  Supplement<Document>::Trace(visitor);
  ContextLifecycleObserver::Trace(visitor);
}

Fullscreen::PendingRequest::PendingRequest(Element* element,
                                           RequestType type,
                                           ScriptPromiseResolver* resolver)
    : element_(element), type_(type), resolver_(resolver) {}

Fullscreen::PendingRequest::~PendingRequest() = default;

void Fullscreen::PendingRequest::Trace(blink::Visitor* visitor) {
  visitor->Trace(element_);
  visitor->Trace(resolver_);
}

}  // namespace blink
