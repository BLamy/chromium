// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/web/web_message_port_converter.h"

#include "third_party/blink/public/common/messaging/message_port_channel.h"
#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_message_port.h"
#include "third_party/blink/renderer/core/messaging/message_port.h"

namespace blink {

std::optional<MessagePortChannel>
WebMessagePortConverter::DisentangleAndExtractMessagePortChannel(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value) {
  MessagePort* port = V8MessagePort::ToWrappable(isolate, value);
  if (!port || port->IsNeutered())
    return std::nullopt;
  return port->Disentangle();
}

v8::Local<v8::Value>
WebMessagePortConverter::EntangleAndInjectMessagePortChannel(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    MessagePortChannel port_channel) {
  auto* execution_context = ToExecutionContext(context);
  CHECK(execution_context);
  auto* port = MakeGarbageCollected<MessagePort>(*execution_context);
  port->Entangle(std::move(port_channel));
  return port->ToV8(isolate, context->Global());
}

}  // namespace blink
