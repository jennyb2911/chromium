// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/chromeos/login/multidevice_setup_screen_handler.h"

#include "chrome/browser/chromeos/login/screens/multidevice_setup_screen.h"
#include "chrome/browser/ui/webui/chromeos/multidevice_setup/multidevice_setup_localized_strings_provider.h"
#include "chrome/grit/generated_resources.h"
#include "components/login/localized_values_builder.h"

namespace {

const char kJsScreenPath[] = "login.MultiDeviceSetupScreen";

}  // namespace

namespace chromeos {

MultiDeviceSetupScreenHandler::MultiDeviceSetupScreenHandler()
    : BaseScreenHandler(kScreenId) {
  set_call_js_prefix(kJsScreenPath);
}

MultiDeviceSetupScreenHandler::~MultiDeviceSetupScreenHandler() = default;

void MultiDeviceSetupScreenHandler::DeclareLocalizedValues(
    ::login::LocalizedValuesBuilder* builder) {
  multidevice_setup::AddLocalizedValuesToBuilder(builder);
}

void MultiDeviceSetupScreenHandler::Bind(MultiDeviceSetupScreen* screen) {
  BaseScreenHandler::SetBaseScreen(screen);
}

void MultiDeviceSetupScreenHandler::Show() {
  ShowScreen(kScreenId);
}

void MultiDeviceSetupScreenHandler::Hide() {}

void MultiDeviceSetupScreenHandler::Initialize() {}

}  // namespace chromeos
