// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/components/drivefs/fake_drivefs.h"

#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/stl_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/task/post_task.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/fake_cros_disks_client.h"
#include "mojo/public/cpp/bindings/strong_binding.h"

namespace drivefs {
namespace {

class FakeDriveFsMojoConnectionDelegate
    : public drivefs::DriveFsHost::MojoConnectionDelegate {
 public:
  FakeDriveFsMojoConnectionDelegate(
      drivefs::mojom::DriveFsBootstrapPtrInfo bootstrap)
      : bootstrap_(std::move(bootstrap)) {}

  drivefs::mojom::DriveFsBootstrapPtrInfo InitializeMojoConnection() override {
    return std::move(bootstrap_);
  }

  void AcceptMojoConnection(base::ScopedFD handle) override { NOTREACHED(); }

 private:
  drivefs::mojom::DriveFsBootstrapPtrInfo bootstrap_;

  DISALLOW_COPY_AND_ASSIGN(FakeDriveFsMojoConnectionDelegate);
};

std::vector<std::pair<base::RepeatingCallback<std::string()>,
                      base::WeakPtr<FakeDriveFs>>>&
GetRegisteredFakeDriveFsIntances() {
  static base::NoDestructor<std::vector<std::pair<
      base::RepeatingCallback<std::string()>, base::WeakPtr<FakeDriveFs>>>>
      registered_fake_drivefs_instances;
  return *registered_fake_drivefs_instances;
}

base::FilePath MaybeMountDriveFs(
    const std::string& source_path,
    const std::vector<std::string>& mount_options) {
  GURL source_url(source_path);
  DCHECK(source_url.is_valid());
  if (source_url.scheme() != "drivefs") {
    return {};
  }
  std::string datadir_suffix;
  for (const auto& option : mount_options) {
    if (base::StartsWith(option, "datadir=", base::CompareCase::SENSITIVE)) {
      auto datadir =
          base::FilePath(base::StringPiece(option).substr(strlen("datadir=")));
      CHECK(datadir.IsAbsolute());
      CHECK(!datadir.ReferencesParent());
      datadir_suffix = datadir.BaseName().value();
      break;
    }
  }
  CHECK(!datadir_suffix.empty());
  for (auto& registration : GetRegisteredFakeDriveFsIntances()) {
    std::string account_id = registration.first.Run();
    if (registration.second && !account_id.empty() &&
        account_id == datadir_suffix) {
      return registration.second->mount_path();
    }
  }
  NOTREACHED() << datadir_suffix;
  return {};
}

}  // namespace

struct FakeDriveFs::FileMetadata {
  std::string mime_type;
  bool pinned = false;
  bool hosted = false;
  bool shared = false;
  std::string original_name;
};

class FakeDriveFs::SearchQuery : public mojom::SearchQuery {
 public:
  SearchQuery(base::WeakPtr<FakeDriveFs> drive_fs,
              const drivefs::mojom::QueryParameters& params)
      : drive_fs_(std::move(drive_fs)),
        query_(base::ToLowerASCII(
            params.title.value_or(params.text_content.value_or("")))),
        shared_with_me_(params.shared_with_me),
        available_offline_(params.available_offline),
        weak_ptr_factory_(this) {}

 private:
  void GetNextPage(GetNextPageCallback callback) override {
    if (!drive_fs_) {
      std::move(callback).Run(drive::FileError::FILE_ERROR_ABORT, {});
    } else {
      // Default implementation: just search for a file name.
      callback_ = std::move(callback);
      base::PostTaskWithTraitsAndReplyWithResult(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
          base::BindOnce(&SearchQuery::SearchFiles, drive_fs_->mount_path()),
          base::BindOnce(&SearchQuery::GetMetadata,
                         weak_ptr_factory_.GetWeakPtr()));
    }
  }

  static std::vector<drivefs::mojom::QueryItemPtr> SearchFiles(
      const base::FilePath& mount_path) {
    std::vector<drivefs::mojom::QueryItemPtr> results;
    base::FileEnumerator walker(
        mount_path, true,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
    for (auto file = walker.Next(); !file.empty(); file = walker.Next()) {
      auto item = drivefs::mojom::QueryItem::New();
      item->path = base::FilePath("/");
      CHECK(mount_path.AppendRelativePath(file, &item->path));
      results.push_back(std::move(item));
    }
    return results;
  }

  void GetMetadata(std::vector<drivefs::mojom::QueryItemPtr> results) {
    if (!drive_fs_) {
      std::move(callback_).Run(drive::FileError::FILE_ERROR_ABORT, {});
    } else {
      results_ = std::move(results);
      pending_callbacks_ = results_.size() + 1;
      for (size_t i = 0; i < results_.size(); ++i) {
        drive_fs_->GetMetadata(
            results_[i]->path,
            base::BindOnce(&SearchQuery::OnMetadata,
                           weak_ptr_factory_.GetWeakPtr(), i));
      }
      OnComplete();
    }
  }

  void OnMetadata(size_t index,
                  drive::FileError error,
                  drivefs::mojom::FileMetadataPtr metadata) {
    if (error == drive::FileError::FILE_ERROR_OK) {
      results_[index]->metadata = std::move(metadata);
    }
    OnComplete();
  }

  void OnComplete() {
    if (--pending_callbacks_ == 0) {
      // Filter out non-matching results.
      base::EraseIf(results_, [=](const auto& item_ptr) {
        if (!item_ptr->metadata) {
          return true;
        }
        const base::FilePath path = item_ptr->path;
        const drivefs::mojom::FileMetadata* metadata = item_ptr->metadata.get();
        if (!query_.empty()) {
          return base::ToLowerASCII(path.BaseName().value()).find(query_) ==
                 std::string::npos;
        }
        if (available_offline_) {
          return !metadata->available_offline &&
                 metadata->type != mojom::FileMetadata::Type::kHosted;
        }
        if (shared_with_me_) {
          return !metadata->shared;
        }
        return false;
      });

      std::move(callback_).Run(drive::FileError::FILE_ERROR_OK,
                               {std::move(results_)});
    }
  }

  base::WeakPtr<FakeDriveFs> drive_fs_;
  const std::string query_;
  const bool shared_with_me_;
  const bool available_offline_;
  GetNextPageCallback callback_;
  std::vector<drivefs::mojom::QueryItemPtr> results_;
  size_t pending_callbacks_ = 0;

  base::WeakPtrFactory<SearchQuery> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(SearchQuery);
};

FakeDriveFs::FakeDriveFs(const base::FilePath& mount_path)
    : mount_path_(mount_path),
      binding_(this),
      bootstrap_binding_(this),
      weak_factory_(this) {
  CHECK(mount_path.IsAbsolute());
  CHECK(!mount_path.ReferencesParent());
}

FakeDriveFs::~FakeDriveFs() = default;

void FakeDriveFs::RegisterMountingForAccountId(
    base::RepeatingCallback<std::string()> account_id_getter) {
  chromeos::DBusThreadManager* dbus_thread_manager =
      chromeos::DBusThreadManager::Get();
  static_cast<chromeos::FakeCrosDisksClient*>(
      dbus_thread_manager->GetCrosDisksClient())
      ->AddCustomMountPointCallback(base::BindRepeating(&MaybeMountDriveFs));

  GetRegisteredFakeDriveFsIntances().emplace_back(std::move(account_id_getter),
                                                  weak_factory_.GetWeakPtr());
}

std::unique_ptr<drivefs::DriveFsHost::MojoConnectionDelegate>
FakeDriveFs::CreateConnectionDelegate() {
  drivefs::mojom::DriveFsBootstrapPtrInfo bootstrap;
  bootstrap_binding_.Bind(mojo::MakeRequest(&bootstrap));
  pending_delegate_request_ = mojo::MakeRequest(&delegate_);
  delegate_->OnMounted();
  return std::make_unique<FakeDriveFsMojoConnectionDelegate>(
      std::move(bootstrap));
}

void FakeDriveFs::SetMetadata(const base::FilePath& path,
                              const std::string& mime_type,
                              const std::string& original_name,
                              bool pinned,
                              bool shared) {
  auto& stored_metadata = metadata_[path];
  stored_metadata.mime_type = mime_type;
  stored_metadata.original_name = original_name;
  stored_metadata.hosted = (original_name != path.BaseName().value());
  if (pinned) {
    stored_metadata.pinned = true;
  }
  if (shared) {
    stored_metadata.shared = true;
  }
}

void FakeDriveFs::Init(drivefs::mojom::DriveFsConfigurationPtr config,
                       drivefs::mojom::DriveFsRequest drive_fs_request,
                       drivefs::mojom::DriveFsDelegatePtr delegate) {
  mojo::FuseInterface(std::move(pending_delegate_request_),
                      delegate.PassInterface());
  binding_.Bind(std::move(drive_fs_request));
}

void FakeDriveFs::GetMetadata(const base::FilePath& path,
                              GetMetadataCallback callback) {
  base::FilePath absolute_path = mount_path_;
  CHECK(base::FilePath("/").AppendRelativePath(path, &absolute_path));
  base::File::Info info;
  if (!base::GetFileInfo(absolute_path, &info)) {
    std::move(callback).Run(drive::FILE_ERROR_NOT_FOUND, nullptr);
    return;
  }
  auto metadata = drivefs::mojom::FileMetadata::New();
  metadata->size = info.size;
  metadata->modification_time = info.last_modified;
  metadata->last_viewed_by_me_time = info.last_modified;

  const auto& stored_metadata = metadata_[path];
  metadata->pinned = stored_metadata.pinned;
  metadata->available_offline = stored_metadata.pinned;
  metadata->shared = stored_metadata.shared;

  metadata->content_mime_type = stored_metadata.mime_type;
  metadata->type = stored_metadata.hosted
                       ? mojom::FileMetadata::Type::kHosted
                       : info.is_directory
                             ? mojom::FileMetadata::Type::kDirectory
                             : mojom::FileMetadata::Type::kFile;

  base::StringPiece prefix;
  if (stored_metadata.hosted) {
    prefix = "https://document_alternate_link/";
  } else if (info.is_directory) {
    prefix = "https://folder_alternate_link/";
  } else {
    prefix = "https://file_alternate_link/";
  }
  std::string suffix = stored_metadata.original_name.empty()
                           ? path.BaseName().value()
                           : stored_metadata.original_name;
  metadata->alternate_url = GURL(base::StrCat({prefix, suffix})).spec();
  metadata->capabilities = mojom::Capabilities::New();

  std::move(callback).Run(drive::FILE_ERROR_OK, std::move(metadata));
}

void FakeDriveFs::SetPinned(const base::FilePath& path,
                            bool pinned,
                            SetPinnedCallback callback) {
  metadata_[path].pinned = pinned;
  std::move(callback).Run(drive::FILE_ERROR_OK);
}

void FakeDriveFs::UpdateNetworkState(bool pause_syncing, bool is_offline) {}

void FakeDriveFs::ResetCache(ResetCacheCallback callback) {
  std::move(callback).Run(drive::FILE_ERROR_OK);
}

void FakeDriveFs::GetThumbnail(const base::FilePath& path,
                               bool crop_to_square,
                               GetThumbnailCallback callback) {
  std::move(callback).Run(base::nullopt);
}

void FakeDriveFs::CopyFile(const base::FilePath& source,
                           const base::FilePath& target,
                           CopyFileCallback callback) {
  base::FilePath source_absolute_path = mount_path_;
  base::FilePath target_absolute_path = mount_path_;
  CHECK(base::FilePath("/").AppendRelativePath(source, &source_absolute_path));
  CHECK(base::FilePath("/").AppendRelativePath(target, &target_absolute_path));

  base::File::Info source_info;
  if (!base::GetFileInfo(source_absolute_path, &source_info)) {
    std::move(callback).Run(drive::FILE_ERROR_NOT_FOUND);
    return;
  }
  if (source_info.is_directory) {
    std::move(callback).Run(drive::FILE_ERROR_NOT_A_FILE);
    return;
  }

  base::File::Info target_directory_info;
  if (!base::GetFileInfo(target_absolute_path.DirName(),
                         &target_directory_info)) {
    std::move(callback).Run(drive::FILE_ERROR_NOT_FOUND);
    return;
  }
  if (!target_directory_info.is_directory) {
    std::move(callback).Run(drive::FILE_ERROR_INVALID_OPERATION);
    return;
  }

  if (base::PathExists(target_absolute_path)) {
    std::move(callback).Run(drive::FILE_ERROR_INVALID_OPERATION);
    return;
  }

  if (!base::CopyFile(source_absolute_path, target_absolute_path)) {
    std::move(callback).Run(drive::FILE_ERROR_FAILED);
    return;
  }
  metadata_[target_absolute_path] = metadata_[source_absolute_path];
  std::move(callback).Run(drive::FILE_ERROR_OK);
}

void FakeDriveFs::StartSearchQuery(
    drivefs::mojom::SearchQueryRequest query,
    drivefs::mojom::QueryParametersPtr query_params) {
  auto search_query =
      std::make_unique<SearchQuery>(weak_factory_.GetWeakPtr(), *query_params);
  mojo::MakeStrongBinding(std::move(search_query), std::move(query));
}

}  // namespace drivefs
