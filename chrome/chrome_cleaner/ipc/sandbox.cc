// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/chrome_cleaner/ipc/sandbox.h"

#include <windows.h>

#include <iterator>
#include <utility>

#include "base/base_switches.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "base/win/win_util.h"
#include "chrome/chrome_cleaner/constants/chrome_cleaner_switches.h"
#include "chrome/chrome_cleaner/crash/crash_reporter.h"
#include "chrome/chrome_cleaner/os/disk_util.h"
#include "chrome/chrome_cleaner/os/inheritable_event.h"
#include "chrome/chrome_cleaner/os/initializer.h"
#include "chrome/chrome_cleaner/os/pre_fetched_paths.h"
#include "chrome/chrome_cleaner/settings/settings.h"
#include "components/chrome_cleaner/public/constants/constants.h"
#include "sandbox/win/src/sandbox_factory.h"
#include "sandbox/win/src/sandbox_types.h"
#include "sandbox/win/src/security_level.h"
#include "sandbox/win/src/target_services.h"

namespace chrome_cleaner {

const wchar_t kSandboxLogFileSuffix[] = L"-sandbox";

namespace {

// Switches to propagate to the sandbox target process.
const char* kSwitchesToPropagate[] = {
    kEnableCrashReportingSwitch, kExecutionModeSwitch,
    kExtendedSafeBrowsingEnabledSwitch, switches::kTestChildProcess,
};

std::map<SandboxType, base::Process>* g_target_processes = nullptr;  // Leaked.

scoped_refptr<sandbox::TargetPolicy> GetSandboxPolicy(
    sandbox::BrokerServices* sandbox_broker_services) {
  scoped_refptr<sandbox::TargetPolicy> policy(
      sandbox_broker_services->CreatePolicy());

  sandbox::ResultCode sandbox_result = policy->SetTokenLevel(
      sandbox::USER_RESTRICTED_SAME_ACCESS, sandbox::USER_LOCKDOWN);
  CHECK_EQ(sandbox::SBOX_ALL_OK, sandbox_result);

  sandbox_result = policy->SetJobLevel(sandbox::JOB_LOCKDOWN, 0);
  CHECK_EQ(sandbox::SBOX_ALL_OK, sandbox_result);

  // Chromium ignores failures on this function but logs a warning. Do the same
  // here.
  // https://chromium.googlesource.com/chromium/src/+/b6a4ff86c730756a73d63cc882ef818fb7818a53/content/common/sandbox_win.cc#420
  sandbox::ResultCode result = policy->SetAlternateDesktop(true);
  LOG_IF(WARNING, result != sandbox::SBOX_ALL_OK)
      << "Failed to apply desktop security";

  sandbox_result =
      policy->SetDelayedIntegrityLevel(sandbox::INTEGRITY_LEVEL_UNTRUSTED);
  CHECK_EQ(sandbox::SBOX_ALL_OK, sandbox_result);

  // This is all the mitigations from security_level.h, except those that need
  // to be enabled later (set in SetDelayedProcessMitigations below).
  sandbox_result = policy->SetProcessMitigations(
      sandbox::MITIGATION_DEP | sandbox::MITIGATION_DEP_NO_ATL_THUNK |
      sandbox::MITIGATION_SEHOP | sandbox::MITIGATION_HEAP_TERMINATE |
      sandbox::MITIGATION_BOTTOM_UP_ASLR |
      sandbox::MITIGATION_HIGH_ENTROPY_ASLR |
      sandbox::MITIGATION_WIN32K_DISABLE |
      sandbox::MITIGATION_EXTENSION_POINT_DISABLE |
      sandbox::MITIGATION_NONSYSTEM_FONT_DISABLE |
      sandbox::MITIGATION_HARDEN_TOKEN_IL_POLICY |
      sandbox::MITIGATION_IMAGE_LOAD_NO_REMOTE |
      sandbox::MITIGATION_IMAGE_LOAD_NO_LOW_LABEL);
  CHECK_EQ(sandbox::SBOX_ALL_OK, sandbox_result);

  // RELOCATE_IMAGE and RELOCATE_IMAGE_REQUIRED could be in
  // SetProcessMitigations above, but they need to be delayed in Debug builds.
  // It's easier to just set them up as delayed for both Debug and Release
  // builds.
  sandbox_result = policy->SetDelayedProcessMitigations(
      sandbox::MITIGATION_RELOCATE_IMAGE |
      sandbox::MITIGATION_RELOCATE_IMAGE_REQUIRED |
      sandbox::MITIGATION_STRICT_HANDLE_CHECKS |
      sandbox::MITIGATION_DLL_SEARCH_ORDER);
  CHECK_EQ(sandbox::SBOX_ALL_OK, sandbox_result);

  // This rule is needed to allow user32.dll and gdi32.dll to initialize during
  // load, while still blocking other WIN32K calls.
  sandbox_result =
      policy->AddRule(sandbox::TargetPolicy::SUBSYS_WIN32K_LOCKDOWN,
                      sandbox::TargetPolicy::FAKE_USER_GDI_INIT, nullptr);
  CHECK_EQ(sandbox::SBOX_ALL_OK, sandbox_result);

#if !defined(CHROME_CLEANER_OFFICIAL_BUILD)
  base::FilePath product_path;
  GetAppDataProductDirectory(&product_path);
  if (!product_path.value().empty()) {
    // In developer builds, let the sandbox target process write logs to the
    // product directory.
    sandbox_result = policy->AddRule(sandbox::TargetPolicy::SUBSYS_FILES,
                                     sandbox::TargetPolicy::FILES_ALLOW_ANY,
                                     product_path.Append(L"*").value().c_str());
    LOG_IF(ERROR, sandbox_result != sandbox::SBOX_ALL_OK)
        << "Failed to give the target process access to the product directory";
  }

  // Also let it write to stdout and stderr. (If they point to the Windows
  // console, these calls will silently fail. But this will let output from the
  // child be captured on the bots or in the msys terminal.)
  policy->SetStdoutHandle(::GetStdHandle(STD_OUTPUT_HANDLE));
  policy->SetStderrHandle(::GetStdHandle(STD_ERROR_HANDLE));
#endif  // CHROME_CLEANER_OFFICIAL_BUILD

  policy->SetLockdownDefaultDacl();

  // Do not include SetDisconnectCsrss because the signature validator uses
  // wincrypt, which uses a garbage-collected connection to csrss.exe that may
  // not be cleaned up yet when we call LowerToken.

  return policy;
}

}  // namespace

SandboxSetupHooks::SandboxSetupHooks() = default;

SandboxSetupHooks::~SandboxSetupHooks() = default;

ResultCode SandboxSetupHooks::UpdateSandboxPolicy(
    sandbox::TargetPolicy* policy,
    base::CommandLine* command_line) {
  return RESULT_CODE_SUCCESS;
}

ResultCode SandboxSetupHooks::TargetSpawned(
    const base::Process& target_process,
    const base::win::ScopedHandle& target_thread) {
  return RESULT_CODE_SUCCESS;
}

ResultCode SandboxSetupHooks::TargetResumed() {
  return RESULT_CODE_SUCCESS;
}

void SandboxSetupHooks::SetupFailed() {}

SandboxTargetHooks::SandboxTargetHooks() = default;

SandboxTargetHooks::~SandboxTargetHooks() = default;

ResultCode SandboxTargetHooks::TargetStartedWithHighPrivileges() {
  return RESULT_CODE_SUCCESS;
}

SandboxType SandboxProcessType() {
  // This should only be called by children processes.
  DCHECK(sandbox::SandboxFactory::GetTargetServices() != nullptr);

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  int val = -1;
  const bool success = base::StringToInt(
      command_line->GetSwitchValueASCII(kSandboxedProcessIdSwitch), &val);

  SandboxType sandbox_type = SandboxType::kNonSandboxed;

  if (success) {
    sandbox_type = static_cast<SandboxType>(val);
    DCHECK_GT(sandbox_type, SandboxType::kNonSandboxed);
    DCHECK_LT(sandbox_type, SandboxType::kNumValues);
  }

  return sandbox_type;
}

ResultCode SpawnSandbox(SandboxSetupHooks* setup_hooks, SandboxType type) {
  base::CommandLine sandbox_command_line(
      PreFetchedPaths::GetInstance()->GetExecutablePath());

  // Propagate the relevant switches from the current process to
  // |sandbox_command_line|.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  for (const char* switch_name : kSwitchesToPropagate) {
    if (command_line->HasSwitch(switch_name)) {
      sandbox_command_line.AppendSwitchNative(
          switch_name, command_line->GetSwitchValueNative(switch_name));
    }
  }
  sandbox_command_line.AppendSwitchNative(kUseCrashHandlerWithIdSwitch,
                                          GetCrashReporterIPCPipeName());

  sandbox_command_line.AppendSwitchASCII(
      kSandboxedProcessIdSwitch, base::IntToString(static_cast<int>(type)));

  return StartSandboxTarget(sandbox_command_line, setup_hooks, type);
}

ResultCode StartSandboxTarget(const base::CommandLine& sandbox_command_line,
                              SandboxSetupHooks* hooks,
                              SandboxType type) {
  if (!g_target_processes)
    g_target_processes = new std::map<SandboxType, base::Process>();
  // |StartSandboxTarget| gets called multiple times in tests.
  if (g_target_processes->erase(type))
    DCHECK_EQ(SandboxType::kTest, type);

  base::ScopedClosureRunner notify_hooks_on_failure(base::DoNothing::Once());

  if (hooks) {
    // Unretained is safe because |hooks| lives for the entire enclosing scope.
    notify_hooks_on_failure.ReplaceClosure(base::BindOnce(
        &SandboxSetupHooks::SetupFailed, base::Unretained(hooks)));
  }

  sandbox::BrokerServices* sandbox_broker_services =
      sandbox::SandboxFactory::GetBrokerServices();
  CHECK(sandbox_broker_services);

  // Make init_result static so broker services will only be initialized once.
  // Otherwise, it could be initialized multiple times during tests.
  static const sandbox::ResultCode init_result =
      sandbox_broker_services->Init();
  if (init_result != sandbox::SBOX_ALL_OK) {
    LOG(FATAL) << "Failed to initialize sandbox BrokerServices: "
               << init_result;
    return RESULT_CODE_FAILED_TO_START_SANDBOX_PROCESS;
  }

  scoped_refptr<sandbox::TargetPolicy> policy =
      GetSandboxPolicy(sandbox_broker_services);
  base::CommandLine command_line = sandbox_command_line;

  // Create an event so the sandboxed process can notify the broker when it
  // has finished it's setup. This is useful because if the sandboxed process
  // fails its setup, we can return the exit code to help debug the issue.
  std::unique_ptr<base::WaitableEvent> init_done_event =
      chrome_cleaner::CreateInheritableEvent(
          base::WaitableEvent::ResetPolicy::MANUAL,
          base::WaitableEvent::InitialState::NOT_SIGNALED);
  command_line.AppendSwitchNative(
      chrome_cleaner::kInitDoneNotifierSwitch,
      base::UintToString16(
          base::win::HandleToUint32(init_done_event->handle())));
  policy->AddHandleToShare(init_done_event->handle());

  if (hooks) {
    ResultCode result_code =
        hooks->UpdateSandboxPolicy(policy.get(), &command_line);
    if (result_code != RESULT_CODE_SUCCESS)
      return result_code;
  }

  // Spawn the sandbox target process.
  PROCESS_INFORMATION temp_process_info = {0};
  DWORD last_win_error = 0;
  sandbox::ResultCode last_result_code = sandbox::SBOX_ALL_OK;
  LOG(INFO) << "Starting sandbox process with command line arguments: "
            << command_line.GetArgumentsString();
  sandbox::ResultCode sandbox_result = sandbox_broker_services->SpawnTarget(
      command_line.GetProgram().value().c_str(),
      command_line.GetCommandLineString().c_str(), policy.get(),
      &last_result_code, &last_win_error, &temp_process_info);
  if (sandbox_result != sandbox::SBOX_ALL_OK) {
    LOG(DFATAL) << "Failed to spawn sandbox target: " << sandbox_result
                << " , last sandbox result : " << last_result_code
                << " , last windows error: " << last_win_error;
    return RESULT_CODE_FAILED_TO_START_SANDBOX_PROCESS;
  }

  base::Process process_handle = base::Process(temp_process_info.hProcess);
  base::win::ScopedHandle thread_handle =
      base::win::ScopedHandle(temp_process_info.hThread);

  // Because objects on the stack on destroyed in reverse order of allocation,
  // an early return from |StartSandboxTarget| will first call
  // process_handle.Terminate() and then close process_handle.
  base::ScopedClosureRunner terminate_process_on_failure(
      base::BindOnce(base::IgnoreResult(&base::Process::Terminate),
                     base::Unretained(&process_handle), -1, true));

  if (hooks) {
    ResultCode result_code =
        hooks->TargetSpawned(process_handle, thread_handle);
    if (result_code != RESULT_CODE_SUCCESS)
      return result_code;
  }

  // The target is spawned paused, so resume it.
  if (::ResumeThread(thread_handle.Get()) == static_cast<DWORD>(-1)) {
    PLOG(ERROR) << "Failed to resume thread";
    return RESULT_CODE_FAILED_TO_START_SANDBOX_PROCESS;
  }

  // Wait for the sandboxed process to signal it is ready, or for the process
  // to exit, indicating a failure.
  std::vector<HANDLE> wait_handles{init_done_event->handle(),
                                   process_handle.Handle()};
  DWORD wait_result = ::WaitForMultipleObjects(
      wait_handles.size(), wait_handles.data(), /*bWaitAll=*/false, INFINITE);
  // WAIT_OBJECT_0 is the first handle in the vector, so if we got any other
  // result it is a failure.
  if (wait_result != WAIT_OBJECT_0) {
    if (wait_result == WAIT_OBJECT_0 + 1) {
      DWORD exit_code = -1;
      BOOL result = ::GetExitCodeProcess(process_handle.Handle(), &exit_code);
      DCHECK(result);
      LOG(ERROR)
          << "Sandboxed process exited before signaling it was initialized, "
             "exit code: "
          << exit_code;
    } else {
      PLOG(ERROR) << "::WaitForMultipleObjects returned an unexpected error, "
                  << wait_result;
    }
    return RESULT_CODE_FAILED_TO_START_SANDBOX_PROCESS;
  }

  if (hooks) {
    ResultCode result_code = hooks->TargetResumed();
    if (result_code != RESULT_CODE_SUCCESS)
      return result_code;
  }

  // Now that the target process has been started successfully, don't call the
  // failure closures. Instead transfer ownership of |process_handle| to a
  // global that will be cleaned up by the OS on exit, so that it can be polled
  // in |IsSandboxTargetRunning|.
  g_target_processes->emplace(type, base::Process(std::move(process_handle)));
  terminate_process_on_failure.ReplaceClosure(base::DoNothing::Once());
  notify_hooks_on_failure.ReplaceClosure(base::DoNothing::Once());

  return RESULT_CODE_SUCCESS;
}

bool IsSandboxTargetRunning(SandboxType type) {
  if (!g_target_processes)
    return false;
  auto type_process_iter = g_target_processes->find(type);
  if (type_process_iter == g_target_processes->end())
    return false;

  int exit_code = 0;
  if (!type_process_iter->second.WaitForExitWithTimeout(base::TimeDelta(),
                                                        &exit_code)) {
    LOG(ERROR) << "WaitForExitWithTimeout failed";
    return false;
  }

  return (exit_code == STILL_ACTIVE);
}

std::map<SandboxType, SystemResourceUsage> GetSandboxSystemResourceUsage() {
  std::map<SandboxType, SystemResourceUsage> result;
  if (g_target_processes) {
    for (const auto& type_process : *g_target_processes) {
      SystemResourceUsage stats;
      if (GetSystemResourceUsage(type_process.second.Handle(), &stats))
        result[type_process.first] = stats;
    }
  }
  return result;
}

ResultCode RunSandboxTarget(const base::CommandLine& command_line,
                            sandbox::TargetServices* sandbox_target_services,
                            SandboxTargetHooks* hooks) {
  CHECK(sandbox_target_services);
  sandbox::ResultCode sandbox_result = sandbox_target_services->Init();
  if (sandbox_result != sandbox::SBOX_ALL_OK) {
    LOG(ERROR) << "Failed to initialize sandbox TargetServices: "
               << sandbox_result;
    return RESULT_CODE_FAILED;
  }

  DCHECK(hooks);
  ResultCode result_code = hooks->TargetStartedWithHighPrivileges();
  if (result_code != RESULT_CODE_SUCCESS)
    return result_code;

  sandbox_target_services->LowerToken();

  NotifyInitializationDone();

  return hooks->TargetDroppedPrivileges(command_line);
}

}  // namespace chrome_cleaner
