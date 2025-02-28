// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/public/cpp/caption_buttons/frame_back_button.h"

#include "ash/public/cpp/ash_layout_constants.h"
#include "ash/public/cpp/caption_buttons/frame_caption_button.h"
#include "ash/public/cpp/vector_icons/vector_icons.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/hit_test.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/events/event_sink.h"
#include "ui/strings/grit/ui_strings.h"
#include "ui/views/widget/widget.h"

namespace ash {

FrameBackButton::FrameBackButton()
    : FrameCaptionButton(this, CAPTION_BUTTON_ICON_BACK, HTMENU) {
  SetPreferredSize(GetAshLayoutSize(AshLayoutSize::kNonBrowserCaption));
  SetAccessibleName(l10n_util::GetStringUTF16(IDS_APP_ACCNAME_BACK));
}

FrameBackButton::~FrameBackButton() = default;

void FrameBackButton::ButtonPressed(Button* sender, const ui::Event& event) {
  // Send up event as well as down event as ARC++ clients expect this sequence.
  // TODO(estade): this may not actually work in Mash, but thus far isn't used
  // there. Consider fixing or moving back to //ash/frame/.
  // https://crbug.com/887663
  aura::Window* root_window = GetWidget()->GetNativeWindow()->GetRootWindow();
  ui::KeyEvent press_key_event(ui::ET_KEY_PRESSED, ui::VKEY_BROWSER_BACK,
                               ui::EF_NONE);
  ignore_result(root_window->GetHost()->event_sink()->OnEventFromSource(
      &press_key_event));
  ui::KeyEvent release_key_event(ui::ET_KEY_RELEASED, ui::VKEY_BROWSER_BACK,
                                 ui::EF_NONE);
  ignore_result(root_window->GetHost()->event_sink()->OnEventFromSource(
      &release_key_event));
}

}  // namespace ash
