// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gcm/gcm_profile_service_factory.h"
#include <memory>

#include "base/sequenced_task_runner.h"
#include "base/task/post_task.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/incognito_helpers.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/identity_manager_factory.h"
#include "components/gcm_driver/gcm_profile_service.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/offline_pages/buildflags/buildflags.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/network_service_instance.h"

#if !defined(OS_ANDROID)
#include "chrome/browser/gcm/gcm_product_util.h"
#include "chrome/common/channel_info.h"
#include "components/gcm_driver/gcm_client_factory.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#endif

#if BUILDFLAG(ENABLE_OFFLINE_PAGES)
#include "chrome/browser/offline_pages/prefetch/prefetch_service_factory.h"
#include "components/gcm_driver/gcm_driver.h"
#include "components/offline_pages/core/offline_page_feature.h"
#include "components/offline_pages/core/prefetch/prefetch_gcm_app_handler.h"
#include "components/offline_pages/core/prefetch/prefetch_service.h"
#endif

namespace gcm {

namespace {

#if !defined(OS_ANDROID)
// Requests a ProxyResolvingSocketFactoryPtr on the UI thread. Note that a
// WeakPtr of GCMProfileService is needed to detect when the KeyedService shuts
// down, and avoid calling into |profile| which might have also been destroyed.
void RequestProxyResolvingSocketFactoryOnUIThread(
    Profile* profile,
    base::WeakPtr<GCMProfileService> service,
    network::mojom::ProxyResolvingSocketFactoryRequest request) {
  if (!service)
    return;
  network::mojom::NetworkContext* network_context =
      content::BrowserContext::GetDefaultStoragePartition(profile)
          ->GetNetworkContext();
  network_context->CreateProxyResolvingSocketFactory(std::move(request));
}

// A thread-safe wrapper to request a ProxyResolvingSocketFactoryPtr.
void RequestProxyResolvingSocketFactory(
    Profile* profile,
    base::WeakPtr<GCMProfileService> service,
    network::mojom::ProxyResolvingSocketFactoryRequest request) {
  base::PostTaskWithTraits(
      FROM_HERE, {content::BrowserThread::UI},
      base::BindOnce(&RequestProxyResolvingSocketFactoryOnUIThread, profile,
                     std::move(service), std::move(request)));
}
#endif

}  // namespace

BrowserContextKeyedServiceFactory::TestingFactoryFunction
    GCMProfileServiceFactory::testing_factory_ = nullptr;

// static
GCMProfileService* GCMProfileServiceFactory::GetForProfile(
    content::BrowserContext* profile) {
  // GCM is not supported in incognito mode.
  if (profile->IsOffTheRecord())
    return NULL;

  return static_cast<GCMProfileService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

// static
GCMProfileServiceFactory* GCMProfileServiceFactory::GetInstance() {
  return base::Singleton<GCMProfileServiceFactory>::get();
}

// static
void GCMProfileServiceFactory::SetGlobalTestingFactory(
    BrowserContextKeyedServiceFactory::TestingFactoryFunction factory) {
  testing_factory_ = factory;
}

GCMProfileServiceFactory::GCMProfileServiceFactory()
    : BrowserContextKeyedServiceFactory(
        "GCMProfileService",
        BrowserContextDependencyManager::GetInstance()) {
  DependsOn(IdentityManagerFactory::GetInstance());
#if BUILDFLAG(ENABLE_OFFLINE_PAGES)
  DependsOn(offline_pages::PrefetchServiceFactory::GetInstance());
#endif  // BUILDFLAG(ENABLE_OFFLINE_PAGES)
}

GCMProfileServiceFactory::~GCMProfileServiceFactory() {
}

KeyedService* GCMProfileServiceFactory::BuildServiceInstanceFor(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  DCHECK(!profile->IsOffTheRecord());

  if (testing_factory_)
    return testing_factory_(context).release();

  scoped_refptr<base::SequencedTaskRunner> blocking_task_runner(
      base::CreateSequencedTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}));
  std::unique_ptr<GCMProfileService> service = nullptr;
#if defined(OS_ANDROID)
  service = base::WrapUnique(
      new GCMProfileService(profile->GetPath(), blocking_task_runner));
#else
  service = std::make_unique<GCMProfileService>(
      profile->GetPrefs(), profile->GetPath(),
      base::BindRepeating(&RequestProxyResolvingSocketFactory, profile),
      content::BrowserContext::GetDefaultStoragePartition(profile)
          ->GetURLLoaderFactoryForBrowserProcess(),
      content::GetNetworkConnectionTracker(), chrome::GetChannel(),
      gcm::GetProductCategoryForSubtypes(profile->GetPrefs()),
      IdentityManagerFactory::GetForProfile(profile),
      std::unique_ptr<GCMClientFactory>(new GCMClientFactory),
      base::CreateSingleThreadTaskRunnerWithTraits(
          {content::BrowserThread::UI}),
      base::CreateSingleThreadTaskRunnerWithTraits(
          {content::BrowserThread::IO}),
      blocking_task_runner);
#endif
#if BUILDFLAG(ENABLE_OFFLINE_PAGES)
  offline_pages::PrefetchService* prefetch_service =
      offline_pages::PrefetchServiceFactory::GetForBrowserContext(context);
  if (prefetch_service != nullptr) {
    offline_pages::PrefetchGCMHandler* prefetch_gcm_handler =
        prefetch_service->GetPrefetchGCMHandler();
    service->driver()->AddAppHandler(prefetch_gcm_handler->GetAppId(),
                                     prefetch_gcm_handler->AsGCMAppHandler());
  }
#endif  // BUILDFLAG(ENABLE_OFFLINE_PAGES)

  return service.release();
}

content::BrowserContext* GCMProfileServiceFactory::GetBrowserContextToUse(
    content::BrowserContext* context) const {
  return chrome::GetBrowserContextOwnInstanceInIncognito(context);
}

}  // namespace gcm
