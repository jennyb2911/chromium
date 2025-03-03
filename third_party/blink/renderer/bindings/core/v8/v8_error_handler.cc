/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
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

#include "third_party/blink/renderer/bindings/core/v8/v8_error_handler.h"

#include "third_party/blink/renderer/bindings/core/v8/script_controller.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_error_event.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_script_runner.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/platform/bindings/v8_private_property.h"

namespace blink {

V8ErrorHandler::V8ErrorHandler(bool is_inline, ScriptState* script_state)
    : V8EventListenerOrEventHandler(is_inline, script_state) {}

v8::Local<v8::Value> V8ErrorHandler::CallListenerFunction(
    ScriptState* script_state,
    v8::Local<v8::Value> js_event,
    Event* event) {
  DCHECK(!js_event.IsEmpty());
  if (!event->HasInterface(EventNames::ErrorEvent)) {
    return V8EventListenerOrEventHandler::CallListenerFunction(script_state,
                                                               js_event, event);
  }

  ErrorEvent* error_event = static_cast<ErrorEvent*>(event);
  v8::Local<v8::Context> context = script_state->GetContext();
  ExecutionContext* execution_context = ToExecutionContext(context);
  v8::Local<v8::Object> listener = GetListenerObjectInternal(execution_context);
  if (listener.IsEmpty() || !listener->IsFunction())
    return v8::Null(GetIsolate());

  v8::Local<v8::Function> call_function =
      v8::Local<v8::Function>::Cast(listener);
  v8::Local<v8::Object> this_value = context->Global();

  // The error attribute should be initialized to null for dedicated workers.
  // https://html.spec.whatwg.org/multipage/workers.html#runtime-script-errors-2
  ScriptValue error = error_event->error(script_state);
  v8::Local<v8::Value> error_value =
      (error.IsEmpty() ||
       error_event->target()->InterfaceName() == EventTargetNames::Worker)
          ? v8::Local<v8::Value>(v8::Null(GetIsolate()))
          : error.V8Value();

  v8::Local<v8::Value> parameters[5] = {
      V8String(GetIsolate(), error_event->message()),
      V8String(GetIsolate(), error_event->filename()),
      v8::Integer::New(GetIsolate(), error_event->lineno()),
      v8::Integer::New(GetIsolate(), error_event->colno()), error_value};
  v8::TryCatch try_catch(GetIsolate());
  try_catch.SetVerbose(true);

  v8::MaybeLocal<v8::Value> result = V8ScriptRunner::CallFunction(
      call_function, execution_context, this_value, base::size(parameters),
      parameters, GetIsolate());
  v8::Local<v8::Value> return_value;
  if (!result.ToLocal(&return_value))
    return v8::Null(GetIsolate());

  return return_value;
}

bool V8ErrorHandler::ShouldPreventDefault(v8::Local<v8::Value> return_value,
                                          Event* event) {
  // Special event handling should be done here according to HTML Standard:
  // https://html.spec.whatwg.org/multipage/webappapis.html#the-event-handler-processing-algorithm
  // We do not need to check current target of event because it must be window
  // or worker on calling this method.
  if (event->HasInterface(EventNames::ErrorEvent) && event->type() == "error")
    return return_value->IsBoolean() && return_value.As<v8::Boolean>()->Value();
  return return_value->IsBoolean() && !return_value.As<v8::Boolean>()->Value();
}

}  // namespace blink
