// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/background_fetch/background_fetch_update_ui_event.h"

#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/fetch/request.h"
#include "third_party/blink/renderer/core/fetch/response.h"
#include "third_party/blink/renderer/modules/background_fetch/background_fetch_bridge.h"
#include "third_party/blink/renderer/modules/background_fetch/background_fetch_event_init.h"
#include "third_party/blink/renderer/modules/background_fetch/background_fetch_icon_loader.h"
#include "third_party/blink/renderer/modules/background_fetch/background_fetch_registration.h"
#include "third_party/blink/renderer/modules/background_fetch/background_fetch_settled_fetch.h"
#include "third_party/blink/renderer/modules/background_fetch/background_fetch_ui_options.h"
#include "third_party/blink/renderer/modules/event_modules_names.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"

namespace blink {

BackgroundFetchUpdateUIEvent::BackgroundFetchUpdateUIEvent(
    const AtomicString& type,
    const BackgroundFetchEventInit& initializer)
    : BackgroundFetchEvent(type, initializer, nullptr /* observer */) {}

BackgroundFetchUpdateUIEvent::BackgroundFetchUpdateUIEvent(
    const AtomicString& type,
    const BackgroundFetchEventInit& initializer,
    WaitUntilObserver* observer,
    ServiceWorkerRegistration* registration)
    : BackgroundFetchEvent(type, initializer, observer),
      service_worker_registration_(registration) {}

BackgroundFetchUpdateUIEvent::~BackgroundFetchUpdateUIEvent() = default;

void BackgroundFetchUpdateUIEvent::Trace(blink::Visitor* visitor) {
  visitor->Trace(service_worker_registration_);
  visitor->Trace(loader_);
  BackgroundFetchEvent::Trace(visitor);
}

ScriptPromise BackgroundFetchUpdateUIEvent::updateUI(
    ScriptState* script_state,
    const BackgroundFetchUIOptions& ui_options) {
  if (update_ui_called_) {
    // Return a rejected promise as this method should only be called once.
    return ScriptPromise::Reject(
        script_state,
        V8ThrowException::CreateTypeError(script_state->GetIsolate(),
                                          "updateUI may only be called once."));
  }

  update_ui_called_ = true;

  if (!service_worker_registration_) {
    // Return a Promise that will never settle when a developer calls this
    // method on a BackgroundFetchSuccessEvent instance they created themselves.
    // TODO(crbug.com/872768): Figure out if this is the right thing to do
    // vs reacting eagerly.
    return ScriptPromise();
  }
  DCHECK(!registration_->unique_id().IsEmpty());

  if (!ui_options.hasTitle() && ui_options.icons().IsEmpty()) {
    // Nothing to update, just return a resolved promise.
    return ScriptPromise::CastUndefined(script_state);
  }

  ScriptPromiseResolver* resolver = ScriptPromiseResolver::Create(script_state);
  ScriptPromise promise = resolver->Promise();

  if (ui_options.icons().IsEmpty()) {
    DidGetIcon(resolver, ui_options.title(), SkBitmap(),
               -1 /* ideal_to_chosen_icon_size */);
  } else {
    DCHECK(!loader_);
    loader_ = new BackgroundFetchIconLoader();
    DCHECK(loader_);
    loader_->Start(BackgroundFetchBridge::From(service_worker_registration_),
                   ExecutionContext::From(script_state), ui_options.icons(),
                   WTF::Bind(&BackgroundFetchUpdateUIEvent::DidGetIcon,
                             WrapPersistent(this), WrapPersistent(resolver),
                             ui_options.title()));
  }

  return promise;
}

void BackgroundFetchUpdateUIEvent::DidGetIcon(
    ScriptPromiseResolver* resolver,
    const String& title,
    const SkBitmap& icon,
    int64_t ideal_to_chosen_icon_size) {
  BackgroundFetchBridge::From(service_worker_registration_)
      ->UpdateUI(registration_->id(), registration_->unique_id(), title, icon,
                 WTF::Bind(&BackgroundFetchUpdateUIEvent::DidUpdateUI,
                           WrapPersistent(this), WrapPersistent(resolver)));
}

void BackgroundFetchUpdateUIEvent::DidUpdateUI(
    ScriptPromiseResolver* resolver,
    mojom::blink::BackgroundFetchError error) {
  switch (error) {
    case mojom::blink::BackgroundFetchError::NONE:
    case mojom::blink::BackgroundFetchError::INVALID_ID:
      resolver->Resolve();
      return;
    case mojom::blink::BackgroundFetchError::STORAGE_ERROR:
      resolver->Reject(
          DOMException::Create(DOMExceptionCode::kAbortError,
                               "Failed to update UI due to I/O error."));
      return;
    case mojom::blink::BackgroundFetchError::DUPLICATED_DEVELOPER_ID:
    case mojom::blink::BackgroundFetchError::INVALID_ARGUMENT:
    case mojom::blink::BackgroundFetchError::SERVICE_WORKER_UNAVAILABLE:
    case mojom::blink::BackgroundFetchError::PERMISSION_DENIED:
    case mojom::blink::BackgroundFetchError::QUOTA_EXCEEDED:
      // Not applicable for this callback.
      break;
  }

  NOTREACHED();
}

}  // namespace blink
