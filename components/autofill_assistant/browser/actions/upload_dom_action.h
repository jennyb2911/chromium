// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_ACTIONS_UPLOAD_DOM_ACTION_H_
#define COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_ACTIONS_UPLOAD_DOM_ACTION_H_

#include <string>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "components/autofill_assistant/browser/actions/action.h"

namespace autofill_assistant {

class UploadDomAction : public Action {
 public:
  explicit UploadDomAction(const ActionProto& proto);
  ~UploadDomAction() override;

  // Overrides Action:
  void ProcessAction(ActionDelegate* delegate,
                     ProcessActionCallback callback) override;

 private:
  void OnBuildNodeTree(ProcessActionCallback callback, bool status);

  base::WeakPtrFactory<UploadDomAction> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(UploadDomAction);
};

}  // namespace autofill_assistant
#endif  // COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_ACTIONS_UPLOAD_DOM_ACTION_H_
