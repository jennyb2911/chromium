// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/code_cache_host_impl.h"

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/task/post_task.h"
#include "base/threading/thread.h"
#include "build/build_config.h"
#include "content/browser/cache_storage/cache_storage_cache.h"
#include "content/browser/cache_storage/cache_storage_cache_handle.h"
#include "content/browser/cache_storage/cache_storage_context_impl.h"
#include "content/browser/cache_storage/cache_storage_manager.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/code_cache/generated_code_cache.h"
#include "content/browser/code_cache/generated_code_cache_context.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/resource_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_features.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "net/base/io_buffer.h"
#include "url/gurl.h"
#include "url/origin.h"

using blink::mojom::CacheStorageError;

namespace content {

namespace {

void NoOpCacheStorageErrorCallback(CacheStorageCacheHandle cache_handle,
                                   CacheStorageError error) {}

base::Optional<url::Origin> GetRendererOrigin(const GURL& url,
                                              int render_process_id) {
  GURL requesting_url =
      ChildProcessSecurityPolicyImpl::GetInstance()->GetOriginLock(
          render_process_id);

  if (!requesting_url.is_valid() || !url.is_valid())
    return base::nullopt;

  url::Origin origin = url::Origin::Create(requesting_url);

  // Don't cache the code corresponding to unique origins. The same-origin
  // checks should always fail for unique origins but the serialized value of
  // unique origins does not ensure this.
  if (origin.unique())
    return base::nullopt;

  return origin;
}

}  // namespace

CodeCacheHostImpl::CodeCacheHostImpl(
    int render_process_id,
    scoped_refptr<CacheStorageContextImpl> cache_storage_context,
    scoped_refptr<GeneratedCodeCacheContext> generated_code_cache_context)
    : render_process_id_(render_process_id),
      cache_storage_context_(std::move(cache_storage_context)),
      generated_code_cache_context_(std::move(generated_code_cache_context)),
      weak_ptr_factory_(this) {}

CodeCacheHostImpl::~CodeCacheHostImpl() {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
}

// static
void CodeCacheHostImpl::Create(
    int render_process_id,
    scoped_refptr<CacheStorageContextImpl> cache_storage_context,
    scoped_refptr<GeneratedCodeCacheContext> generated_code_cache_context,
    blink::mojom::CodeCacheHostRequest request) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  mojo::MakeStrongBinding(
      std::make_unique<CodeCacheHostImpl>(
          render_process_id, std::move(cache_storage_context),
          std::move(generated_code_cache_context)),
      std::move(request));
}

void CodeCacheHostImpl::DidGenerateCacheableMetadata(
    const GURL& url,
    base::Time expected_response_time,
    const std::vector<uint8_t>& data) {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    mojo::ReportBadMessage("Invalid URL scheme for code cache.");
    return;
  }

  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (!base::FeatureList::IsEnabled(features::kIsolatedCodeCache)) {
    base::PostTaskWithTraits(
        FROM_HERE, {BrowserThread::UI},
        base::BindOnce(&CodeCacheHostImpl::DidGenerateCacheableMetadataOnUI,
                       render_process_id_, url, expected_response_time, data));
  } else {
    if (!generated_code_cache_context_ ||
        !generated_code_cache_context_->generated_code_cache())
      return;

    base::Optional<url::Origin> requesting_origin =
        GetRendererOrigin(url, render_process_id_);
    if (!requesting_origin)
      return;

    generated_code_cache_context_->generated_code_cache()->WriteData(
        url, *requesting_origin, expected_response_time, data);
  }
}

void CodeCacheHostImpl::FetchCachedCode(const GURL& url,
                                        FetchCachedCodeCallback callback) {
  if (!generated_code_cache_context_ ||
      !generated_code_cache_context_->generated_code_cache()) {
    std::move(callback).Run(base::Time(), std::vector<uint8_t>());
    return;
  }

  base::Optional<url::Origin> requesting_origin =
      GetRendererOrigin(url, render_process_id_);
  if (!requesting_origin) {
    std::move(callback).Run(base::Time(), std::vector<uint8_t>());
    return;
  }

  auto read_callback = base::BindRepeating(
      &CodeCacheHostImpl::OnReceiveCachedCode, weak_ptr_factory_.GetWeakPtr(),
      base::Passed(&callback));
  generated_code_cache_context_->generated_code_cache()->FetchEntry(
      url, *requesting_origin, read_callback);
}

void CodeCacheHostImpl::ClearCodeCacheEntry(const GURL& url) {
  if (!generated_code_cache_context_ ||
      !generated_code_cache_context_->generated_code_cache())
    return;

  base::Optional<url::Origin> requesting_origin =
      GetRendererOrigin(url, render_process_id_);
  if (!requesting_origin)
    return;

  generated_code_cache_context_->generated_code_cache()->DeleteEntry(
      url, *requesting_origin);
}

void CodeCacheHostImpl::DidGenerateCacheableMetadataInCacheStorage(
    const GURL& url,
    base::Time expected_response_time,
    const std::vector<uint8_t>& data,
    const url::Origin& cache_storage_origin,
    const std::string& cache_storage_cache_name) {
  scoped_refptr<net::IOBuffer> buf =
      base::MakeRefCounted<net::IOBuffer>(data.size());
  if (!data.empty())
    memcpy(buf->data(), &data.front(), data.size());

  cache_storage_context_->cache_manager()->OpenCache(
      cache_storage_origin, CacheStorageOwner::kCacheAPI,
      cache_storage_cache_name,
      base::BindOnce(&CodeCacheHostImpl::OnCacheStorageOpenCallback,
                     weak_ptr_factory_.GetWeakPtr(), url,
                     expected_response_time, buf, data.size()));
}

void CodeCacheHostImpl::OnReceiveCachedCode(FetchCachedCodeCallback callback,
                                            const base::Time& response_time,
                                            const std::vector<uint8_t>& data) {
  // TODO(crbug.com/867848): Pass the data as a mojo data pipe instead
  // of vector<uint8>
  std::move(callback).Run(response_time, data);
}

void CodeCacheHostImpl::OnCacheStorageOpenCallback(
    const GURL& url,
    base::Time expected_response_time,
    scoped_refptr<net::IOBuffer> buf,
    int buf_len,
    CacheStorageCacheHandle cache_handle,
    CacheStorageError error) {
  if (error != CacheStorageError::kSuccess || !cache_handle.value())
    return;
  CacheStorageCache* cache = cache_handle.value();
  cache->WriteSideData(
      base::BindOnce(&NoOpCacheStorageErrorCallback, std::move(cache_handle)),
      url, expected_response_time, buf, buf_len);
}

// static
void CodeCacheHostImpl::DidGenerateCacheableMetadataOnUI(
    int render_process_id,
    const GURL& url,
    base::Time expected_response_time,
    const std::vector<uint8_t>& data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  RenderProcessHost* host = RenderProcessHost::FromID(render_process_id);
  if (!host)
    return;

  // Use the same priority for the metadata write as for script
  // resources (see defaultPriorityForResourceType() in WebKit's
  // CachedResource.cpp). Note that WebURLRequest::PriorityMedium
  // corresponds to net::LOW (see ConvertWebKitPriorityToNetPriority()
  // in weburlloader_impl.cc).
  const net::RequestPriority kPriority = net::LOW;
  host->GetStoragePartition()->GetNetworkContext()->WriteCacheMetadata(
      url, kPriority, expected_response_time, data);
}

}  // namespace content
