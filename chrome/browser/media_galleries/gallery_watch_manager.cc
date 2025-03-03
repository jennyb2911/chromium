// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media_galleries/gallery_watch_manager.h"

#include <stddef.h>

#include <tuple>

#include "base/bind.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/sequence_checker.h"
#include "base/stl_util.h"
#include "base/task/post_task.h"
#include "base/task/task_traits.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/media_galleries/gallery_watch_manager_observer.h"
#include "chrome/browser/media_galleries/media_file_system_registry.h"
#include "chrome/browser/media_galleries/media_galleries_preferences_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/keyed_service/content/browser_context_keyed_service_shutdown_notifier_factory.h"
#include "components/storage_monitor/storage_monitor.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/common/extension.h"

using content::BrowserContext;
using content::BrowserThread;

namespace {

// Don't send a notification more than once per 3 seconds (chosen arbitrarily).
const int kMinNotificationDelayInSeconds = 3;

class GalleryWatchManagerShutdownNotifierFactory
    : public BrowserContextKeyedServiceShutdownNotifierFactory {
 public:
  static GalleryWatchManagerShutdownNotifierFactory* GetInstance() {
    return base::Singleton<GalleryWatchManagerShutdownNotifierFactory>::get();
  }

 private:
  friend struct base::DefaultSingletonTraits<
      GalleryWatchManagerShutdownNotifierFactory>;

  GalleryWatchManagerShutdownNotifierFactory()
      : BrowserContextKeyedServiceShutdownNotifierFactory(
            "GalleryWatchManager") {
    DependsOn(MediaGalleriesPreferencesFactory::GetInstance());
  }
  ~GalleryWatchManagerShutdownNotifierFactory() override {}

  DISALLOW_COPY_AND_ASSIGN(GalleryWatchManagerShutdownNotifierFactory);
};

}  // namespace.

const char GalleryWatchManager::kInvalidGalleryIDError[] = "Invalid gallery ID";
const char GalleryWatchManager::kNoPermissionError[] =
    "No permission for gallery ID.";
const char GalleryWatchManager::kCouldNotWatchGalleryError[] =
    "Could not watch gallery path.";

// Manages a collection of file path watchers on a sequenced task runner and
// relays the change events to |callback| on the UI thread. This file is
// constructed on the UI thread, but operates and is destroyed on a sequenced
// task runner. If |callback| is called with an error, all watches on that path
// have been dropped.
class GalleryWatchManager::FileWatchManager {
 public:
  explicit FileWatchManager(const base::FilePathWatcher::Callback& callback);
  ~FileWatchManager();

  // Posts success or failure via |callback| to the UI thread.
  void AddFileWatch(const base::FilePath& path,
                    const base::Callback<void(bool)>& callback);

  void RemoveFileWatch(const base::FilePath& path);

  base::WeakPtr<FileWatchManager> GetWeakPtr();

 private:
  using WatcherMap =
      std::map<base::FilePath, std::unique_ptr<base::FilePathWatcher>>;

  void OnFilePathChanged(const base::FilePath& path, bool error);

  WatcherMap watchers_;

  base::FilePathWatcher::Callback callback_;

  SEQUENCE_CHECKER(sequence_checker_);

  base::WeakPtrFactory<FileWatchManager> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(FileWatchManager);
};

GalleryWatchManager::FileWatchManager::FileWatchManager(
    const base::FilePathWatcher::Callback& callback)
    : callback_(callback), weak_factory_(this) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Bind to the sequenced task runner, not the UI thread.
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

GalleryWatchManager::FileWatchManager::~FileWatchManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void GalleryWatchManager::FileWatchManager::AddFileWatch(
    const base::FilePath& path,
    const base::Callback<void(bool)>& callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // This can occur if the GalleryWatchManager attempts to watch the same path
  // again before recieving the callback. It's benign.
  if (base::ContainsKey(watchers_, path)) {
    base::PostTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                             base::BindOnce(callback, false));
    return;
  }

  auto watcher = std::make_unique<base::FilePathWatcher>();
  bool success = watcher->Watch(path,
                                true /*recursive*/,
                                base::Bind(&FileWatchManager::OnFilePathChanged,
                                           weak_factory_.GetWeakPtr()));

  if (success)
    watchers_[path] = std::move(watcher);

  base::PostTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                           base::BindOnce(callback, success));
}

void GalleryWatchManager::FileWatchManager::RemoveFileWatch(
    const base::FilePath& path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  size_t erased = watchers_.erase(path);
  DCHECK_EQ(erased, 1u);
}

base::WeakPtr<GalleryWatchManager::FileWatchManager>
GalleryWatchManager::FileWatchManager::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void GalleryWatchManager::FileWatchManager::OnFilePathChanged(
    const base::FilePath& path,
    bool error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (error)
    RemoveFileWatch(path);
  base::PostTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                           base::BindOnce(callback_, path, error));
}

GalleryWatchManager::WatchOwner::WatchOwner(BrowserContext* browser_context,
                                            const std::string& extension_id,
                                            MediaGalleryPrefId gallery_id)
    : browser_context(browser_context),
      extension_id(extension_id),
      gallery_id(gallery_id) {
}

bool GalleryWatchManager::WatchOwner::operator<(const WatchOwner& other) const {
  return std::tie(browser_context, extension_id, gallery_id) <
    std::tie(other.browser_context, other.extension_id, other.gallery_id);
}

GalleryWatchManager::NotificationInfo::NotificationInfo()
    : delayed_notification_pending(false) {
}

GalleryWatchManager::NotificationInfo::NotificationInfo(
    const NotificationInfo& other) = default;

GalleryWatchManager::NotificationInfo::~NotificationInfo() {
}

GalleryWatchManager::GalleryWatchManager()
    : storage_monitor_observed_(false),
      watch_manager_task_runner_(base::CreateSequencedTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT})),
      weak_factory_(this) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  watch_manager_.reset(new FileWatchManager(base::Bind(
      &GalleryWatchManager::OnFilePathChanged, weak_factory_.GetWeakPtr())));
}

GalleryWatchManager::~GalleryWatchManager() {
  weak_factory_.InvalidateWeakPtrs();

  if (storage_monitor_observed_) {
    DCHECK(storage_monitor::StorageMonitor::GetInstance());
    storage_monitor::StorageMonitor::GetInstance()->RemoveObserver(this);
  }

  watch_manager_task_runner_->DeleteSoon(FROM_HERE, watch_manager_.release());
}

void GalleryWatchManager::AddObserver(BrowserContext* browser_context,
                                      GalleryWatchManagerObserver* observer) {
  DCHECK(browser_context);
  DCHECK(observer);
  DCHECK(!base::ContainsKey(observers_, browser_context));
  observers_[browser_context] = observer;
}

void GalleryWatchManager::RemoveObserver(BrowserContext* browser_context) {
  DCHECK(browser_context);
  size_t erased = observers_.erase(browser_context);
  DCHECK_EQ(erased, 1u);
}

void GalleryWatchManager::ShutdownBrowserContext(
    BrowserContext* browser_context) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(browser_context);

  MediaGalleriesPreferences* preferences =
      g_browser_process->media_file_system_registry()->GetPreferences(
          Profile::FromBrowserContext(browser_context));
  size_t observed = observed_preferences_.erase(preferences);
  if (observed > 0)
    preferences->RemoveGalleryChangeObserver(this);

  WatchesMap::iterator it = watches_.begin();
  while (it != watches_.end()) {
    if (it->first.browser_context == browser_context) {
      DeactivateFileWatch(it->first, it->second);
      // Post increment moves iterator to next element while deleting current.
      watches_.erase(it++);
    } else {
      ++it;
    }
  }

  browser_context_subscription_map_.erase(browser_context);
}

void GalleryWatchManager::AddWatch(BrowserContext* browser_context,
                                   const extensions::Extension* extension,
                                   MediaGalleryPrefId gallery_id,
                                   const ResultCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(browser_context);
  DCHECK(extension);

  WatchOwner owner(browser_context, extension->id(), gallery_id);
  if (base::ContainsKey(watches_, owner)) {
    callback.Run(std::string());
    return;
  }

  MediaGalleriesPreferences* preferences =
      g_browser_process->media_file_system_registry()->GetPreferences(
          Profile::FromBrowserContext(browser_context));

  if (!base::ContainsKey(preferences->known_galleries(), gallery_id)) {
    callback.Run(kInvalidGalleryIDError);
    return;
  }

  MediaGalleryPrefIdSet permitted =
      preferences->GalleriesForExtension(*extension);
  if (!base::ContainsKey(permitted, gallery_id)) {
    callback.Run(kNoPermissionError);
    return;
  }

  base::FilePath path =
      preferences->known_galleries().find(gallery_id)->second.AbsolutePath();

  if (!storage_monitor_observed_) {
    storage_monitor_observed_ = true;
    storage_monitor::StorageMonitor::GetInstance()->AddObserver(this);
  }

  // Observe the preferences if we haven't already.
  if (!base::ContainsKey(observed_preferences_, preferences)) {
    observed_preferences_.insert(preferences);
    preferences->AddGalleryChangeObserver(this);
  }

  watches_[owner] = path;
  EnsureBrowserContextSubscription(owner.browser_context);

  // Start the FilePathWatcher on |gallery_path| if necessary.
  if (base::ContainsKey(watched_paths_, path)) {
    OnFileWatchActivated(owner, path, callback, true);
  } else {
    base::Callback<void(bool)> on_watch_added =
        base::Bind(&GalleryWatchManager::OnFileWatchActivated,
                   weak_factory_.GetWeakPtr(),
                   owner,
                   path,
                   callback);
    watch_manager_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&FileWatchManager::AddFileWatch,
                       watch_manager_->GetWeakPtr(), path, on_watch_added));
  }
}

void GalleryWatchManager::RemoveWatch(BrowserContext* browser_context,
                                      const std::string& extension_id,
                                      MediaGalleryPrefId gallery_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(browser_context);

  WatchOwner owner(browser_context, extension_id, gallery_id);
  WatchesMap::iterator it = watches_.find(owner);
  if (it != watches_.end()) {
    DeactivateFileWatch(owner, it->second);
    watches_.erase(it);
  }
}

void GalleryWatchManager::RemoveAllWatches(BrowserContext* browser_context,
                                           const std::string& extension_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(browser_context);

  WatchesMap::iterator it = watches_.begin();
  while (it != watches_.end()) {
    if (it->first.extension_id == extension_id) {
      DeactivateFileWatch(it->first, it->second);
      // Post increment moves iterator to next element while deleting current.
      watches_.erase(it++);
    } else {
      ++it;
    }
  }
}

MediaGalleryPrefIdSet GalleryWatchManager::GetWatchSet(
    BrowserContext* browser_context,
    const std::string& extension_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(browser_context);

  MediaGalleryPrefIdSet result;
  for (WatchesMap::const_iterator it = watches_.begin(); it != watches_.end();
       ++it) {
    if (it->first.browser_context == browser_context &&
        it->first.extension_id == extension_id) {
      result.insert(it->first.gallery_id);
    }
  }
  return result;
}

void GalleryWatchManager::EnsureBrowserContextSubscription(
    BrowserContext* browser_context) {
  auto it = browser_context_subscription_map_.find(browser_context);
  if (it == browser_context_subscription_map_.end()) {
    browser_context_subscription_map_[browser_context] =
        GalleryWatchManagerShutdownNotifierFactory::GetInstance()
            ->Get(browser_context)
            ->Subscribe(base::Bind(&GalleryWatchManager::ShutdownBrowserContext,
                                   base::Unretained(this), browser_context));
  }
}

void GalleryWatchManager::DeactivateFileWatch(const WatchOwner& owner,
                                              const base::FilePath& path) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  WatchedPaths::iterator it = watched_paths_.find(path);
  if (it == watched_paths_.end())
    return;

  it->second.owners.erase(owner);
  if (it->second.owners.empty()) {
    watched_paths_.erase(it);
    watch_manager_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&FileWatchManager::RemoveFileWatch,
                                  watch_manager_->GetWeakPtr(), path));
  }
}

void GalleryWatchManager::OnFileWatchActivated(const WatchOwner& owner,
                                               const base::FilePath& path,
                                               const ResultCallback& callback,
                                               bool success) {
  if (success) {
    // |watched_paths_| doesn't necessarily to contain |path| yet.
    // In that case, it calls the default constructor for NotificationInfo.
    watched_paths_[path].owners.insert(owner);
    callback.Run(std::string());
  } else {
    callback.Run(kCouldNotWatchGalleryError);
  }
}

void GalleryWatchManager::OnFilePathChanged(const base::FilePath& path,
                                            bool error) {
  WatchedPaths::iterator notification_info = watched_paths_.find(path);
  if (notification_info == watched_paths_.end())
    return;

  // On error, all watches on that path are dropped, so update records and
  // notify observers.
  if (error) {
    // Make a copy, as |watched_paths_| is modified as we erase watches.
    std::set<WatchOwner> owners = notification_info->second.owners;
    for (std::set<WatchOwner>::iterator it = owners.begin(); it != owners.end();
         ++it) {
      Profile* profile = Profile::FromBrowserContext(it->browser_context);
      RemoveWatch(it->browser_context, it->extension_id, it->gallery_id);
      if (base::ContainsKey(observers_, profile))
        observers_[profile]->OnGalleryWatchDropped(it->extension_id,
                                                   it->gallery_id);
    }

    return;
  }

  base::TimeDelta time_since_last_notify =
      base::Time::Now() - notification_info->second.last_notify_time;
  if (time_since_last_notify <
      base::TimeDelta::FromSeconds(kMinNotificationDelayInSeconds)) {
    if (!notification_info->second.delayed_notification_pending) {
      notification_info->second.delayed_notification_pending = true;
      base::TimeDelta delay_to_next_valid_time =
          notification_info->second.last_notify_time +
          base::TimeDelta::FromSeconds(kMinNotificationDelayInSeconds) -
          base::Time::Now();
      base::PostDelayedTaskWithTraits(
          FROM_HERE, {BrowserThread::UI},
          base::BindOnce(&GalleryWatchManager::OnFilePathChanged,
                         weak_factory_.GetWeakPtr(), path, error),
          delay_to_next_valid_time);
    }
    return;
  }
  notification_info->second.delayed_notification_pending = false;
  notification_info->second.last_notify_time = base::Time::Now();

  std::set<WatchOwner>::const_iterator it;
  for (it = notification_info->second.owners.begin();
       it != notification_info->second.owners.end();
       ++it) {
    DCHECK(base::ContainsKey(watches_, *it));
    if (base::ContainsKey(observers_, it->browser_context)) {
      observers_[it->browser_context]->OnGalleryChanged(it->extension_id,
                                                        it->gallery_id);
    }
  }
}

void GalleryWatchManager::OnPermissionRemoved(MediaGalleriesPreferences* pref,
                                              const std::string& extension_id,
                                              MediaGalleryPrefId pref_id) {
  RemoveWatch(pref->profile(), extension_id, pref_id);
  if (base::ContainsKey(observers_, pref->profile()))
    observers_[pref->profile()]->OnGalleryWatchDropped(extension_id, pref_id);
}

void GalleryWatchManager::OnGalleryRemoved(MediaGalleriesPreferences* pref,
                                           MediaGalleryPrefId pref_id) {
  // Removing a watch may update |watches_|, so extract the extension ids first.
  std::set<std::string> extension_ids;
  for (WatchesMap::const_iterator it = watches_.begin(); it != watches_.end();
       ++it) {
    if (it->first.browser_context == pref->profile() &&
        it->first.gallery_id == pref_id) {
      extension_ids.insert(it->first.extension_id);
    }
  }

  for (std::set<std::string>::const_iterator it = extension_ids.begin();
       it != extension_ids.end();
       ++it) {
    RemoveWatch(pref->profile(), *it, pref_id);
    if (base::ContainsKey(observers_, pref->profile()))
      observers_[pref->profile()]->OnGalleryWatchDropped(*it, pref_id);
  }
}

void GalleryWatchManager::OnRemovableStorageDetached(
    const storage_monitor::StorageInfo& info) {
  WatchesMap::iterator it = watches_.begin();
  while (it != watches_.end()) {
    MediaGalleriesPreferences* preferences =
        g_browser_process->media_file_system_registry()->GetPreferences(
            Profile::FromBrowserContext(it->first.browser_context));
    MediaGalleryPrefIdSet detached_ids =
        preferences->LookUpGalleriesByDeviceId(info.device_id());

    if (base::ContainsKey(detached_ids, it->first.gallery_id)) {
      WatchOwner owner = it->first;
      DeactivateFileWatch(owner, it->second);
      // Post increment moves iterator to next element while deleting current.
      watches_.erase(it++);
      observers_[preferences->profile()]->OnGalleryWatchDropped(
          owner.extension_id, owner.gallery_id);
    } else {
      ++it;
    }
  }
}
