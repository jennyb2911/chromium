// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/nacl/browser/nacl_browser.h"

#include <stddef.h>

#include "base/command_line.h"
#include "base/files/file_proxy.h"
#include "base/files/file_util.h"
#include "base/lazy_instance.h"
#include "base/location.h"
#include "base/metrics/histogram_macros.h"
#include "base/path_service.h"
#include "base/pickle.h"
#include "base/rand_util.h"
#include "base/single_thread_task_runner.h"
#include "base/task/post_task.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "url/gurl.h"

#if defined(OS_WIN)
#include "base/win/windows_version.h"
#endif

namespace {

// An arbitrary delay to coalesce multiple writes to the cache.
const int kValidationCacheCoalescingTimeMS = 6000;
const base::FilePath::CharType kValidationCacheFileName[] =
    FILE_PATH_LITERAL("nacl_validation_cache.bin");

const bool kValidationCacheEnabledByDefault = true;

// Keep the cache bounded to an arbitrary size.  If it's too small, useful
// entries could be evicted when multiple .nexes are loaded at once.  On the
// other hand, entries are not always claimed (and hence removed), so the size
// of the cache will likely saturate at its maximum size.
// Entries may not be claimed for two main reasons. 1) the NaCl process could
// be killed while it is loading.  2) the trusted NaCl plugin opens files using
// the code path but doesn't resolve them.
// TODO(ncbray) don't cache files that the plugin will not resolve.
const int kFilePathCacheSize = 100;

const base::FilePath::StringType NaClIrtName() {
  base::FilePath::StringType irt_name(FILE_PATH_LITERAL("nacl_irt_"));

#if defined(ARCH_CPU_X86_FAMILY)
#if defined(ARCH_CPU_X86_64)
  bool is64 = true;
#elif defined(OS_WIN)
  bool is64 = (base::win::OSInfo::GetInstance()->wow64_status() ==
               base::win::OSInfo::WOW64_ENABLED);
#else
  bool is64 = false;
#endif
  if (is64)
    irt_name.append(FILE_PATH_LITERAL("x86_64"));
  else
    irt_name.append(FILE_PATH_LITERAL("x86_32"));

#elif defined(ARCH_CPU_ARMEL)
  irt_name.append(FILE_PATH_LITERAL("arm"));
#elif defined(ARCH_CPU_MIPSEL)
  irt_name.append(FILE_PATH_LITERAL("mips32"));
#else
#error Add support for your architecture to NaCl IRT file selection
#endif
  irt_name.append(FILE_PATH_LITERAL(".nexe"));
  return irt_name;
}

#if !defined(OS_ANDROID)
bool CheckEnvVar(const char* name, bool default_value) {
  bool result = default_value;
  const char* var = getenv(name);
  if (var && strlen(var) > 0) {
    result = var[0] != '0';
  }
  return result;
}
#endif

void ReadCache(const base::FilePath& filename, std::string* data) {
  if (!base::ReadFileToString(filename, data)) {
    // Zero-size data used as an in-band error code.
    data->clear();
  }
}

void WriteCache(const base::FilePath& filename, const base::Pickle* pickle) {
  base::WriteFile(filename, static_cast<const char*>(pickle->data()),
                       pickle->size());
}

void RemoveCache(const base::FilePath& filename,
                 const base::Closure& callback) {
  base::DeleteFile(filename, false);
  base::PostTaskWithTraits(FROM_HERE, {content::BrowserThread::IO}, callback);
}

void LogCacheQuery(nacl::NaClBrowser::ValidationCacheStatus status) {
  UMA_HISTOGRAM_ENUMERATION("NaCl.ValidationCache.Query", status,
                            nacl::NaClBrowser::CACHE_MAX);
}

void LogCacheSet(nacl::NaClBrowser::ValidationCacheStatus status) {
  // Bucket zero is reserved for future use.
  UMA_HISTOGRAM_ENUMERATION("NaCl.ValidationCache.Set", status,
                            nacl::NaClBrowser::CACHE_MAX);
}

// Crash throttling parameters.
const size_t kMaxCrashesPerInterval = 3;
const int64_t kCrashesIntervalInSeconds = 120;

// Holds the NaClBrowserDelegate, which is leaked on shutdown.
NaClBrowserDelegate* g_browser_delegate = nullptr;

}  // namespace

namespace nacl {

base::File OpenNaClReadExecImpl(const base::FilePath& file_path,
                                bool is_executable) {
  // Get a file descriptor. On Windows, we need 'GENERIC_EXECUTE' in order to
  // memory map the executable.
  // IMPORTANT: This file descriptor must not have write access - that could
  // allow a NaCl inner sandbox escape.
  uint32_t flags = base::File::FLAG_OPEN | base::File::FLAG_READ;
  if (is_executable)
    flags |= base::File::FLAG_EXECUTE;  // Windows only flag.
  base::File file(file_path, flags);
  if (!file.IsValid())
    return file;

  // Check that the file does not reference a directory. Returning a descriptor
  // to an extension directory could allow an outer sandbox escape. openat(...)
  // could be used to traverse into the file system.
  base::File::Info file_info;
  if (!file.GetInfo(&file_info) || file_info.is_directory)
    return base::File();

  return file;
}

NaClBrowser::NaClBrowser()
    : irt_filepath_(),
      irt_state_(NaClResourceUninitialized),
      validation_cache_file_path_(),
      validation_cache_is_enabled_(false),
      validation_cache_is_modified_(false),
      validation_cache_state_(NaClResourceUninitialized),
      path_cache_(kFilePathCacheSize),
      has_failed_(false) {
#if !defined(OS_ANDROID)
      validation_cache_is_enabled_ =
          CheckEnvVar("NACL_VALIDATION_CACHE",
                      kValidationCacheEnabledByDefault);
#endif
      DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
}

void NaClBrowser::SetDelegate(std::unique_ptr<NaClBrowserDelegate> delegate) {
  // In the browser SetDelegate is called after threads are initialized.
  // In tests it is called before initializing BrowserThreads.
  if (content::BrowserThread::IsThreadInitialized(content::BrowserThread::UI)) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  }
  DCHECK(delegate);
  DCHECK(!g_browser_delegate);
  g_browser_delegate = delegate.release();
}

NaClBrowserDelegate* NaClBrowser::GetDelegate() {
  // NaClBrowser calls this on the IO thread, not the UI thread.
  DCHECK(g_browser_delegate);
  return g_browser_delegate;
}

void NaClBrowser::ClearAndDeleteDelegateForTest() {
  DCHECK(
      !content::BrowserThread::IsThreadInitialized(content::BrowserThread::UI));
  DCHECK(g_browser_delegate);
  delete g_browser_delegate;
  g_browser_delegate = nullptr;
}

void NaClBrowser::EarlyStartup() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  InitIrtFilePath();
  InitValidationCacheFilePath();
}

NaClBrowser::~NaClBrowser() {
  NOTREACHED();
}

void NaClBrowser::InitIrtFilePath() {
  // Allow the IRT library to be overridden via an environment
  // variable.  This allows the NaCl/Chromium integration bot to
  // specify a newly-built IRT rather than using a prebuilt one
  // downloaded via Chromium's DEPS file.  We use the same environment
  // variable that the standalone NaCl PPAPI plugin accepts.
  const char* irt_path_var = getenv("NACL_IRT_LIBRARY");
  if (irt_path_var != NULL) {
    base::FilePath::StringType path_string(
        irt_path_var, const_cast<const char*>(strchr(irt_path_var, '\0')));
    irt_filepath_ = base::FilePath(path_string);
  } else {
    base::FilePath plugin_dir;
    if (!GetDelegate()->GetPluginDirectory(&plugin_dir)) {
      DLOG(ERROR) << "Failed to locate the plugins directory, NaCl disabled.";
      MarkAsFailed();
      return;
    }
    irt_filepath_ = plugin_dir.Append(NaClIrtName());
  }
}

#if defined(OS_WIN)
bool NaClBrowser::GetNaCl64ExePath(base::FilePath* exe_path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  base::FilePath module_path;
  if (!base::PathService::Get(base::FILE_MODULE, &module_path)) {
    LOG(ERROR) << "NaCl process launch failed: could not resolve module";
    return false;
  }
  *exe_path = module_path.DirName().Append(L"nacl64");
  return true;
}
#endif

// static
NaClBrowser* NaClBrowser::GetInstanceInternal() {
  static NaClBrowser* g_instance = nullptr;
  if (!g_instance)
    g_instance = new NaClBrowser();
  return g_instance;
}

NaClBrowser* NaClBrowser::GetInstance() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  return GetInstanceInternal();
}

bool NaClBrowser::IsReady() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  return (IsOk() &&
          irt_state_ == NaClResourceReady &&
          validation_cache_state_ == NaClResourceReady);
}

bool NaClBrowser::IsOk() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  return !has_failed_;
}

const base::File& NaClBrowser::IrtFile() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  CHECK_EQ(irt_state_, NaClResourceReady);
  CHECK(irt_file_.IsValid());
  return irt_file_;
}

void NaClBrowser::EnsureAllResourcesAvailable() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  EnsureIrtAvailable();
  EnsureValidationCacheAvailable();
}

// Load the IRT async.
void NaClBrowser::EnsureIrtAvailable() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (IsOk() && irt_state_ == NaClResourceUninitialized) {
    irt_state_ = NaClResourceRequested;
    auto task_runner = base::CreateTaskRunnerWithTraits(
        {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
         base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN});
    std::unique_ptr<base::FileProxy> file_proxy(
        new base::FileProxy(task_runner.get()));
    base::FileProxy* proxy = file_proxy.get();
    if (!proxy->CreateOrOpen(
            irt_filepath_, base::File::FLAG_OPEN | base::File::FLAG_READ,
            base::BindOnce(&NaClBrowser::OnIrtOpened, base::Unretained(this),
                           std::move(file_proxy)))) {
      LOG(ERROR) << "Internal error, NaCl disabled.";
      MarkAsFailed();
    }
  }
}

void NaClBrowser::OnIrtOpened(std::unique_ptr<base::FileProxy> file_proxy,
                              base::File::Error error_code) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  DCHECK_EQ(irt_state_, NaClResourceRequested);
  if (file_proxy->IsValid()) {
    irt_file_ = file_proxy->TakeFile();
  } else {
    LOG(ERROR) << "Failed to open NaCl IRT file \""
               << irt_filepath_.LossyDisplayName()
               << "\": " << error_code;
    MarkAsFailed();
  }
  irt_state_ = NaClResourceReady;
  CheckWaiting();
}

void NaClBrowser::SetProcessGdbDebugStubPort(int process_id, int port) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  gdb_debug_stub_port_map_[process_id] = port;
  if (port != kGdbDebugStubPortUnknown &&
      !debug_stub_port_listener_.is_null()) {
    base::PostTaskWithTraits(FROM_HERE, {content::BrowserThread::IO},
                             base::BindOnce(debug_stub_port_listener_, port));
  }
}

// static
void NaClBrowser::SetGdbDebugStubPortListenerForTest(
    base::Callback<void(int)> listener) {
  GetInstanceInternal()->debug_stub_port_listener_ = listener;
}

// static
void NaClBrowser::ClearGdbDebugStubPortListenerForTest() {
  GetInstanceInternal()->debug_stub_port_listener_.Reset();
}

int NaClBrowser::GetProcessGdbDebugStubPort(int process_id) {
  // Called from TaskManager TaskGroup impl, on CrBrowserMain.
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  GdbDebugStubPortMap::iterator i = gdb_debug_stub_port_map_.find(process_id);
  if (i != gdb_debug_stub_port_map_.end()) {
    return i->second;
  }
  return kGdbDebugStubPortUnused;
}

void NaClBrowser::InitValidationCacheFilePath() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  // Determine where the validation cache resides in the file system.  It
  // exists in Chrome's cache directory and is not tied to any specific
  // profile.
  // Start by finding the user data directory.
  base::FilePath user_data_dir;
  if (!GetDelegate()->GetUserDirectory(&user_data_dir)) {
    RunWithoutValidationCache();
    return;
  }
  // The cache directory may or may not be the user data directory.
  base::FilePath cache_file_path;
  GetDelegate()->GetCacheDirectory(&cache_file_path);
  // Append the base file name to the cache directory.

  validation_cache_file_path_ =
      cache_file_path.Append(kValidationCacheFileName);
}

void NaClBrowser::EnsureValidationCacheAvailable() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (IsOk() && validation_cache_state_ == NaClResourceUninitialized) {
    if (ValidationCacheIsEnabled()) {
      validation_cache_state_ = NaClResourceRequested;

      // Structure for carrying data between the callbacks.
      std::string* data = new std::string();
      // We can get away not giving this a sequence ID because this is the first
      // task and further file access will not occur until after we get a
      // response.
      base::PostTaskWithTraitsAndReply(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
          base::BindOnce(ReadCache, validation_cache_file_path_, data),
          base::BindOnce(&NaClBrowser::OnValidationCacheLoaded,
                         base::Unretained(this), base::Owned(data)));
    } else {
      RunWithoutValidationCache();
    }
  }
}

void NaClBrowser::OnValidationCacheLoaded(const std::string *data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  // Did the cache get cleared before the load completed?  If so, ignore the
  // incoming data.
  if (validation_cache_state_ == NaClResourceReady)
    return;

  if (data->size() == 0) {
    // No file found.
    validation_cache_.Reset();
  } else {
    base::Pickle pickle(data->data(), data->size());
    validation_cache_.Deserialize(&pickle);
  }
  validation_cache_state_ = NaClResourceReady;
  CheckWaiting();
}

void NaClBrowser::RunWithoutValidationCache() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  // Be paranoid.
  validation_cache_.Reset();
  validation_cache_is_enabled_ = false;
  validation_cache_state_ = NaClResourceReady;
  CheckWaiting();
}

void NaClBrowser::CheckWaiting() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (!IsOk() || IsReady()) {
    // Queue the waiting tasks into the message loop.  This helps avoid
    // re-entrancy problems that could occur if the closure was invoked
    // directly.  For example, this could result in use-after-free of the
    // process host.
    for (std::vector<base::Closure>::iterator iter = waiting_.begin();
         iter != waiting_.end(); ++iter) {
      base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE, *iter);
    }
    waiting_.clear();
  }
}

void NaClBrowser::MarkAsFailed() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  has_failed_ = true;
  CheckWaiting();
}

void NaClBrowser::WaitForResources(const base::Closure& reply) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  waiting_.push_back(reply);
  EnsureAllResourcesAvailable();
  CheckWaiting();
}

const base::FilePath& NaClBrowser::GetIrtFilePath() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  return irt_filepath_;
}

void NaClBrowser::PutFilePath(const base::FilePath& path,
                              uint64_t* file_token_lo,
                              uint64_t* file_token_hi) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  while (true) {
    uint64_t file_token[2] = {base::RandUint64(), base::RandUint64()};
    // A zero file_token indicates there is no file_token, if we get zero, ask
    // for another number.
    if (file_token[0] != 0 || file_token[1] != 0) {
      // If the file_token is in use, ask for another number.
      std::string key(reinterpret_cast<char*>(file_token), sizeof(file_token));
      PathCacheType::iterator iter = path_cache_.Peek(key);
      if (iter == path_cache_.end()) {
        path_cache_.Put(key, path);
        *file_token_lo = file_token[0];
        *file_token_hi = file_token[1];
        break;
      }
    }
  }
}

bool NaClBrowser::GetFilePath(uint64_t file_token_lo,
                              uint64_t file_token_hi,
                              base::FilePath* path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  uint64_t file_token[2] = {file_token_lo, file_token_hi};
  std::string key(reinterpret_cast<char*>(file_token), sizeof(file_token));
  PathCacheType::iterator iter = path_cache_.Peek(key);
  if (iter == path_cache_.end()) {
    *path = base::FilePath(FILE_PATH_LITERAL(""));
    return false;
  }
  *path = iter->second;
  path_cache_.Erase(iter);
  return true;
}


bool NaClBrowser::QueryKnownToValidate(const std::string& signature,
                                       bool off_the_record) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (off_the_record) {
    // If we're off the record, don't reorder the main cache.
    return validation_cache_.QueryKnownToValidate(signature, false) ||
        off_the_record_validation_cache_.QueryKnownToValidate(signature, true);
  } else {
    bool result = validation_cache_.QueryKnownToValidate(signature, true);
    LogCacheQuery(result ? CACHE_HIT : CACHE_MISS);
    // Queries can modify the MRU order of the cache.
    MarkValidationCacheAsModified();
    return result;
  }
}

void NaClBrowser::SetKnownToValidate(const std::string& signature,
                                     bool off_the_record) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (off_the_record) {
    off_the_record_validation_cache_.SetKnownToValidate(signature);
  } else {
    validation_cache_.SetKnownToValidate(signature);
    // The number of sets should be equal to the number of cache misses, minus
    // validation failures and successful validations where stubout occurs.
    LogCacheSet(CACHE_HIT);
    MarkValidationCacheAsModified();
  }
}

void NaClBrowser::ClearValidationCache(const base::Closure& callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  // Note: this method may be called before EnsureValidationCacheAvailable has
  // been invoked.  In other words, this method may be called before any NaCl
  // processes have been created.  This method must succeed and invoke the
  // callback in such a case.  If it does not invoke the callback, Chrome's UI
  // will hang in that case.
  validation_cache_.Reset();
  off_the_record_validation_cache_.Reset();

  if (validation_cache_file_path_.empty()) {
    // Can't figure out what file to remove, but don't drop the callback.
    base::PostTaskWithTraits(FROM_HERE, {content::BrowserThread::IO}, callback);
  } else {
    // Delegate the removal of the cache from the filesystem to another thread
    // to avoid blocking the IO thread.
    // This task is dispatched immediately, not delayed and coalesced, because
    // the user interface for cache clearing is likely waiting for the callback.
    // In addition, we need to make sure the cache is actually cleared before
    // invoking the callback to meet the implicit guarantees of the UI.
    file_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(RemoveCache, validation_cache_file_path_, callback));
  }

  // Make sure any delayed tasks to persist the cache to the filesystem are
  // squelched.
  validation_cache_is_modified_ = false;

  // If the cache is cleared before it is loaded from the filesystem, act as if
  // we just loaded an empty cache.
  if (validation_cache_state_ != NaClResourceReady) {
    validation_cache_state_ = NaClResourceReady;
    CheckWaiting();
  }
}

void NaClBrowser::MarkValidationCacheAsModified() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (!validation_cache_is_modified_) {
    // Wait before persisting to disk.  This can coalesce multiple cache
    // modifications info a single disk write.
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NaClBrowser::PersistValidationCache,
                       base::Unretained(this)),
        base::TimeDelta::FromMilliseconds(kValidationCacheCoalescingTimeMS));
    validation_cache_is_modified_ = true;
  }
}

void NaClBrowser::PersistValidationCache() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  // validation_cache_is_modified_ may be false if the cache was cleared while
  // this delayed task was pending.
  // validation_cache_file_path_ may be empty if something went wrong during
  // initialization.
  if (validation_cache_is_modified_ && !validation_cache_file_path_.empty()) {
    base::Pickle* pickle = new base::Pickle();
    validation_cache_.Serialize(pickle);

    // Pass the serialized data to another thread to write to disk.  File IO is
    // not allowed on the IO thread (which is the thread this method runs on)
    // because it can degrade the responsiveness of the browser.
    // The task is sequenced so that multiple writes happen in order.
    file_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(WriteCache, validation_cache_file_path_,
                                  base::Owned(pickle)));
  }
  validation_cache_is_modified_ = false;
}

void NaClBrowser::OnProcessEnd(int process_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  gdb_debug_stub_port_map_.erase(process_id);
}

void NaClBrowser::OnProcessCrashed() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (crash_times_.size() == kMaxCrashesPerInterval) {
    crash_times_.pop_front();
  }
  base::Time time = base::Time::Now();
  crash_times_.push_back(time);
}

bool NaClBrowser::IsThrottled() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  if (crash_times_.size() != kMaxCrashesPerInterval) {
    return false;
  }
  base::TimeDelta delta = base::Time::Now() - crash_times_.front();
  return delta.InSeconds() <= kCrashesIntervalInSeconds;
}

}  // namespace nacl
