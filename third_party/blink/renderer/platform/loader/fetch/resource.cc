/*
    Copyright (C) 1998 Lars Knoll (knoll@mpi-hd.mpg.de)
    Copyright (C) 2001 Dirk Mueller (mueller@kde.org)
    Copyright (C) 2002 Waldo Bastian (bastian@kde.org)
    Copyright (C) 2006 Samuel Weinig (sam.weinig@gmail.com)
    Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011 Apple Inc. All
    rights reserved.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "third_party/blink/renderer/platform/loader/fetch/resource.h"

#include <stdint.h>

#include <algorithm>
#include <cassert>
#include <memory>

#include "base/single_thread_task_runner.h"
#include "build/build_config.h"
#include "services/network/public/mojom/fetch_api.mojom-blink.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/renderer/platform/histogram.h"
#include "third_party/blink/renderer/platform/instance_counters.h"
#include "third_party/blink/renderer/platform/instrumentation/tracing/trace_event.h"
#include "third_party/blink/renderer/platform/loader/cors/cors.h"
#include "third_party/blink/renderer/platform/loader/fetch/cached_metadata.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_initiator_type_names.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_parameters.h"
#include "third_party/blink/renderer/platform/loader/fetch/integrity_metadata.h"
#include "third_party/blink/renderer/platform/loader/fetch/memory_cache.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_client.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_client_walker.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_finish_observer.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_loader.h"
#include "third_party/blink/renderer/platform/network/http_parsers.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/blink/renderer/platform/shared_buffer.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/math_extras.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"
#include "third_party/blink/renderer/platform/wtf/text/cstring.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/wtf/time.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

namespace {

void NotifyFinishObservers(
    HeapHashSet<WeakMember<ResourceFinishObserver>>* observers) {
  for (const auto& observer : *observers)
    observer->NotifyFinished();
}

}  // namespace

// These response headers are not copied from a revalidated response to the
// cached response headers. For compatibility, this list is based on Chromium's
// net/http/http_response_headers.cc.
const char* const kHeadersToIgnoreAfterRevalidation[] = {
    "allow",
    "connection",
    "etag",
    "expires",
    "keep-alive",
    "last-modified",
    "proxy-authenticate",
    "proxy-connection",
    "trailer",
    "transfer-encoding",
    "upgrade",
    "www-authenticate",
    "x-frame-options",
    "x-xss-protection",
};

// Some header prefixes mean "Don't copy this header from a 304 response.".
// Rather than listing all the relevant headers, we can consolidate them into
// this list, also grabbed from Chromium's net/http/http_response_headers.cc.
const char* const kHeaderPrefixesToIgnoreAfterRevalidation[] = {
    "content-", "x-content-", "x-webkit-"};

static inline bool ShouldUpdateHeaderAfterRevalidation(
    const AtomicString& header) {
  for (size_t i = 0; i < arraysize(kHeadersToIgnoreAfterRevalidation); i++) {
    if (DeprecatedEqualIgnoringCase(header,
                                    kHeadersToIgnoreAfterRevalidation[i]))
      return false;
  }
  for (size_t i = 0; i < arraysize(kHeaderPrefixesToIgnoreAfterRevalidation);
       i++) {
    if (header.StartsWithIgnoringASCIICase(
            kHeaderPrefixesToIgnoreAfterRevalidation[i]))
      return false;
  }
  return true;
}

// This is a CachedMetadataSender implementation for normal responses.
class CachedMetadataSenderImpl : public CachedMetadataSender {
 public:
  CachedMetadataSenderImpl(const Resource*);
  ~CachedMetadataSenderImpl() override = default;

  void Send(const char*, size_t) override;
  bool IsServedFromCacheStorage() override { return false; }

 private:
  const KURL response_url_;
  const Time response_time_;
};

CachedMetadataSenderImpl::CachedMetadataSenderImpl(const Resource* resource)
    : response_url_(resource->GetResponse().Url()),
      response_time_(resource->GetResponse().ResponseTime()) {
  DCHECK(resource->GetResponse().CacheStorageCacheName().IsNull());
}

void CachedMetadataSenderImpl::Send(const char* data, size_t size) {
  Platform::Current()->CacheMetadata(response_url_, response_time_, data, size);
}

// This is a CachedMetadataSender implementation that does nothing.
class NullCachedMetadataSender : public CachedMetadataSender {
 public:
  NullCachedMetadataSender() = default;
  ~NullCachedMetadataSender() override = default;

  void Send(const char*, size_t) override {}
  bool IsServedFromCacheStorage() override { return false; }
};

// This is a CachedMetadataSender implementation for responses that are served
// by a ServiceWorker from cache storage.
class ServiceWorkerCachedMetadataSender : public CachedMetadataSender {
 public:
  ServiceWorkerCachedMetadataSender(const Resource*, const SecurityOrigin*);
  ~ServiceWorkerCachedMetadataSender() override = default;

  void Send(const char*, size_t) override;
  bool IsServedFromCacheStorage() override { return true; }

 private:
  const KURL response_url_;
  const Time response_time_;
  const String cache_storage_cache_name_;
  scoped_refptr<const SecurityOrigin> security_origin_;
};

ServiceWorkerCachedMetadataSender::ServiceWorkerCachedMetadataSender(
    const Resource* resource,
    const SecurityOrigin* security_origin)
    : response_url_(resource->GetResponse().Url()),
      response_time_(resource->GetResponse().ResponseTime()),
      cache_storage_cache_name_(
          resource->GetResponse().CacheStorageCacheName()),
      security_origin_(security_origin) {
  DCHECK(!cache_storage_cache_name_.IsNull());
}

void ServiceWorkerCachedMetadataSender::Send(const char* data, size_t size) {
  Platform::Current()->CacheMetadataInCacheStorage(
      response_url_, response_time_, data, size,
      WebSecurityOrigin(security_origin_), cache_storage_cache_name_);
}

Resource::Resource(const ResourceRequest& request,
                   ResourceType type,
                   const ResourceLoaderOptions& options)
    : type_(type),
      status_(ResourceStatus::kNotStarted),
      identifier_(0),
      encoded_size_(0),
      encoded_size_memory_usage_(0),
      decoded_size_(0),
      overhead_size_(CalculateOverheadSize()),
      cache_identifier_(MemoryCache::DefaultCacheIdentifier()),
      link_preload_(false),
      is_revalidating_(false),
      is_alive_(false),
      is_add_remove_client_prohibited_(false),
      integrity_disposition_(ResourceIntegrityDisposition::kNotChecked),
      options_(options),
      response_timestamp_(CurrentTime()),
      resource_request_(request) {
  InstanceCounters::IncrementCounter(InstanceCounters::kResourceCounter);

  if (IsMainThread())
    MemoryCoordinator::Instance().RegisterClient(this);
}

Resource::~Resource() {
  InstanceCounters::DecrementCounter(InstanceCounters::kResourceCounter);
}

void Resource::Trace(blink::Visitor* visitor) {
  visitor->Trace(loader_);
  visitor->Trace(cache_handler_);
  visitor->Trace(clients_);
  visitor->Trace(clients_awaiting_callback_);
  visitor->Trace(finished_clients_);
  visitor->Trace(finish_observers_);
  MemoryCoordinatorClient::Trace(visitor);
}

void Resource::SetLoader(ResourceLoader* loader) {
  CHECK(!loader_);
  DCHECK(StillNeedsLoad());
  loader_ = loader;
}

void Resource::CheckResourceIntegrity() {
  // Skip the check and reuse the previous check result, especially on
  // successful revalidation.
  if (IntegrityDisposition() != ResourceIntegrityDisposition::kNotChecked)
    return;

  // Loading error occurred? Then result is uncheckable.
  integrity_report_info_.Clear();
  if (ErrorOccurred()) {
    CHECK(!Data());
    integrity_disposition_ = ResourceIntegrityDisposition::kFailed;
    return;
  }

  // No integrity attributes to check? Then we're passing.
  if (IntegrityMetadata().IsEmpty()) {
    integrity_disposition_ = ResourceIntegrityDisposition::kPassed;
    return;
  }

  const char* data = nullptr;
  size_t data_length = 0;

  // Edge case: If a resource actually has zero bytes then it will not
  // typically have a resource buffer, but we still need to check integrity
  // because people might want to assert a zero-length resource.
  CHECK(DecodedSize() == 0 || Data());
  if (Data()) {
    data = Data()->Data();
    data_length = Data()->size();
  }

  if (SubresourceIntegrity::CheckSubresourceIntegrity(IntegrityMetadata(), data,
                                                      data_length, Url(), *this,
                                                      integrity_report_info_))
    integrity_disposition_ = ResourceIntegrityDisposition::kPassed;
  else
    integrity_disposition_ = ResourceIntegrityDisposition::kFailed;
  DCHECK_NE(IntegrityDisposition(), ResourceIntegrityDisposition::kNotChecked);
}

void Resource::NotifyFinished() {
  DCHECK(IsLoaded());

  ResourceClientWalker<ResourceClient> w(clients_);
  while (ResourceClient* c = w.Next()) {
    MarkClientFinished(c);
    c->NotifyFinished(this);
  }
}

void Resource::MarkClientFinished(ResourceClient* client) {
  if (clients_.Contains(client)) {
    finished_clients_.insert(client);
    clients_.erase(client);
  }
}

void Resource::AppendData(const char* data, size_t length) {
  TRACE_EVENT0("blink", "Resource::appendData");
  DCHECK(!is_revalidating_);
  DCHECK(!ErrorOccurred());
  if (options_.data_buffering_policy == kBufferData) {
    if (data_)
      data_->Append(data, length);
    else
      data_ = SharedBuffer::Create(data, length);
    SetEncodedSize(data_->size());
  }
  ResourceClientWalker<ResourceClient> w(Clients());
  while (ResourceClient* c = w.Next())
    c->DataReceived(this, data, length);
}

void Resource::SetResourceBuffer(scoped_refptr<SharedBuffer> resource_buffer) {
  DCHECK(!is_revalidating_);
  DCHECK(!ErrorOccurred());
  DCHECK_EQ(options_.data_buffering_policy, kBufferData);
  data_ = std::move(resource_buffer);
  SetEncodedSize(data_->size());
}

void Resource::ClearData() {
  data_ = nullptr;
  encoded_size_memory_usage_ = 0;
}

void Resource::TriggerNotificationForFinishObservers(
    base::SingleThreadTaskRunner* task_runner) {
  if (finish_observers_.IsEmpty())
    return;

  auto* new_collections = new HeapHashSet<WeakMember<ResourceFinishObserver>>(
      std::move(finish_observers_));
  finish_observers_.clear();

  task_runner->PostTask(FROM_HERE, WTF::Bind(&NotifyFinishObservers,
                                             WrapPersistent(new_collections)));

  DidRemoveClientOrObserver();
}

void Resource::SetDataBufferingPolicy(
    DataBufferingPolicy data_buffering_policy) {
  options_.data_buffering_policy = data_buffering_policy;
  ClearData();
  SetEncodedSize(0);
}

static bool NeedsSynchronousCacheHit(ResourceType type,
                                     const ResourceLoaderOptions& options) {
  // Synchronous requests must always succeed or fail synchronously.
  if (options.synchronous_policy == kRequestSynchronously)
    return true;
  // Some resources types default to return data synchronously. For most of
  // these, it's because there are layout tests that expect data to return
  // synchronously in case of cache hit. In the case of fonts, there was a
  // performance regression.
  // FIXME: Get to the point where we don't need to special-case sync/async
  // behavior for different resource types.
  if (type == ResourceType::kCSSStyleSheet)
    return true;
  if (type == ResourceType::kScript)
    return true;
  if (type == ResourceType::kFont)
    return true;
  return false;
}

void Resource::FinishAsError(const ResourceError& error,
                             base::SingleThreadTaskRunner* task_runner) {
  error_ = error;
  is_revalidating_ = false;

  if (IsMainThread())
    GetMemoryCache()->Remove(this);

  bool failed_during_start = status_ == ResourceStatus::kNotStarted;
  if (!ErrorOccurred()) {
    SetStatus(ResourceStatus::kLoadError);
    // If the response type has not been set, set it to "error". This is
    // important because in some cases we arrive here after setting the response
    // type (e.g., while downloading payload), and that shouldn't change the
    // response type.
    if (response_.GetType() == network::mojom::FetchResponseType::kDefault)
      response_.SetType(network::mojom::FetchResponseType::kError);
  }
  DCHECK(ErrorOccurred());
  ClearData();
  loader_ = nullptr;
  CheckResourceIntegrity();
  TriggerNotificationForFinishObservers(task_runner);

  // Most resource types don't expect to succeed or fail inside
  // ResourceFetcher::RequestResource(). If the request does complete
  // immediately, the convention is to notify the client asynchronously
  // unless the type is exempted for historical reasons (mostly due to
  // performance implications to making those notifications asynchronous).
  // So if this is an immediate failure (i.e., before NotifyStartLoad()),
  // post a task if the Resource::Type supports it.
  if (failed_during_start && !NeedsSynchronousCacheHit(GetType(), options_)) {
    task_runner->PostTask(FROM_HERE, WTF::Bind(&Resource::NotifyFinished,
                                               WrapWeakPersistent(this)));
  } else {
    NotifyFinished();
  }
}

void Resource::Finish(TimeTicks load_finish_time,
                      base::SingleThreadTaskRunner* task_runner) {
  DCHECK(!is_revalidating_);
  load_finish_time_ = load_finish_time;
  if (!ErrorOccurred())
    status_ = ResourceStatus::kCached;
  loader_ = nullptr;
  CheckResourceIntegrity();
  TriggerNotificationForFinishObservers(task_runner);
  NotifyFinished();
}

AtomicString Resource::HttpContentType() const {
  return GetResponse().HttpContentType();
}

bool Resource::MustRefetchDueToIntegrityMetadata(
    const FetchParameters& params) const {
  if (params.IntegrityMetadata().IsEmpty())
    return false;

  return !IntegrityMetadata::SetsEqual(IntegrityMetadata(),
                                       params.IntegrityMetadata());
}

const scoped_refptr<const SecurityOrigin>& Resource::GetOrigin() const {
  return LastResourceRequest().RequestorOrigin();
}

static double CurrentAge(const ResourceResponse& response,
                         double response_timestamp) {
  // RFC2616 13.2.3
  // No compensation for latency as that is not terribly important in practice
  double date_value = response.Date();
  double apparent_age = std::isfinite(date_value)
                            ? std::max(0., response_timestamp - date_value)
                            : 0;
  double age_value = response.Age();
  double corrected_received_age = std::isfinite(age_value)
                                      ? std::max(apparent_age, age_value)
                                      : apparent_age;
  double resident_time = CurrentTime() - response_timestamp;
  return corrected_received_age + resident_time;
}

static double FreshnessLifetime(const ResourceResponse& response,
                                double response_timestamp) {
#if !defined(OS_ANDROID)
  // On desktop, local files should be reloaded in case they change.
  if (response.Url().IsLocalFile())
    return 0;
#endif

  // Cache other non-http / non-filesystem resources liberally.
  if (!response.Url().ProtocolIsInHTTPFamily() &&
      !response.Url().ProtocolIs("filesystem"))
    return std::numeric_limits<double>::max();

  // RFC2616 13.2.4
  double max_age_value = response.CacheControlMaxAge();
  if (std::isfinite(max_age_value))
    return max_age_value;
  double expires_value = response.Expires();
  double date_value = response.Date();
  double creation_time =
      std::isfinite(date_value) ? date_value : response_timestamp;
  if (std::isfinite(expires_value))
    return expires_value - creation_time;
  double last_modified_value = response.LastModified();
  if (std::isfinite(last_modified_value))
    return (creation_time - last_modified_value) * 0.1;
  // If no cache headers are present, the specification leaves the decision to
  // the UA. Other browsers seem to opt for 0.
  return 0;
}

static bool CanUseResponse(const ResourceResponse& response,
                           bool allow_stale,
                           double response_timestamp) {
  if (response.IsNull())
    return false;

  if (response.CacheControlContainsNoCache() ||
      response.CacheControlContainsNoStore())
    return false;

  if (response.HttpStatusCode() == 303) {
    // Must not be cached.
    return false;
  }

  if (response.HttpStatusCode() == 302 || response.HttpStatusCode() == 307) {
    // Default to not cacheable unless explicitly allowed.
    bool has_max_age = std::isfinite(response.CacheControlMaxAge());
    bool has_expires = std::isfinite(response.Expires());
    // TODO: consider catching Cache-Control "private" and "public" here.
    if (!has_max_age && !has_expires)
      return false;
  }

  double max_life = FreshnessLifetime(response, response_timestamp);
  if (allow_stale)
    max_life += response.CacheControlStaleWhileRevalidate();

  return CurrentAge(response, response_timestamp) <= max_life;
}

const ResourceRequest& Resource::LastResourceRequest() const {
  if (!redirect_chain_.size())
    return GetResourceRequest();
  return redirect_chain_.back().request_;
}

void Resource::SetRevalidatingRequest(const ResourceRequest& request) {
  SECURITY_CHECK(redirect_chain_.IsEmpty());
  SECURITY_CHECK(!is_unused_preload_);
  DCHECK(!request.IsNull());
  CHECK(!is_revalidation_start_forbidden_);
  is_revalidating_ = true;
  resource_request_ = request;
  status_ = ResourceStatus::kNotStarted;
}

bool Resource::WillFollowRedirect(const ResourceRequest& new_request,
                                  const ResourceResponse& redirect_response) {
  if (is_revalidating_)
    RevalidationFailed();
  redirect_chain_.push_back(RedirectPair(new_request, redirect_response));
  return true;
}

void Resource::SetResponse(const ResourceResponse& response) {
  response_ = response;

  // Currently we support the metadata caching only for HTTP family.
  if (!GetResourceRequest().Url().ProtocolIsInHTTPFamily() ||
      !GetResponse().Url().ProtocolIsInHTTPFamily()) {
    cache_handler_.Clear();
    return;
  }

  cache_handler_ = CreateCachedMetadataHandler(CreateCachedMetadataSender());
}

std::unique_ptr<CachedMetadataSender> Resource::CreateCachedMetadataSender()
    const {
  if (GetResponse().WasFetchedViaServiceWorker()) {
    scoped_refptr<const SecurityOrigin> origin =
        GetResourceRequest().RequestorOrigin();
    // TODO(leszeks): Check whether it's correct that |origin| can be nullptr.
    if (!origin || GetResponse().CacheStorageCacheName().IsNull()) {
      return std::make_unique<NullCachedMetadataSender>();
    }
    return std::make_unique<ServiceWorkerCachedMetadataSender>(this,
                                                               origin.get());
  }
  return std::make_unique<CachedMetadataSenderImpl>(this);
}

void Resource::ResponseReceived(const ResourceResponse& response,
                                std::unique_ptr<WebDataConsumerHandle>) {
  response_timestamp_ = CurrentTime();
  if (is_revalidating_) {
    if (response.HttpStatusCode() == 304) {
      RevalidationSucceeded(response);
      return;
    }
    RevalidationFailed();
  }
  SetResponse(response);
  String encoding = response.TextEncodingName();
  if (!encoding.IsNull())
    SetEncoding(encoding);
}

void Resource::SetSerializedCachedMetadata(const char* data, size_t size) {
  DCHECK(!is_revalidating_);
  DCHECK(!GetResponse().IsNull());
}

String Resource::ReasonNotDeletable() const {
  StringBuilder builder;
  if (HasClientsOrObservers()) {
    builder.Append("hasClients(");
    builder.AppendNumber(clients_.size());
    if (!clients_awaiting_callback_.IsEmpty()) {
      builder.Append(", AwaitingCallback=");
      builder.AppendNumber(clients_awaiting_callback_.size());
    }
    if (!finished_clients_.IsEmpty()) {
      builder.Append(", Finished=");
      builder.AppendNumber(finished_clients_.size());
    }
    builder.Append(')');
  }
  if (loader_) {
    if (!builder.IsEmpty())
      builder.Append(' ');
    builder.Append("loader_");
  }
  if (IsMainThread() && GetMemoryCache()->Contains(this)) {
    if (!builder.IsEmpty())
      builder.Append(' ');
    builder.Append("in_memory_cache");
  }
  return builder.ToString();
}

void Resource::DidAddClient(ResourceClient* c) {
  if (scoped_refptr<SharedBuffer> data = Data()) {
    for (const auto& span : *data) {
      c->DataReceived(this, span.data(), span.size());
      // Stop pushing data if the client removed itself.
      if (!HasClient(c))
        break;
    }
  }
  if (!HasClient(c))
    return;
  if (IsLoaded()) {
    c->NotifyFinished(this);
    if (clients_.Contains(c)) {
      finished_clients_.insert(c);
      clients_.erase(c);
    }
  }
}

void Resource::WillAddClientOrObserver() {
  if (!HasClientsOrObservers()) {
    is_alive_ = true;
  }
}

void Resource::AddClient(ResourceClient* client,
                         base::SingleThreadTaskRunner* task_runner) {
  CHECK(!is_add_remove_client_prohibited_);

  WillAddClientOrObserver();

  if (is_revalidating_) {
    clients_.insert(client);
    return;
  }

  // If an error has occurred or we have existing data to send to the new client
  // and the resource type supports it, send it asynchronously.
  if ((ErrorOccurred() || !GetResponse().IsNull()) &&
      !NeedsSynchronousCacheHit(GetType(), options_)) {
    clients_awaiting_callback_.insert(client);
    if (!async_finish_pending_clients_task_.IsActive()) {
      async_finish_pending_clients_task_ = PostCancellableTask(
          *task_runner, FROM_HERE,
          WTF::Bind(&Resource::FinishPendingClients, WrapWeakPersistent(this)));
    }
    return;
  }

  clients_.insert(client);
  DidAddClient(client);
  return;
}

void Resource::RemoveClient(ResourceClient* client) {
  CHECK(!is_add_remove_client_prohibited_);

  // This code may be called in a pre-finalizer, where weak members in the
  // HashCountedSet are already swept out.

  if (finished_clients_.Contains(client))
    finished_clients_.erase(client);
  else if (clients_awaiting_callback_.Contains(client))
    clients_awaiting_callback_.erase(client);
  else
    clients_.erase(client);

  if (clients_awaiting_callback_.IsEmpty() &&
      async_finish_pending_clients_task_.IsActive()) {
    async_finish_pending_clients_task_.Cancel();
  }

  DidRemoveClientOrObserver();
}

void Resource::AddFinishObserver(ResourceFinishObserver* client,
                                 base::SingleThreadTaskRunner* task_runner) {
  CHECK(!is_add_remove_client_prohibited_);
  DCHECK(!finish_observers_.Contains(client));

  WillAddClientOrObserver();
  finish_observers_.insert(client);
  if (IsLoaded())
    TriggerNotificationForFinishObservers(task_runner);
}

void Resource::RemoveFinishObserver(ResourceFinishObserver* client) {
  CHECK(!is_add_remove_client_prohibited_);

  finish_observers_.erase(client);
  DidRemoveClientOrObserver();
}

void Resource::DidRemoveClientOrObserver() {
  if (!HasClientsOrObservers() && is_alive_) {
    is_alive_ = false;
    AllClientsAndObserversRemoved();

    // RFC2616 14.9.2:
    // "no-store: ... MUST make a best-effort attempt to remove the information
    // from volatile storage as promptly as possible"
    // "... History buffers MAY store such responses as part of their normal
    // operation."
    // We allow non-secure content to be reused in history, but we do not allow
    // secure content to be reused.
    if (HasCacheControlNoStoreHeader() && Url().ProtocolIs("https") &&
        IsMainThread())
      GetMemoryCache()->Remove(this);
  }
}

void Resource::AllClientsAndObserversRemoved() {
  if (loader_)
    loader_->ScheduleCancel();
}

void Resource::SetDecodedSize(size_t decoded_size) {
  if (decoded_size == decoded_size_)
    return;
  size_t old_size = size();
  decoded_size_ = decoded_size;
  if (IsMainThread())
    GetMemoryCache()->Update(this, old_size, size());
}

void Resource::SetEncodedSize(size_t encoded_size) {
  if (encoded_size == encoded_size_ &&
      encoded_size == encoded_size_memory_usage_)
    return;
  size_t old_size = size();
  encoded_size_ = encoded_size;
  encoded_size_memory_usage_ = encoded_size;
  if (IsMainThread())
    GetMemoryCache()->Update(this, old_size, size());
}

void Resource::FinishPendingClients() {
  // We're going to notify clients one by one. It is simple if the client does
  // nothing. However there are a couple other things that can happen.
  //
  // 1. Clients can be added during the loop. Make sure they are not processed.
  // 2. Clients can be removed during the loop. Make sure they are always
  //    available to be removed. Also don't call removed clients or add them
  //    back.
  //
  // Handle case (1) by saving a list of clients to notify. A separate list also
  // ensure a client is either in cliens_ or clients_awaiting_callback_.
  HeapVector<Member<ResourceClient>> clients_to_notify;
  CopyToVector(clients_awaiting_callback_, clients_to_notify);

  for (const auto& client : clients_to_notify) {
    // Handle case (2) to skip removed clients.
    if (!clients_awaiting_callback_.erase(client))
      continue;
    clients_.insert(client);

    // When revalidation starts after waiting clients are scheduled and
    // before they are added here. In such cases, we just add the clients
    // to |clients_| without DidAddClient(), as in Resource::AddClient().
    if (!is_revalidating_)
      DidAddClient(client);
  }

  // It is still possible for the above loop to finish a new client
  // synchronously. If there's no client waiting we should deschedule.
  bool scheduled = async_finish_pending_clients_task_.IsActive();
  if (scheduled && clients_awaiting_callback_.IsEmpty())
    async_finish_pending_clients_task_.Cancel();

  // Prevent the case when there are clients waiting but no callback scheduled.
  DCHECK(clients_awaiting_callback_.IsEmpty() || scheduled);
}

Resource::MatchStatus Resource::CanReuse(const FetchParameters& params) const {
  const ResourceRequest& new_request = params.GetResourceRequest();
  const ResourceLoaderOptions& new_options = params.Options();
  scoped_refptr<const SecurityOrigin> existing_origin =
      GetResourceRequest().RequestorOrigin();
  scoped_refptr<const SecurityOrigin> new_origin =
      new_request.RequestorOrigin();
  DCHECK_EQ(GetDataBufferingPolicy(), kBufferData);

  DCHECK(existing_origin);
  DCHECK(new_origin);

  // Never reuse opaque responses from a service worker for requests that are
  // not no-cors. https://crbug.com/625575
  // TODO(yhirano): Remove this.
  if (GetResponse().WasFetchedViaServiceWorker() &&
      GetResponse().GetType() == network::mojom::FetchResponseType::kOpaque &&
      new_request.GetFetchRequestMode() !=
          network::mojom::FetchRequestMode::kNoCORS) {
    return MatchStatus::kUnknownFailure;
  }

  // If credentials were sent with the previous request and won't be with this
  // one, or vice versa, re-fetch the resource.
  //
  // This helps with the case where the server sends back
  // "Access-Control-Allow-Origin: *" all the time, but some of the client's
  // requests are made without CORS and some with.
  if (GetResourceRequest().AllowStoredCredentials() !=
      new_request.AllowStoredCredentials()) {
    return MatchStatus::kRequestCredentialsModeDoesNotMatch;
  }

  // Certain requests (e.g., XHRs) might have manually set headers that require
  // revalidation. In theory, this should be a Revalidate case. In practice, the
  // MemoryCache revalidation path assumes a whole bunch of things about how
  // revalidation works that manual headers violate, so punt to Reload instead.
  //
  // Similarly, a request with manually added revalidation headers can lead to a
  // 304 response for a request that wasn't flagged as a revalidation attempt.
  // Normally, successful revalidation will maintain the original response's
  // status code, but for a manual revalidation the response code remains 304.
  // In this case, the Resource likely has insufficient context to provide a
  // useful cache hit or revalidation. See http://crbug.com/643659
  if (new_request.IsConditional() || response_.HttpStatusCode() == 304) {
    return MatchStatus::kUnknownFailure;
  }

  // Answers the question "can a separate request with different options be
  // re-used" (e.g. preload request). The safe (but possibly slow) answer is
  // always false.
  //
  // Data buffering policy differences are believed to be safe for re-use.
  //
  // TODO: Check content_security_policy_option.
  //
  // initiator_info is purely informational and should be benign for re-use.
  //
  // request_initiator_context is benign (indicates document vs. worker).

  // Reuse only if both the existing Resource and the new request are
  // asynchronous. Particularly,
  // 1. Sync and async Resource/requests shouldn't be mixed (crbug.com/652172),
  // 2. Sync existing Resources shouldn't be revalidated, and
  // 3. Sync new requests shouldn't revalidate existing Resources.
  //
  // 2. and 3. are because SyncResourceHandler handles redirects without
  // calling WillFollowRedirect, and causes response URL mismatch
  // (crbug.com/618967) and bypassing redirect restriction around revalidation
  // (crbug.com/613971 for 2. and crbug.com/614989 for 3.).
  if (new_options.synchronous_policy == kRequestSynchronously ||
      options_.synchronous_policy == kRequestSynchronously) {
    return MatchStatus::kSynchronousFlagDoesNotMatch;
  }

  if (resource_request_.GetKeepalive() || new_request.GetKeepalive())
    return MatchStatus::kKeepaliveSet;

  if (GetResourceRequest().HttpMethod() != new_request.HttpMethod())
    return MatchStatus::kRequestMethodDoesNotMatch;

  if (GetResourceRequest().HttpBody() != new_request.HttpBody())
    return MatchStatus::kUnknownFailure;


  // Don't reuse an existing resource when the source origin is different.
  if (!existing_origin->IsSameSchemeHostPort(new_origin.get()))
    return MatchStatus::kUnknownFailure;

  // securityOrigin has more complicated checks which callers are responsible
  // for.

  if (new_request.GetFetchCredentialsMode() !=
      resource_request_.GetFetchCredentialsMode()) {
    return MatchStatus::kRequestCredentialsModeDoesNotMatch;
  }

  const auto new_mode = new_request.GetFetchRequestMode();
  const auto existing_mode = resource_request_.GetFetchRequestMode();

  if (new_mode != existing_mode)
    return MatchStatus::kRequestModeDoesNotMatch;

  switch (new_mode) {
    case network::mojom::FetchRequestMode::kNoCORS:
    case network::mojom::FetchRequestMode::kNavigate:
      break;

    case network::mojom::FetchRequestMode::kCORS:
    case network::mojom::FetchRequestMode::kSameOrigin:
    case network::mojom::FetchRequestMode::kCORSWithForcedPreflight:
      // We have two separate CORS handling logics in ThreadableLoader
      // and ResourceLoader and sharing resources is difficult when they are
      // handled differently.
      if (options_.cors_handling_by_resource_fetcher !=
          new_options.cors_handling_by_resource_fetcher) {
        // If the existing one is handled in ThreadableLoader and the
        // new one is handled in ResourceLoader, reusing the existing one will
        // lead to CORS violations.
        if (!options_.cors_handling_by_resource_fetcher)
          return MatchStatus::kUnknownFailure;

        // Otherwise (i.e., if the existing one is handled in ResourceLoader
        // and the new one is handled in ThreadableLoader), reusing
        // the existing one will lead to double check which is harmless.
      }
      break;
  }

  return MatchStatus::kOk;
}

void Resource::Prune() {
  DestroyDecodedDataIfPossible();
}

void Resource::OnPurgeMemory() {
  Prune();
  if (!cache_handler_)
    return;
  cache_handler_->ClearCachedMetadata(CachedMetadataHandler::kCacheLocally);
}

void Resource::OnMemoryDump(WebMemoryDumpLevelOfDetail level_of_detail,
                            WebProcessMemoryDump* memory_dump) const {
  static const size_t kMaxURLReportLength = 128;
  static const int kMaxResourceClientToShowInMemoryInfra = 10;

  const String dump_name = GetMemoryDumpName();
  WebMemoryAllocatorDump* dump =
      memory_dump->CreateMemoryAllocatorDump(dump_name);
  dump->AddScalar("encoded_size", "bytes", encoded_size_memory_usage_);
  if (HasClientsOrObservers())
    dump->AddScalar("live_size", "bytes", encoded_size_memory_usage_);
  else
    dump->AddScalar("dead_size", "bytes", encoded_size_memory_usage_);

  if (data_)
    data_->OnMemoryDump(dump_name, memory_dump);

  if (level_of_detail == WebMemoryDumpLevelOfDetail::kDetailed) {
    String url_to_report = Url().GetString();
    if (url_to_report.length() > kMaxURLReportLength) {
      url_to_report.Truncate(kMaxURLReportLength);
      url_to_report = url_to_report + "...";
    }
    dump->AddString("url", "", url_to_report);

    dump->AddString("reason_not_deletable", "", ReasonNotDeletable());

    Vector<String> client_names;
    ResourceClientWalker<ResourceClient> walker(clients_);
    while (ResourceClient* client = walker.Next())
      client_names.push_back(client->DebugName());
    ResourceClientWalker<ResourceClient> walker2(clients_awaiting_callback_);
    while (ResourceClient* client = walker2.Next())
      client_names.push_back("(awaiting) " + client->DebugName());
    ResourceClientWalker<ResourceClient> walker3(finished_clients_);
    while (ResourceClient* client = walker3.Next())
      client_names.push_back("(finished) " + client->DebugName());
    std::sort(client_names.begin(), client_names.end(),
              WTF::CodePointCompareLessThan);

    StringBuilder builder;
    for (size_t i = 0;
         i < client_names.size() && i < kMaxResourceClientToShowInMemoryInfra;
         ++i) {
      if (i > 0)
        builder.Append(" / ");
      builder.Append(client_names[i]);
    }
    if (client_names.size() > kMaxResourceClientToShowInMemoryInfra) {
      builder.Append(" / and ");
      builder.AppendNumber(client_names.size() -
                           kMaxResourceClientToShowInMemoryInfra);
      builder.Append(" more");
    }
    dump->AddString("ResourceClient", "", builder.ToString());
  }

  const String overhead_name = dump_name + "/metadata";
  WebMemoryAllocatorDump* overhead_dump =
      memory_dump->CreateMemoryAllocatorDump(overhead_name);
  overhead_dump->AddScalar("size", "bytes", OverheadSize());
  memory_dump->AddSuballocation(
      overhead_dump->Guid(), String(WTF::Partitions::kAllocatedObjectPoolName));
}

String Resource::GetMemoryDumpName() const {
  return String::Format(
      "web_cache/%s_resources/%ld",
      ResourceTypeToString(GetType(), Options().initiator_info.name),
      identifier_);
}

void Resource::SetPreviewsState(WebURLRequest::PreviewsState previews_state) {
  resource_request_.SetPreviewsState(previews_state);
}

void Resource::ClearRangeRequestHeader() {
  resource_request_.ClearHTTPHeaderField("range");
}

void Resource::RevalidationSucceeded(
    const ResourceResponse& validating_response) {
  SECURITY_CHECK(redirect_chain_.IsEmpty());
  SECURITY_CHECK(EqualIgnoringFragmentIdentifier(validating_response.Url(),
                                                 GetResponse().Url()));
  response_.SetResourceLoadTiming(validating_response.GetResourceLoadTiming());

  // RFC2616 10.3.5
  // Update cached headers from the 304 response
  const HTTPHeaderMap& new_headers = validating_response.HttpHeaderFields();
  for (const auto& header : new_headers) {
    // Entity headers should not be sent by servers when generating a 304
    // response; misconfigured servers send them anyway. We shouldn't allow such
    // headers to update the original request. We'll base this on the list
    // defined by RFC2616 7.1, with a few additions for extension headers we
    // care about.
    if (!ShouldUpdateHeaderAfterRevalidation(header.key))
      continue;
    response_.SetHTTPHeaderField(header.key, header.value);
  }

  is_revalidating_ = false;
}

void Resource::RevalidationFailed() {
  SECURITY_CHECK(redirect_chain_.IsEmpty());
  ClearData();
  cache_handler_.Clear();
  integrity_disposition_ = ResourceIntegrityDisposition::kNotChecked;
  integrity_report_info_.Clear();
  DestroyDecodedDataForFailedRevalidation();
  is_revalidating_ = false;
}

void Resource::MarkAsPreload() {
  DCHECK(!is_unused_preload_);
  is_unused_preload_ = true;
}

bool Resource::MatchPreload(const FetchParameters& params,
                            base::SingleThreadTaskRunner*) {
  DCHECK(is_unused_preload_);
  is_unused_preload_ = false;
  return true;
}

bool Resource::CanReuseRedirectChain() const {
  for (auto& redirect : redirect_chain_) {
    if (!CanUseResponse(redirect.redirect_response_, false /*allow_stale*/,
                        response_timestamp_))
      return false;
    if (redirect.request_.CacheControlContainsNoCache() ||
        redirect.request_.CacheControlContainsNoStore())
      return false;
  }
  return true;
}

bool Resource::HasCacheControlNoStoreHeader() const {
  return GetResponse().CacheControlContainsNoStore() ||
         GetResourceRequest().CacheControlContainsNoStore();
}

bool Resource::MustReloadDueToVaryHeader(
    const ResourceRequest& new_request) const {
  const AtomicString& vary = GetResponse().HttpHeaderField(HTTPNames::Vary);
  if (vary.IsNull())
    return false;
  if (vary == "*")
    return true;

  CommaDelimitedHeaderSet vary_headers;
  ParseCommaDelimitedHeader(vary, vary_headers);
  for (const String& header : vary_headers) {
    AtomicString atomic_header(header);
    if (GetResourceRequest().HttpHeaderField(atomic_header) !=
        new_request.HttpHeaderField(atomic_header)) {
      return true;
    }
  }
  return false;
}

bool Resource::MustRevalidateDueToCacheHeaders(bool allow_stale) const {
  return !CanUseResponse(GetResponse(), allow_stale, response_timestamp_) ||
         GetResourceRequest().CacheControlContainsNoCache() ||
         GetResourceRequest().CacheControlContainsNoStore();
}

static bool ShouldRevalidateStaleResponse(const ResourceRequest& request,
                                          const ResourceResponse& response,
                                          double response_timestamp) {
  double staleness = response.CacheControlStaleWhileRevalidate();
  if (staleness == 0)
    return false;

  return CurrentAge(response, response_timestamp) >
         FreshnessLifetime(response, response_timestamp);
}

bool Resource::ShouldRevalidateStaleResponse() const {
  for (auto& redirect : redirect_chain_) {
    // Use |response_timestamp_| since we don't store the timestamp
    // of each redirect response.
    if (blink::ShouldRevalidateStaleResponse(redirect.request_,
                                             redirect.redirect_response_,
                                             response_timestamp_)) {
      return true;
    }
  }

  return blink::ShouldRevalidateStaleResponse(
      GetResourceRequest(), GetResponse(), response_timestamp_);
}

bool Resource::StaleRevalidationRequested() const {
  if (GetResponse().AsyncRevalidationRequested())
    return true;

  for (auto& redirect : redirect_chain_) {
    if (redirect.redirect_response_.AsyncRevalidationRequested())
      return true;
  }
  return false;
}

bool Resource::CanUseCacheValidator() const {
  if (IsLoading() || ErrorOccurred())
    return false;

  if (HasCacheControlNoStoreHeader())
    return false;

  // Do not revalidate Resource with redirects. https://crbug.com/613971
  if (!RedirectChain().IsEmpty())
    return false;

  return GetResponse().HasCacheValidatorFields() ||
         GetResourceRequest().HasCacheValidatorFields();
}

size_t Resource::CalculateOverheadSize() const {
  static const int kAverageClientsHashMapSize = 384;
  return sizeof(Resource) + GetResponse().MemoryUsage() +
         kAverageClientsHashMapSize +
         GetResourceRequest().Url().GetString().length() * 2;
}

void Resource::DidChangePriority(ResourceLoadPriority load_priority,
                                 int intra_priority_value) {
  resource_request_.SetPriority(load_priority, intra_priority_value);
  if (loader_)
    loader_->DidChangePriority(load_priority, intra_priority_value);
}

// TODO(toyoshim): Consider to generate automatically. https://crbug.com/675515.
static const char* InitiatorTypeNameToString(
    const AtomicString& initiator_type_name) {
  if (initiator_type_name == FetchInitiatorTypeNames::audio)
    return "Audio";
  if (initiator_type_name == FetchInitiatorTypeNames::css)
    return "CSS resource";
  if (initiator_type_name == FetchInitiatorTypeNames::document)
    return "Document";
  if (initiator_type_name == FetchInitiatorTypeNames::icon)
    return "Icon";
  if (initiator_type_name == FetchInitiatorTypeNames::internal)
    return "Internal resource";
  if (initiator_type_name == FetchInitiatorTypeNames::fetch)
    return "Fetch";
  if (initiator_type_name == FetchInitiatorTypeNames::link)
    return "Link element resource";
  if (initiator_type_name == FetchInitiatorTypeNames::other)
    return "Other resource";
  if (initiator_type_name == FetchInitiatorTypeNames::processinginstruction)
    return "Processing instruction";
  if (initiator_type_name == FetchInitiatorTypeNames::track)
    return "Track";
  if (initiator_type_name == FetchInitiatorTypeNames::uacss)
    return "User Agent CSS resource";
  if (initiator_type_name == FetchInitiatorTypeNames::video)
    return "Video";
  if (initiator_type_name == FetchInitiatorTypeNames::xml)
    return "XML resource";
  if (initiator_type_name == FetchInitiatorTypeNames::xmlhttprequest)
    return "XMLHttpRequest";

  static_assert(
      FetchInitiatorTypeNames::FetchInitiatorTypeNamesCount == 17,
      "New FetchInitiatorTypeNames should be handled correctly here.");

  return "Resource";
}

const char* Resource::ResourceTypeToString(
    ResourceType type,
    const AtomicString& fetch_initiator_name) {
  switch (type) {
    case ResourceType::kMainResource:
      return "Main resource";
    case ResourceType::kImage:
      return "Image";
    case ResourceType::kCSSStyleSheet:
      return "CSS stylesheet";
    case ResourceType::kScript:
      return "Script";
    case ResourceType::kFont:
      return "Font";
    case ResourceType::kRaw:
      return InitiatorTypeNameToString(fetch_initiator_name);
    case ResourceType::kSVGDocument:
      return "SVG document";
    case ResourceType::kXSLStyleSheet:
      return "XSL stylesheet";
    case ResourceType::kLinkPrefetch:
      return "Link prefetch resource";
    case ResourceType::kTextTrack:
      return "Text track";
    case ResourceType::kImportResource:
      return "Imported resource";
    case ResourceType::kAudio:
      return "Audio";
    case ResourceType::kVideo:
      return "Video";
    case ResourceType::kManifest:
      return "Manifest";
    case ResourceType::kMock:
      return "Mock";
  }
  NOTREACHED();
  return InitiatorTypeNameToString(fetch_initiator_name);
}

bool Resource::ShouldBlockLoadEvent() const {
  return !link_preload_ && IsLoadEventBlockingResourceType();
}

bool Resource::IsLoadEventBlockingResourceType() const {
  switch (type_) {
    case ResourceType::kMainResource:
    case ResourceType::kImage:
    case ResourceType::kCSSStyleSheet:
    case ResourceType::kScript:
    case ResourceType::kFont:
    case ResourceType::kSVGDocument:
    case ResourceType::kXSLStyleSheet:
    case ResourceType::kImportResource:
      return true;
    case ResourceType::kRaw:
    case ResourceType::kLinkPrefetch:
    case ResourceType::kTextTrack:
    case ResourceType::kAudio:
    case ResourceType::kVideo:
    case ResourceType::kManifest:
    case ResourceType::kMock:
      return false;
  }
  NOTREACHED();
  return false;
}

}  // namespace blink
