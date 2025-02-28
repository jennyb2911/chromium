// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/widget/widget_utils_mac.h"

#import "ui/views/cocoa/bridged_native_widget.h"

namespace views {

gfx::Size GetWindowSizeForClientSize(Widget* widget, const gfx::Size& size) {
  DCHECK(widget);
  return BridgedNativeWidgetImpl::GetWindowSizeForClientSize(
      widget->GetNativeWindow(), size);
}

}  // namespace views
