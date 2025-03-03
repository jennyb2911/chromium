// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_ACCESSIBILITY_DEFAULT_ACCESSIBILITY_DELEGATE_H_
#define ASH_ACCESSIBILITY_DEFAULT_ACCESSIBILITY_DELEGATE_H_

#include "ash/accessibility/accessibility_delegate.h"
#include "ash/ash_export.h"
#include "base/macros.h"

namespace ash {

// Used in tests and non-production code like ash_shell_with_content.
class ASH_EXPORT DefaultAccessibilityDelegate : public AccessibilityDelegate {
 public:
  DefaultAccessibilityDelegate();
  ~DefaultAccessibilityDelegate() override;

  void SetMagnifierEnabled(bool enabled) override;
  bool IsMagnifierEnabled() const override;
  bool ShouldShowAccessibilityMenu() const override;
  void SaveScreenMagnifierScale(double scale) override;
  double GetSavedScreenMagnifierScale() override;
  void DispatchAccessibilityEvent(const ui::AXTreeID& tree_id,
                                  const std::vector<ui::AXTreeUpdate>& updates,
                                  const ui::AXEvent& event) override;
  void DispatchTreeDestroyedEvent(const ui::AXTreeID& tree_id) override;

 private:
  bool screen_magnifier_enabled_ = false;

  DISALLOW_COPY_AND_ASSIGN(DefaultAccessibilityDelegate);
};

}  // namespace ash

#endif  // ASH_ACCESSIBILITY_DEFAULT_ACCESSIBILITY_DELEGATE_H_
