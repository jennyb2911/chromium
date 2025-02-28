// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_TEST_BASE_TESTING_PROFILE_H_
#define CHROME_TEST_BASE_TESTING_PROFILE_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/optional.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "components/domain_reliability/clear_mode.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "extensions/buildflags/buildflags.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/network_service.mojom.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/settings/scoped_cros_settings_test_helper.h"
#endif

class BrowserContextDependencyManager;
class ExtensionSpecialStoragePolicy;
class HostContentSettingsMap;

namespace content {
class MockResourceContext;
class SSLHostStateDelegate;
#if !defined(OS_ANDROID)
class ZoomLevelDelegate;
#endif  // !defined(OS_ANDROID)
}

namespace net {
class URLRequestContextGetter;
}

namespace policy {
class PolicyService;
class ProfilePolicyConnector;
class SchemaRegistryService;
}

namespace storage {
class SpecialStoragePolicy;
}

namespace sync_preferences {
class PrefServiceSyncable;
class TestingPrefServiceSyncable;
}

class TestingProfile : public Profile {
 public:
  // Profile directory name for the test user. This is "Default" on most
  // platforms but must be different on ChromeOS because a logged-in user cannot
  // use "Default" as profile directory.
  // Browser- and UI tests should always use this to get to the user's profile
  // directory. Unit-tests, though, should use |kInitialProfile|, which is
  // always "Default", because they are runnining without logged-in user.
  static const char kTestUserProfileDir[];

  // Default constructor that cannot be used with multi-profiles.
  TestingProfile();

  using TestingFactories =
      std::vector<std::pair<BrowserContextKeyedServiceFactory*,
                            BrowserContextKeyedServiceFactory::TestingFactory>>;

  // Helper class for building an instance of TestingProfile (allows injecting
  // mocks for various services prior to profile initialization).
  // TODO(atwilson): Remove non-default constructors and various setters in
  // favor of using the Builder API.
  class Builder {
   public:
    Builder();
    ~Builder();

    // Sets a Delegate to be called back during profile init. This causes the
    // final initialization to be performed via a task so the caller must run
    // a MessageLoop. Caller maintains ownership of the Delegate
    // and must manage its lifetime so it continues to exist until profile
    // initialization is complete.
    void SetDelegate(Delegate* delegate);

    // Adds a testing factory to the TestingProfile. These testing factories
    // are applied before the ProfileKeyedServices are created.
    // DEPRECATED: use TestingFactory version instead, see
    // http://crbug.com/809610
    void AddTestingFactory(
        BrowserContextKeyedServiceFactory* service_factory,
        BrowserContextKeyedServiceFactory::TestingFactoryFunction function);

    // Adds a testing factory to the TestingProfile. These testing factories
    // are applied before the ProfileKeyedServices are created.
    void AddTestingFactory(
        BrowserContextKeyedServiceFactory* service_factory,
        BrowserContextKeyedServiceFactory::TestingFactory testing_factory);

#if BUILDFLAG(ENABLE_EXTENSIONS)
    // Sets the ExtensionSpecialStoragePolicy to be returned by
    // GetExtensionSpecialStoragePolicy().
    void SetExtensionSpecialStoragePolicy(
        scoped_refptr<ExtensionSpecialStoragePolicy> policy);
#endif

    // Sets the path to the directory to be used to hold profile data.
    void SetPath(const base::FilePath& path);

    // Sets the PrefService to be used by this profile.
    void SetPrefService(
        std::unique_ptr<sync_preferences::PrefServiceSyncable> prefs);

    // Makes the Profile being built a guest profile.
    void SetGuestSession();

    // Override the default behavior of is_new_profile to return the provided
    // value.
    void OverrideIsNewProfile(bool is_new_profile);

    // Sets the supervised user ID (which is empty by default). If it is set to
    // a non-empty string, the profile is supervised.
    void SetSupervisedUserId(const std::string& supervised_user_id);

    // Sets the PolicyService to be used by this profile.
    void SetPolicyService(
        std::unique_ptr<policy::PolicyService> policy_service);

    // Sets the UserProfileName to be used by this profile.
    void SetProfileName(const std::string& profile_name);

    // Creates the TestingProfile using previously-set settings.
    std::unique_ptr<TestingProfile> Build();

    // Build an incognito profile, owned by |original_profile|. Note: unless you
    // need to customize the Builder, or access TestingProfile member functions,
    // you can use original_profile->GetOffTheRecordProfile().
    TestingProfile* BuildIncognito(TestingProfile* original_profile);

   private:
    // If true, Build() has already been called.
    bool build_called_;

    // Various staging variables where values are held until Build() is invoked.
    std::unique_ptr<sync_preferences::PrefServiceSyncable> pref_service_;
#if BUILDFLAG(ENABLE_EXTENSIONS)
    scoped_refptr<ExtensionSpecialStoragePolicy> extension_policy_;
#endif
    base::FilePath path_;
    Delegate* delegate_;
    bool guest_session_;
    base::Optional<bool> is_new_profile_;
    std::string supervised_user_id_;
    std::unique_ptr<policy::PolicyService> policy_service_;
    TestingFactories testing_factories_;
    std::string profile_name_;

    DISALLOW_COPY_AND_ASSIGN(Builder);
  };

  // Multi-profile aware constructor that takes the path to a directory managed
  // for this profile. This constructor is meant to be used by
  // TestingProfileManager::CreateTestingProfile. If you need to create multi-
  // profile profiles, use that factory method instead of this directly.
  // Exception: if you need to create multi-profile profiles for testing the
  // ProfileManager, then use the constructor below instead.
  explicit TestingProfile(const base::FilePath& path);

  // Multi-profile aware constructor that takes the path to a directory managed
  // for this profile and a delegate. This constructor is meant to be used
  // for unittesting the ProfileManager.
  TestingProfile(const base::FilePath& path, Delegate* delegate);

  // Full constructor allowing the setting of all possible instance data.
  // Callers should use Builder::Build() instead of invoking this constructor.
  TestingProfile(const base::FilePath& path,
                 Delegate* delegate,
#if BUILDFLAG(ENABLE_EXTENSIONS)
                 scoped_refptr<ExtensionSpecialStoragePolicy> extension_policy,
#endif
                 std::unique_ptr<sync_preferences::PrefServiceSyncable> prefs,
                 TestingProfile* parent,
                 bool guest_session,
                 base::Optional<bool> is_new_profile,
                 const std::string& supervised_user_id,
                 std::unique_ptr<policy::PolicyService> policy_service,
                 TestingFactories testing_factories,
                 const std::string& profile_name);

  ~TestingProfile() override;

  // Creates the favicon service. Consequent calls would recreate the service.
  void CreateFaviconService();

  // !!!!!!!! WARNING: THIS IS GENERALLY NOT SAFE TO CALL! !!!!!!!!
  // This bypasses the BrowserContextDependencyManager, and in particular, it
  // destroys any previously-created HistoryService. That means any other
  // KeyedServices that depend on HistoryService may be left with dangling
  // pointers.
  // Instead, use Builder::AddTestingFactory to inject your own factories.
  // !!!!!!!! WARNING: THIS IS GENERALLY NOT SAFE TO CALL! !!!!!!!!
  // Creates the history service. If |delete_file| is true, the history file is
  // deleted first, then the HistoryService is created. As TestingProfile
  // deletes the directory containing the files used by HistoryService, this
  // only matters if you're recreating the HistoryService.  If |no_db| is true,
  // the history backend will fail to initialize its database; this is useful
  // for testing error conditions. Returns true on success.
  bool CreateHistoryService(bool delete_file, bool no_db) WARN_UNUSED_RESULT;

  // Creates the BookmarkBarModel. If not invoked the bookmark bar model is
  // NULL. If |delete_file| is true, the bookmarks file is deleted first, then
  // the model is created. As TestingProfile deletes the directory containing
  // the files used by HistoryService, the boolean only matters if you're
  // recreating the BookmarkModel.
  //
  // NOTE: this does not block until the bookmarks are loaded. For that use
  // WaitForBookmarkModelToLoad().
  void CreateBookmarkModel(bool delete_file);

  // Creates a WebDataService. If not invoked, the web data service is NULL.
  void CreateWebDataService();

  // Blocks until the HistoryService finishes restoring its in-memory cache.
  // This is NOT invoked from CreateHistoryService.
  void BlockUntilHistoryIndexIsRefreshed();

  // Allow setting a profile as Guest after-the-fact to simplify some tests.
  void SetGuestSession(bool guest);

  // Allow setting the return value of IsNewProfile.
  void SetIsNewProfile(bool is_new_profile);

  sync_preferences::TestingPrefServiceSyncable* GetTestingPrefService();

  // Called on the parent of an incognito |profile|. Usually called from the
  // constructor of an incognito TestingProfile, but can also be used by tests
  // to provide an OffTheRecordProfileImpl instance.
  void SetOffTheRecordProfile(std::unique_ptr<Profile> profile);

  void SetSupervisedUserId(const std::string& id);

  // content::BrowserContext
  base::FilePath GetPath() const override;
  base::FilePath GetCachePath() const override;
#if !defined(OS_ANDROID)
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath& partition_path) override;
#endif  // !defined(OS_ANDROID)
  scoped_refptr<base::SequencedTaskRunner> GetIOTaskRunner() override;
  bool IsOffTheRecord() const override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;
  content::ResourceContext* GetResourceContext() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate()
      override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate()
      override;
  net::URLRequestContextGetter* CreateRequestContext(
      content::ProtocolHandlerMap* protocol_handlers,
      content::URLRequestInterceptorScopedVector request_interceptors) override;
  net::URLRequestContextGetter* CreateRequestContextForStoragePartition(
      const base::FilePath& partition_path,
      bool in_memory,
      content::ProtocolHandlerMap* protocol_handlers,
      content::URLRequestInterceptorScopedVector request_interceptors) override;
  net::URLRequestContextGetter* CreateMediaRequestContext() override;
  net::URLRequestContextGetter* CreateMediaRequestContextForStoragePartition(
      const base::FilePath& partition_path,
      bool in_memory) override;

  TestingProfile* AsTestingProfile() override;

  // Profile
  std::string GetProfileUserName() const override;
  ProfileType GetProfileType() const override;

  // DEPRECATED, because it's fragile to change a profile from non-incognito
  // to incognito after the ProfileKeyedServices have been created (some
  // ProfileKeyedServices either should not exist in incognito mode, or will
  // crash when they try to get references to other services they depend on,
  // but do not exist in incognito mode).
  // TODO(atwilson): Remove this API (http://crbug.com/277296).
  //
  // Changes a profile's to/from incognito mode temporarily - profile will be
  // returned to non-incognito before destruction to allow services to
  // properly shutdown. This is only supported for legacy tests - new tests
  // should create a true incognito profile using Builder::SetIncognito() or
  // by using the TestingProfile constructor that allows setting the incognito
  // flag.
  void ForceIncognito(bool force_incognito) {
    force_incognito_ = force_incognito;
  }

  Profile* GetOffTheRecordProfile() override;
  void DestroyOffTheRecordProfile() override {}
  bool HasOffTheRecordProfile() override;
  Profile* GetOriginalProfile() override;
  const Profile* GetOriginalProfile() const override;
  bool IsSupervised() const override;
  bool IsChild() const override;
  bool IsLegacySupervised() const override;
#if BUILDFLAG(ENABLE_EXTENSIONS)
  void SetExtensionSpecialStoragePolicy(
      ExtensionSpecialStoragePolicy* extension_special_storage_policy);
#endif
  ExtensionSpecialStoragePolicy* GetExtensionSpecialStoragePolicy() override;

  PrefService* GetPrefs() override;
  const PrefService* GetPrefs() const override;
#if !defined(OS_ANDROID)
  ChromeZoomLevelPrefs* GetZoomLevelPrefs() override;
#endif  // !defined(OS_ANDROID)
  net::URLRequestContextGetter* GetRequestContext() override;
  net::URLRequestContextGetter* GetRequestContextForExtensions() override;
  scoped_refptr<network::SharedURLLoaderFactory> GetURLLoaderFactory() override;

  void set_last_session_exited_cleanly(bool value) {
    last_session_exited_cleanly_ = value;
  }
  bool IsSameProfile(Profile* profile) override;
  base::Time GetStartTime() const override;
  base::FilePath last_selected_directory() override;
  void set_last_selected_directory(const base::FilePath& path) override;
  bool WasCreatedByVersionOrLater(const std::string& version) override;
  bool IsGuestSession() const override;
  bool IsNewProfile() override;
  void SetExitType(ExitType exit_type) override {}
  ExitType GetLastSessionExitType() override;
  network::mojom::NetworkContextPtr CreateNetworkContext(
      bool in_memory,
      const base::FilePath& relative_partition_path) override;

#if defined(OS_CHROMEOS)
  void ChangeAppLocale(const std::string&, AppLocaleChangedVia) override {}
  void OnLogin() override {}
  void InitChromeOSPreferences() override {}
  chromeos::ScopedCrosSettingsTestHelper* ScopedCrosSettingsTestHelper();
#endif  // defined(OS_CHROMEOS)

  // Schedules a task on the history backend and runs a nested loop until the
  // task is processed.  This has the effect of blocking the caller until the
  // history service processes all pending requests.
  void BlockUntilHistoryProcessesPendingRequests();

  GURL GetHomePage() override;

  PrefService* GetOffTheRecordPrefs() override;

  void set_profile_name(const std::string& profile_name) {
    profile_name_ = profile_name;
  }

 private:
  // We use a temporary directory to store testing profile data. This
  // must be declared before anything that may make use of the
  // directory so as to ensure files are closed before cleanup.  In a
  // multi-profile environment, this is invalid and the directory is
  // managed by the TestingProfileManager.
  base::ScopedTempDir temp_dir_;

 protected:
  base::Time start_time_;
  std::unique_ptr<sync_preferences::PrefServiceSyncable> prefs_;
  // ref only for right type, lifecycle is managed by prefs_
  sync_preferences::TestingPrefServiceSyncable* testing_prefs_;

 private:
  // Creates a temporary directory for use by this profile.
  void CreateTempProfileDir();

  // Common initialization between the two constructors.
  void Init();

  // Finishes initialization when a profile is created asynchronously.
  void FinishInit();

  // Creates a TestingPrefService and associates it with the TestingProfile.
  void CreateTestingPrefService();

  // Creates a pref service that uses SupervisedUserPrefStore and associates
  // it with the TestingProfile.
  void CreatePrefServiceForSupervisedUser();

  // Initializes |prefs_| for an incognito profile, derived from
  // |original_profile_|.
  void CreateIncognitoPrefService();

  // Creates a ProfilePolicyConnector that the ProfilePolicyConnectorFactory
  // maps to this profile.
  void CreateProfilePolicyConnector();

  // Internally, this is a TestURLRequestContextGetter that creates a dummy
  // request context. Currently, only the CookieMonster is hooked up.
  scoped_refptr<net::URLRequestContextGetter> extensions_request_context_;

  // Holds a dummy network context request to avoid triggering connection error
  // handler.
  network::mojom::NetworkContextRequest network_context_request_;

  bool force_incognito_;
  std::unique_ptr<Profile> incognito_profile_;
  TestingProfile* original_profile_;

  bool guest_session_;

  base::Optional<bool> is_new_profile_;

  std::string supervised_user_id_;

  // Did the last session exit cleanly? Default is true.
  bool last_session_exited_cleanly_;

  scoped_refptr<HostContentSettingsMap> host_content_settings_map_;

  base::FilePath last_selected_directory_;

#if BUILDFLAG(ENABLE_EXTENSIONS)
  scoped_refptr<ExtensionSpecialStoragePolicy>
      extension_special_storage_policy_;
#endif

  // The path to this profile. This will be valid in either of the two above
  // cases.
  base::FilePath profile_path_;

  base::FilePath extensions_path_;

  // We keep a weak pointer to the dependency manager we want to notify on our
  // death. Defaults to the Singleton implementation but overridable for
  // testing.
  BrowserContextDependencyManager* browser_context_dependency_manager_;

  // Owned, but must be deleted on the IO thread so not placing in a
  // std::unique_ptr<>.
  content::MockResourceContext* resource_context_;

  std::unique_ptr<policy::SchemaRegistryService> schema_registry_service_;
  std::unique_ptr<policy::ProfilePolicyConnector> profile_policy_connector_;

  // Weak pointer to a delegate for indicating that a profile was created.
  Delegate* delegate_;

  std::string profile_name_;

#if defined(OS_CHROMEOS)
  std::unique_ptr<chromeos::ScopedCrosSettingsTestHelper>
      scoped_cros_settings_test_helper_;
#endif  // defined(OS_CHROMEOS)

  std::unique_ptr<policy::PolicyService> policy_service_;
};

#endif  // CHROME_TEST_BASE_TESTING_PROFILE_H_
