// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GCM_GCM_PROFILE_SERVICE_FACTORY_H_
#define CHROME_BROWSER_GCM_GCM_PROFILE_SERVICE_FACTORY_H_

#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/memory/singleton.h"
#include "components/gcm_driver/system_encryptor.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"

namespace gcm {

class GCMProfileService;

// Singleton that owns all GCMProfileService and associates them with
// Profiles.
class GCMProfileServiceFactory : public BrowserContextKeyedServiceFactory {
 public:
  static GCMProfileService* GetForProfile(content::BrowserContext* profile);
  static GCMProfileServiceFactory* GetInstance();
  static void SetGlobalTestingFactory(TestingFactoryFunction factory);

 private:
  friend struct base::DefaultSingletonTraits<GCMProfileServiceFactory>;

  GCMProfileServiceFactory();
  ~GCMProfileServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  KeyedService* BuildServiceInstanceFor(
      content::BrowserContext* profile) const override;
  content::BrowserContext* GetBrowserContextToUse(
      content::BrowserContext* context) const override;

  static TestingFactoryFunction testing_factory_;

  DISALLOW_COPY_AND_ASSIGN(GCMProfileServiceFactory);
};

}  // namespace gcm

#endif  // CHROME_BROWSER_GCM_GCM_PROFILE_SERVICE_FACTORY_H_
