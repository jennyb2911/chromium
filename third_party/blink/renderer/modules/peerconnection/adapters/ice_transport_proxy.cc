// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/peerconnection/adapters/ice_transport_proxy.h"

#include "third_party/blink/renderer/modules/peerconnection/adapters/ice_transport_host.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/web_rtc_cross_thread_copier.h"
#include "third_party/blink/renderer/platform/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/web_task_runner.h"

namespace blink {

IceTransportProxy::IceTransportProxy(
    FrameScheduler* frame_scheduler,
    scoped_refptr<base::SingleThreadTaskRunner> proxy_thread,
    scoped_refptr<base::SingleThreadTaskRunner> host_thread,
    Delegate* delegate,
    std::unique_ptr<IceTransportAdapterCrossThreadFactory> adapter_factory)
    : host_thread_(std::move(host_thread)),
      host_(nullptr, base::OnTaskRunnerDeleter(host_thread_)),
      delegate_(delegate),
      connection_handle_for_scheduler_(
          frame_scheduler->OnActiveConnectionCreated()),
      weak_ptr_factory_(this) {
  DCHECK(host_thread_);
  DCHECK(delegate_);
  DCHECK(adapter_factory);
  DCHECK(proxy_thread->BelongsToCurrentThread());
  adapter_factory->InitializeOnMainThread();
  // Wait to initialize the host until the weak_ptr_factory_ is initialized.
  // The IceTransportHost is constructed on the proxy thread but should only be
  // interacted with via PostTask to the host thread. The OnTaskRunnerDeleter
  // (configured above) will ensure it gets deleted on the host thread.
  host_.reset(new IceTransportHost(std::move(proxy_thread),
                                   weak_ptr_factory_.GetWeakPtr()));
  PostCrossThreadTask(*host_thread_, FROM_HERE,
                      CrossThreadBind(&IceTransportHost::Initialize,
                                      CrossThreadUnretained(host_.get()),
                                      WTF::Passed(std::move(adapter_factory))));
}

IceTransportProxy::~IceTransportProxy() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // Note: The IceTransportHost will be deleted on the host thread.
}

void IceTransportProxy::StartGathering(
    const cricket::IceParameters& local_parameters,
    const cricket::ServerAddresses& stun_servers,
    const std::vector<cricket::RelayServerConfig>& turn_servers,
    IceTransportPolicy policy) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  PostCrossThreadTask(
      *host_thread_, FROM_HERE,
      CrossThreadBind(&IceTransportHost::StartGathering,
                      CrossThreadUnretained(host_.get()), local_parameters,
                      stun_servers, turn_servers, policy));
}

void IceTransportProxy::Start(
    const cricket::IceParameters& remote_parameters,
    cricket::IceRole role,
    const std::vector<cricket::Candidate>& initial_remote_candidates) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  PostCrossThreadTask(
      *host_thread_, FROM_HERE,
      CrossThreadBind(&IceTransportHost::Start,
                      CrossThreadUnretained(host_.get()), remote_parameters,
                      role, initial_remote_candidates));
}

void IceTransportProxy::HandleRemoteRestart(
    const cricket::IceParameters& new_remote_parameters) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  PostCrossThreadTask(*host_thread_, FROM_HERE,
                      CrossThreadBind(&IceTransportHost::HandleRemoteRestart,
                                      CrossThreadUnretained(host_.get()),
                                      new_remote_parameters));
}

void IceTransportProxy::AddRemoteCandidate(
    const cricket::Candidate& candidate) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  PostCrossThreadTask(
      *host_thread_, FROM_HERE,
      CrossThreadBind(&IceTransportHost::AddRemoteCandidate,
                      CrossThreadUnretained(host_.get()), candidate));
}

void IceTransportProxy::OnGatheringStateChanged(
    cricket::IceGatheringState new_state) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  delegate_->OnGatheringStateChanged(new_state);
}

void IceTransportProxy::OnCandidateGathered(
    const cricket::Candidate& candidate) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  delegate_->OnCandidateGathered(candidate);
}

void IceTransportProxy::OnStateChanged(cricket::IceTransportState new_state) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  delegate_->OnStateChanged(new_state);
}

}  // namespace blink
