// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_TEST_UTILS_H_
#define CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_TEST_UTILS_H_

#include <memory>

#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/memory/weak_ptr.h"
#include "base/task/post_task.h"
#include "content/browser/service_worker/service_worker_database.h"
#include "content/common/service_worker/service_worker_provider.mojom.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/common/content_switches.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"

namespace net {

class HttpResponseInfo;

}  // namespace net

namespace content {

class ServiceWorkerContextCore;
class ServiceWorkerProviderHost;
class ServiceWorkerStorage;
class ServiceWorkerVersion;

template <typename Arg>
void ReceiveResult(BrowserThread::ID run_quit_thread,
                   base::OnceClosure quit,
                   base::Optional<Arg>* out,
                   Arg actual) {
  *out = actual;
  if (!quit.is_null())
    base::PostTaskWithTraits(FROM_HERE, {run_quit_thread}, std::move(quit));
}

template <typename Arg>
base::OnceCallback<void(Arg)> CreateReceiver(BrowserThread::ID run_quit_thread,
                                             base::OnceClosure quit,
                                             base::Optional<Arg>* out) {
  return base::BindOnce(&ReceiveResult<Arg>, run_quit_thread, std::move(quit),
                        out);
}

template <typename Arg>
base::OnceCallback<void(Arg)> CreateReceiverOnCurrentThread(
    base::Optional<Arg>* out,
    base::OnceClosure quit = base::OnceClosure()) {
  BrowserThread::ID id;
  bool ret = BrowserThread::GetCurrentThreadIdentifier(&id);
  DCHECK(ret);
  return CreateReceiver(id, std::move(quit), out);
}

// Container for keeping the Mojo connection to the service worker provider on
// the renderer alive.
class ServiceWorkerRemoteProviderEndpoint {
 public:
  ServiceWorkerRemoteProviderEndpoint();
  ServiceWorkerRemoteProviderEndpoint(
      ServiceWorkerRemoteProviderEndpoint&& other);
  ~ServiceWorkerRemoteProviderEndpoint();

  void BindWithProviderHostInfo(mojom::ServiceWorkerProviderHostInfoPtr* info);
  void BindWithProviderInfo(
      mojom::ServiceWorkerProviderInfoForStartWorkerPtr info);

  mojom::ServiceWorkerContainerHostAssociatedPtr* host_ptr() {
    return &host_ptr_;
  }

  mojom::ServiceWorkerContainerAssociatedRequest* client_request() {
    return &client_request_;
  }

 private:
  // Bound with content::ServiceWorkerProviderHost. The provider host will be
  // removed asynchronously when this pointer is closed.
  mojom::ServiceWorkerContainerHostAssociatedPtr host_ptr_;
  // This is the other end of ServiceWorkerContainerAssociatedPtr owned by
  // content::ServiceWorkerProviderHost.
  mojom::ServiceWorkerContainerAssociatedRequest client_request_;

  DISALLOW_COPY_AND_ASSIGN(ServiceWorkerRemoteProviderEndpoint);
};

mojom::ServiceWorkerProviderHostInfoPtr CreateProviderHostInfoForWindow(
    int provider_id,
    int route_id);

std::unique_ptr<ServiceWorkerProviderHost> CreateProviderHostForWindow(
    int process_id,
    int provider_id,
    bool is_parent_frame_secure,
    base::WeakPtr<ServiceWorkerContextCore> context,
    ServiceWorkerRemoteProviderEndpoint* output_endpoint);

base::WeakPtr<ServiceWorkerProviderHost>
CreateProviderHostForServiceWorkerContext(
    int process_id,
    bool is_parent_frame_secure,
    ServiceWorkerVersion* hosted_version,
    base::WeakPtr<ServiceWorkerContextCore> context,
    ServiceWorkerRemoteProviderEndpoint* output_endpoint);

// Writes the script down to |storage| synchronously. This should not be used in
// base::RunLoop since base::RunLoop is used internally to wait for completing
// all of tasks. If it's in another base::RunLoop, consider to use
// WriteToDiskCacheAsync().
ServiceWorkerDatabase::ResourceRecord WriteToDiskCacheSync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    const std::string& meta_data);

// Writes the script with custom net::HttpResponseInfo down to |storage|
// synchronously. This should not be used in base::RunLoop since base::RunLoop
// is used internally to wait for completing all of tasks. If it's in another
// base::RunLoop, consider to use WriteToDiskCacheWithCustomResponseInfoAsync().
ServiceWorkerDatabase::ResourceRecord
WriteToDiskCacheWithCustomResponseInfoSync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    std::unique_ptr<net::HttpResponseInfo> http_info,
    const std::string& body,
    const std::string& meta_data);

// Writes the script down to |storage| asynchronously. When completing tasks,
// |callback| will be called. You must wait for |callback| instead of
// base::RunUntilIdle because wiriting to the storage might happen on another
// thread and base::RunLoop could get idle before writes has not finished yet.
ServiceWorkerDatabase::ResourceRecord WriteToDiskCacheAsync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    const std::string& meta_data,
    base::OnceClosure callback);

// Writes the script with custom net::HttpResponseInfo down to |storage|
// asynchronously. When completing tasks, |callback| will be called. You must
// wait for |callback| instead of base::RunUntilIdle because wiriting to the
// storage might happen on another thread and base::RunLoop could get idle
// before writes has not finished yet.
ServiceWorkerDatabase::ResourceRecord
WriteToDiskCacheWithCustomResponseInfoAsync(
    ServiceWorkerStorage* storage,
    const GURL& script_url,
    int64_t resource_id,
    std::unique_ptr<net::HttpResponseInfo> http_info,
    const std::string& body,
    const std::string& meta_data,
    base::OnceClosure callback);

}  // namespace content

#endif  // CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_TEST_UTILS_H_
