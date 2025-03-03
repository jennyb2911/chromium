// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill_assistant/browser/script_executor.h"

#include <string>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/autofill/core/browser/autofill_profile.h"
#include "components/autofill_assistant/browser/protocol_utils.h"
#include "components/autofill_assistant/browser/service.h"
#include "components/autofill_assistant/browser/ui_controller.h"
#include "components/autofill_assistant/browser/web_controller.h"

namespace autofill_assistant {

ScriptExecutor::ScriptExecutor(const std::string& script_path,
                               ScriptExecutorDelegate* delegate)
    : script_path_(script_path), delegate_(delegate), weak_ptr_factory_(this) {
  DCHECK(delegate_);
}
ScriptExecutor::~ScriptExecutor() {}

void ScriptExecutor::Run(RunScriptCallback callback) {
  callback_ = std::move(callback);
  DCHECK(delegate_->GetService());

  delegate_->GetService()->GetActions(
      script_path_, delegate_->GetParameters(),
      base::BindOnce(&ScriptExecutor::OnGetActions,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ScriptExecutor::ShowStatusMessage(const std::string& message) {
  delegate_->GetUiController()->ShowStatusMessage(message);
}

void ScriptExecutor::ClickElement(const std::vector<std::string>& selectors,
                                  base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->ClickElement(selectors, std::move(callback));
}

void ScriptExecutor::ElementExists(const std::vector<std::string>& selectors,
                                   base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->ElementExists(selectors, std::move(callback));
}

void ScriptExecutor::ChooseAddress(
    base::OnceCallback<void(const std::string&)> callback) {
  delegate_->GetUiController()->ChooseAddress(std::move(callback));
}

void ScriptExecutor::FillAddressForm(const std::string& guid,
                                     const std::vector<std::string>& selectors,
                                     base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->FillAddressForm(guid, selectors,
                                                 std::move(callback));
}

void ScriptExecutor::ChooseCard(
    base::OnceCallback<void(const std::string&)> callback) {
  delegate_->GetUiController()->ChooseCard(std::move(callback));
}

void ScriptExecutor::FillCardForm(const std::string& guid,
                                  const std::vector<std::string>& selectors,
                                  base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->FillCardForm(guid, selectors,
                                              std::move(callback));
}

void ScriptExecutor::SelectOption(const std::vector<std::string>& selectors,
                                  const std::string& selected_option,
                                  base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->SelectOption(selectors, selected_option,
                                              std::move(callback));
}

void ScriptExecutor::FocusElement(const std::vector<std::string>& selectors,
                                  base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->FocusElement(selectors, std::move(callback));
}

void ScriptExecutor::GetFieldValue(
    const std::vector<std::string>& selectors,
    base::OnceCallback<void(const std::string&)> callback) {
  delegate_->GetWebController()->GetFieldValue(selectors, std::move(callback));
}

void ScriptExecutor::SetFieldValue(const std::vector<std::string>& selectors,
                                   const std::string& value,
                                   base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->SetFieldValue(selectors, value,
                                               std::move(callback));
}

const autofill::AutofillProfile* ScriptExecutor::GetAutofillProfile(
    const std::string& guid) {
  // TODO(crbug.com/806868): Implement GetAutofillProfile.
  return nullptr;
}

void ScriptExecutor::BuildNodeTree(const std::vector<std::string>& selectors,
                                   NodeProto* node_tree_out,
                                   base::OnceCallback<void(bool)> callback) {
  delegate_->GetWebController()->BuildNodeTree(selectors, node_tree_out,
                                               std::move(callback));
}

ClientMemory* ScriptExecutor::GetClientMemory() {
  return delegate_->GetClientMemory();
}

void ScriptExecutor::OnGetActions(bool result, const std::string& response) {
  if (!result) {
    std::move(callback_).Run(false);
    return;
  }
  processed_actions_.clear();
  actions_.clear();

  bool parse_result =
      ProtocolUtils::ParseActions(response, &last_server_payload_, &actions_);
  if (!parse_result) {
    std::move(callback_).Run(false);
    return;
  }

  if (actions_.empty()) {
    // Finished executing the script if there are no more actions.
    std::move(callback_).Run(true);
    return;
  }

  ProcessNextAction();
}

void ScriptExecutor::ProcessNextAction() {
  // We could get into a strange situation if ProcessNextAction is called before
  // the action was reported as processed, which should not happen. In that case
  // we could have more |processed_actions| than |actions_|.
  if (actions_.size() <= processed_actions_.size()) {
    DCHECK_EQ(actions_.size(), processed_actions_.size());
    // Request more actions to execute.
    GetNextActions();
    return;
  }

  Action* action = actions_[processed_actions_.size()].get();
  int delay_ms = action->proto().action_delay_ms();
  if (delay_ms > 0) {
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ScriptExecutor::ProcessAction,
                       weak_ptr_factory_.GetWeakPtr(), action),
        base::TimeDelta::FromMilliseconds(delay_ms));
  } else {
    ProcessAction(action);
  }
}

void ScriptExecutor::ProcessAction(Action* action) {
  action->ProcessAction(this, base::BindOnce(&ScriptExecutor::OnProcessedAction,
                                             weak_ptr_factory_.GetWeakPtr()));
}

void ScriptExecutor::GetNextActions() {
  delegate_->GetService()->GetNextActions(
      last_server_payload_, processed_actions_,
      base::BindOnce(&ScriptExecutor::OnGetActions,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ScriptExecutor::OnProcessedAction(
    std::unique_ptr<ProcessedActionProto> processed_action_proto) {
  processed_actions_.emplace_back(*processed_action_proto);
  if (processed_actions_.back().status() !=
      ProcessedActionStatus::ACTION_APPLIED) {
    // Report error immediately, interrupting action processing.
    GetNextActions();
    return;
  }
  ProcessNextAction();
}

}  // namespace autofill_assistant
