// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NET_PROXY_CONFIG_MONITOR_H_
#define CHROME_BROWSER_NET_PROXY_CONFIG_MONITOR_H_

#include <memory>

#include "base/macros.h"
#include "build/buildflag.h"
#include "extensions/buildflags/buildflags.h"
#include "mojo/public/cpp/bindings/binding_set.h"
#include "mojo/public/cpp/bindings/interface_ptr_set.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "services/network/public/mojom/proxy_config.mojom.h"

namespace net {
class ProxyConfigWithAnnotation;
}

class Profile;
class PrefProxyConfigTracker;

// Tracks the ProxyConfig to use, and passes any updates to a NetworkContext's
// ProxyConfigClient. This also responds to errors related to proxy settings
// from the NetworkContext, and forwards them any listening Chrome extensions
// associated with the profile.
class ProxyConfigMonitor : public net::ProxyConfigService::Observer,
                           public network::mojom::ProxyConfigPollerClient
#if BUILDFLAG(ENABLE_EXTENSIONS)
    ,
                           public network::mojom::ProxyErrorClient
#endif

{
 public:
  // Creates a ProxyConfigMonitor that gets proxy settings from |profile| and
  // watches for changes. The created ProxyConfigMonitor must be destroyed
  // before |profile|.
  explicit ProxyConfigMonitor(Profile* profile);

  // Creates a ProxyConfigMonitor that gets proxy settings from the
  // BrowserProcess's |local_state_|, for use with NetworkContexts not
  // assocaited with a profile. Must be destroyed before the BrowserProcess's
  // |local_state_|.
  ProxyConfigMonitor();

  ~ProxyConfigMonitor() override;

  // Populates proxy-related fields of |network_context_params|. Updated
  // ProxyConfigs will be sent to a NetworkContext created with those params
  // whenever the configuration changes. Can be called more than once to inform
  // multiple NetworkContexts of proxy changes.
  void AddToNetworkContextParams(
      network::mojom::NetworkContextParams* network_context_params);

  // Flushes all pending data on the pipe, blocking the current thread until
  // they're received, to allow tests to wait until all pending proxy
  // configuration changes have been applied.
  void FlushForTesting();

 private:
  // net::ProxyConfigService::Observer implementation:
  void OnProxyConfigChanged(
      const net::ProxyConfigWithAnnotation& config,
      net::ProxyConfigService::ConfigAvailability availability) override;

  // network::mojom::ProxyConfigPollerClient implementation:
  void OnLazyProxyConfigPoll() override;

#if BUILDFLAG(ENABLE_EXTENSIONS)
  // network::mojom::ProxyErrorClient implementation:
  void OnPACScriptError(int32_t line_number,
                        const std::string& details) override;
  void OnRequestMaybeFailedDueToProxySettings(int32_t net_error) override;
#endif

  std::unique_ptr<net::ProxyConfigService> proxy_config_service_;
  // Monitors global and Profile prefs related to proxy configuration.
  std::unique_ptr<PrefProxyConfigTracker> pref_proxy_config_tracker_;

  mojo::BindingSet<network::mojom::ProxyConfigPollerClient> poller_binding_set_;

  mojo::InterfacePtrSet<network::mojom::ProxyConfigClient>
      proxy_config_client_set_;

#if BUILDFLAG(ENABLE_EXTENSIONS)
  mojo::BindingSet<network::mojom::ProxyErrorClient> error_binding_set_;
  Profile* profile_ = nullptr;
#endif

  DISALLOW_COPY_AND_ASSIGN(ProxyConfigMonitor);
};

#endif  // CHROME_BROWSER_NET_PROXY_CONFIG_MONITOR_H_
