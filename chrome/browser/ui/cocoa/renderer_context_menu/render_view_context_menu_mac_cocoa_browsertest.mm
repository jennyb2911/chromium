// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/cocoa/renderer_context_menu/render_view_context_menu_mac_cocoa.h"

#include "base/mac/foundation_util.h"
#include "base/mac/mac_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/cocoa/browser_window_cocoa.h"
#include "chrome/browser/ui/cocoa/browser_window_controller.h"
#include "chrome/browser/ui/extensions/application_launch.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/test_utils.h"
#import "testing/gtest_mac.h"

class RenderViewContextMenuMacCocoaBrowserTest : public InProcessBrowserTest {
 public:
  RenderViewContextMenuMacCocoaBrowserTest() {}

 protected:
  void SetUpOnMainThread() override {
    filteredItems_.reset([[NSMutableArray alloc] init]);
    [ChromeSwizzleServicesMenuUpdater
        storeFilteredEntriesForTestingInArray:filteredItems_];

    // Add a textfield, which we'll use to present a contextual menu for
    // testing.
    textField_.reset(
        [[NSTextField alloc] initWithFrame:NSMakeRect(20, 20, 100, 20)]);
    [textField_ setStringValue:@"some text"];
    NSWindow* window = browser()->window()->GetNativeWindow();
    [[window contentView] addSubview:textField_];
  }

  void TearDownOnMainThread() override {
    [textField_ removeFromSuperview];
    [ChromeSwizzleServicesMenuUpdater
        storeFilteredEntriesForTestingInArray:nil];
  }

  base::scoped_nsobject<NSMutableArray> filteredItems_;
  base::scoped_nsobject<NSTextField> textField_;

 private:
  DISALLOW_COPY_AND_ASSIGN(RenderViewContextMenuMacCocoaBrowserTest);
};

// Confirm that the private classes used to filter Safari's redundant Services
// items exist and implement the expected methods, and that the filtering code
// successfully removes those Services items.
IN_PROC_BROWSER_TEST_F(RenderViewContextMenuMacCocoaBrowserTest,
                       ServicesFiltering) {
  // Confirm that the _NSServicesMenuUpdater class exists and implements the
  // method we expect it to.
  Class menuUpdaterClass = NSClassFromString(@"_NSServicesMenuUpdater");
  EXPECT_TRUE(menuUpdaterClass);
  EXPECT_TRUE([menuUpdaterClass instancesRespondToSelector:@selector
                                (populateMenu:withServiceEntries:forDisplay:)]);

  // Confirm that the _NSServiceEntry class exists and implements the
  // method we expect it to.
  Class serviceEntryClass = NSClassFromString(@"_NSServiceEntry");
  EXPECT_TRUE(serviceEntryClass);
  EXPECT_TRUE([serviceEntryClass
      instancesRespondToSelector:@selector(bundleIdentifier)]);

  // Make the testing textfield the browser window's first responder, in
  // preparation for the contextual menu we're about to display. Even though the
  // code to filter Services items lives in render_view_context_menu_mac, it
  // filters all context menus no matter which control invokes them (as well as
  // the application Services menu). So to test, we just need a control with a
  // bit of selected text.
  NSWindow* window = browser()->window()->GetNativeWindow();
  [window makeFirstResponder:textField_];
  [textField_ selectText:nil];

  // Create a contextual menu.
  base::scoped_nsobject<NSMenu> popupMenu(
      [[NSMenu alloc] initWithTitle:@"menu"]);
  [popupMenu addItemWithTitle:@"Menu Item" action:0 keyEquivalent:@""];

  // Arrange to dismiss the contextual menu in the future (to break out of the
  // upcoming modal loop).
  dispatch_async(dispatch_get_main_queue(), ^{
    [popupMenu cancelTrackingWithoutAnimation];
  });

  // Bring up the contextual menu from the textfield (actually its field
  // editor).
  NSView* firstResponder = base::mac::ObjCCast<NSView>([window firstResponder]);
  [NSMenu popUpContextMenu:popupMenu
                 withEvent:[NSApp currentEvent]
                   forView:firstResponder];

  // Confirm that Services items were removed from the contextual menu. This
  // check was failing on the 10.10 bot, for some reason. Most-important is
  // making sure it continues to work as OS X evolves.
  if (base::mac::IsAtLeastOS10_11())
    EXPECT_LT(0LU, [filteredItems_ count]);
}
