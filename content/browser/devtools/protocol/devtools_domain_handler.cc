// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/devtools/protocol/devtools_domain_handler.h"

#include "content/browser/devtools/devtools_agent_host_impl.h"
#include "content/browser/devtools/protocol/protocol.h"

namespace content {
namespace protocol {

DevToolsDomainHandler::DevToolsDomainHandler(const std::string& name)
    : name_(name) {
}

DevToolsDomainHandler::~DevToolsDomainHandler() {
}

void DevToolsDomainHandler::InitRenderer(int process_host_id,
                                         RenderFrameHostImpl* frame_host) {
  SetRenderer(process_host_id, frame_host);
}

void DevToolsDomainHandler::UpdateRenderer(int process_host_id,
                                           RenderFrameHostImpl* frame_host) {
  SetRenderer(process_host_id, frame_host);
}

void DevToolsDomainHandler::SetRenderer(int process_host_id,
                                        RenderFrameHostImpl* frame_host) {}

void DevToolsDomainHandler::Wire(UberDispatcher* dispatcher) {
}

Response DevToolsDomainHandler::Disable() {
  return Response::OK();
}

}  // namespace protocol
}  // namespace content
