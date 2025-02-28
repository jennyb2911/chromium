// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_ACTIONS_MOCK_ACTION_DELEGATE_H_
#define COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_ACTIONS_MOCK_ACTION_DELEGATE_H_

#include "base/callback.h"
#include "components/autofill_assistant/browser/actions/action_delegate.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace autofill_assistant {

class MockActionDelegate : public ActionDelegate {
 public:
  MockActionDelegate();
  ~MockActionDelegate() override;

  MOCK_METHOD1(ShowStatusMessage, void(const std::string& message));
  MOCK_METHOD2(ClickElement,
               void(const std::vector<std::string>& selectors,
                    base::OnceCallback<void(bool)> callback));
  MOCK_METHOD2(ElementExists,
               void(const std::vector<std::string>& selectors,
                    base::OnceCallback<void(bool)> callback));

  void ChooseAddress(
      base::OnceCallback<void(const std::string&)> callback) override {
    OnChooseAddress(callback);
  }

  MOCK_METHOD1(OnChooseAddress,
               void(base::OnceCallback<void(const std::string&)>& callback));

  void FillAddressForm(const std::string& guid,
                       const std::vector<std::string>& selectors,
                       base::OnceCallback<void(bool)> callback) override {
    OnFillAddressForm(guid, selectors, callback);
  }

  MOCK_METHOD3(OnFillAddressForm,
               void(const std::string& guid,
                    const std::vector<std::string>& selectors,
                    base::OnceCallback<void(bool)>& callback));

  void ChooseCard(
      base::OnceCallback<void(const std::string&)> callback) override {
    OnChooseCard(callback);
  }

  MOCK_METHOD1(OnChooseCard,
               void(base::OnceCallback<void(const std::string&)>& callback));

  void FillCardForm(const std::string& guid,
                    const std::vector<std::string>& selectors,
                    base::OnceCallback<void(bool)> callback) override {
    OnFillCardForm(guid, selectors, callback);
  }

  MOCK_METHOD3(OnFillCardForm,
               void(const std::string& guid,
                    const std::vector<std::string>& selectors,
                    base::OnceCallback<void(bool)>& callback));
  MOCK_METHOD3(SelectOption,
               void(const std::vector<std::string>& selectors,
                    const std::string& selected_option,
                    base::OnceCallback<void(bool)> callback));
  MOCK_METHOD2(FocusElement,
               void(const std::vector<std::string>& selectors,
                    base::OnceCallback<void(bool)> callback));

  void GetFieldValue(
      const std::vector<std::string>& selectors,
      base::OnceCallback<void(const std::string&)> callback) override {
    OnGetFieldValue(selectors, callback);
  }

  MOCK_METHOD2(OnGetFieldValue,
               void(const std::vector<std::string>& selectors,
                    base::OnceCallback<void(const std::string&)>& callback));

  void SetFieldValue(const std::vector<std::string>& selectors,
                     const std::string& value,
                     base::OnceCallback<void(bool)> callback) {
    OnSetFieldValue(selectors, value, callback);
  }

  MOCK_METHOD3(OnSetFieldValue,
               void(const std::vector<std::string>& selectors,
                    const std::string& value,
                    base::OnceCallback<void(bool)>& callback));
  MOCK_METHOD1(GetAutofillProfile,
               const autofill::AutofillProfile*(const std::string& guid));
  MOCK_METHOD3(BuildNodeTree,
               void(const std::vector<std::string>& selectors,
                    NodeProto* node_tree_out,
                    base::OnceCallback<void(bool)> callback));
  MOCK_METHOD0(GetClientMemory, ClientMemory*());
};

}  // namespace autofill_assistant

#endif  // COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_ACTIONS_MOCK_ACTION_DELEGATE_H_
