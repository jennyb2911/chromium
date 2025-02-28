// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/accessibility/ax_host_service.h"

#include "ash/test/ash_test_base.h"
#include "base/bind.h"
#include "base/macros.h"
#include "base/test/scoped_task_environment.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/accessibility/mojom/ax_host.mojom.h"

namespace ash {
namespace {

class TestAXRemoteHost : ax::mojom::AXRemoteHost {
 public:
  TestAXRemoteHost() : binding_(this) {}
  ~TestAXRemoteHost() override = default;

  ax::mojom::AXRemoteHostPtr CreateInterfacePtr() {
    ax::mojom::AXRemoteHostPtr ptr;
    binding_.Bind(mojo::MakeRequest(&ptr));
    return ptr;
  }

  // Simulates the real AXRemoteHost.
  void SetRemoteHostCallback(const ui::AXTreeID& tree_id, bool enabled) {
    tree_id_ = tree_id;
    OnAutomationEnabled(enabled);
  }

  // ax::mojom::AXRemoteHost:
  void OnAutomationEnabled(bool enabled) override {
    ++automation_enabled_count_;
    last_automation_enabled_ = enabled;
  }
  void PerformAction(const ui::AXActionData& action) override {
    ++perform_action_count_;
    last_action_ = action;
  }

  mojo::Binding<ax::mojom::AXRemoteHost> binding_;
  ui::AXTreeID tree_id_;
  int automation_enabled_count_ = 0;
  bool last_automation_enabled_ = false;
  int perform_action_count_ = 0;
  ui::AXActionData last_action_;

 private:
  DISALLOW_COPY_AND_ASSIGN(TestAXRemoteHost);
};

using AXHostServiceTest = AshTestBase;

TEST_F(AXHostServiceTest, AddClientThenEnable) {
  AXHostService service;
  TestAXRemoteHost remote;
  service.SetRemoteHost(remote.CreateInterfacePtr(),
                        base::BindOnce(&TestAXRemoteHost::SetRemoteHostCallback,
                                       base::Unretained(&remote)));
  service.FlushForTesting();

  // Remote received initial state.
  EXPECT_EQ(1, remote.automation_enabled_count_);
  EXPECT_FALSE(remote.last_automation_enabled_);
  EXPECT_EQ(service.tree_id_for_testing(), remote.tree_id_);

  AXHostService::SetAutomationEnabled(true);
  service.FlushForTesting();

  // Remote received updated state.
  EXPECT_EQ(2, remote.automation_enabled_count_);
  EXPECT_TRUE(remote.last_automation_enabled_);
}

TEST_F(AXHostServiceTest, EnableThenAddClient) {
  AXHostService service;
  AXHostService::SetAutomationEnabled(true);

  TestAXRemoteHost remote;
  service.SetRemoteHost(remote.CreateInterfacePtr(),
                        base::BindOnce(&TestAXRemoteHost::SetRemoteHostCallback,
                                       base::Unretained(&remote)));
  service.FlushForTesting();

  // Remote received initial state.
  EXPECT_EQ(1, remote.automation_enabled_count_);
  EXPECT_TRUE(remote.last_automation_enabled_);
  EXPECT_EQ(service.tree_id_for_testing(), remote.tree_id_);
}

TEST_F(AXHostServiceTest, PerformAction) {
  AXHostService service;
  AXHostService::SetAutomationEnabled(true);

  TestAXRemoteHost remote;
  service.SetRemoteHost(remote.CreateInterfacePtr(),
                        base::BindOnce(&TestAXRemoteHost::SetRemoteHostCallback,
                                       base::Unretained(&remote)));
  service.FlushForTesting();

  ui::AXActionData action;
  action.action = ax::mojom::Action::kScrollUp;
  service.PerformAction(action);
  service.FlushForTesting();

  // Remote interface received the action.
  EXPECT_EQ(1, remote.perform_action_count_);
  EXPECT_EQ(ax::mojom::Action::kScrollUp, remote.last_action_.action);
}

}  // namespace
}  // namespace ash
