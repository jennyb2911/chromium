// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ui/views/cocoa/bridged_native_widget.h"

#import <Cocoa/Cocoa.h>

#include <memory>

#import "base/mac/foundation_util.h"
#import "base/mac/mac_util.h"
#import "base/mac/scoped_objc_class_swizzler.h"
#import "base/mac/sdk_forward_declarations.h"
#include "base/macros.h"
#include "base/strings/stringprintf.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_task_environment.h"
#import "testing/gtest_mac.h"
#import "ui/base/cocoa/window_size_constants.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/material_design/material_design_controller.h"
#import "ui/base/test/cocoa_helper.h"
#include "ui/base/test/material_design_controller_test_api.h"
#include "ui/events/test/cocoa_test_event_utils.h"
#import "ui/gfx/mac/coordinate_conversion.h"
#import "ui/views/cocoa/bridged_content_view.h"
#import "ui/views/cocoa/bridged_native_widget_host_impl.h"
#import "ui/views/cocoa/native_widget_mac_nswindow.h"
#import "ui/views/cocoa/views_nswindow_delegate.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/controls/textfield/textfield_model.h"
#include "ui/views/test/test_views_delegate.h"
#include "ui/views/view.h"
#include "ui/views/widget/native_widget_mac.h"
#include "ui/views/widget/root_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

using base::ASCIIToUTF16;
using base::SysNSStringToUTF8;
using base::SysNSStringToUTF16;
using base::SysUTF8ToNSString;
using base::SysUTF16ToNSString;

#define EXPECT_EQ_RANGE(a, b)        \
  EXPECT_EQ(a.location, b.location); \
  EXPECT_EQ(a.length, b.length);

// Helpers to verify an expectation against both the actual toolkit-views
// behaviour and the Cocoa behaviour.

#define EXPECT_NSEQ_3(expected_literal, expected_cocoa, actual_views) \
  EXPECT_NSEQ(expected_literal, actual_views);                        \
  EXPECT_NSEQ(expected_cocoa, actual_views);

#define EXPECT_EQ_RANGE_3(expected_literal, expected_cocoa, actual_views) \
  EXPECT_EQ_RANGE(expected_literal, actual_views);                        \
  EXPECT_EQ_RANGE(expected_cocoa, actual_views);

#define EXPECT_EQ_3(expected_literal, expected_cocoa, actual_views) \
  EXPECT_EQ(expected_literal, actual_views);                        \
  EXPECT_EQ(expected_cocoa, actual_views);

namespace {

// Implemented NSResponder action messages for use in tests.
NSArray* const kMoveActions = @[
  @"moveForward:",
  @"moveRight:",
  @"moveBackward:",
  @"moveLeft:",
  @"moveUp:",
  @"moveDown:",
  @"moveWordForward:",
  @"moveWordBackward:",
  @"moveToBeginningOfLine:",
  @"moveToEndOfLine:",
  @"moveToBeginningOfParagraph:",
  @"moveToEndOfParagraph:",
  @"moveToEndOfDocument:",
  @"moveToBeginningOfDocument:",
  @"pageDown:",
  @"pageUp:",
  @"moveWordRight:",
  @"moveWordLeft:",
  @"moveToLeftEndOfLine:",
  @"moveToRightEndOfLine:"
];

NSArray* const kSelectActions = @[
  @"moveBackwardAndModifySelection:",
  @"moveForwardAndModifySelection:",
  @"moveWordForwardAndModifySelection:",
  @"moveWordBackwardAndModifySelection:",
  @"moveUpAndModifySelection:",
  @"moveDownAndModifySelection:",
  @"moveToBeginningOfLineAndModifySelection:",
  @"moveToEndOfLineAndModifySelection:",
  @"moveToBeginningOfParagraphAndModifySelection:",
  @"moveToEndOfParagraphAndModifySelection:",
  @"moveToEndOfDocumentAndModifySelection:",
  @"moveToBeginningOfDocumentAndModifySelection:",
  @"pageDownAndModifySelection:",
  @"pageUpAndModifySelection:",
  @"moveParagraphForwardAndModifySelection:",
  @"moveParagraphBackwardAndModifySelection:",
  @"moveRightAndModifySelection:",
  @"moveLeftAndModifySelection:",
  @"moveWordRightAndModifySelection:",
  @"moveWordLeftAndModifySelection:",
  @"moveToLeftEndOfLineAndModifySelection:",
  @"moveToRightEndOfLineAndModifySelection:"
];

NSArray* const kDeleteActions = @[
  @"deleteForward:", @"deleteBackward:", @"deleteWordForward:",
  @"deleteWordBackward:", @"deleteToBeginningOfLine:", @"deleteToEndOfLine:",
  @"deleteToBeginningOfParagraph:", @"deleteToEndOfParagraph:"
];

NSArray* const kMiscActions =
    @[ @"insertText:", @"cancelOperation:", @"transpose:", @"yank:" ];

// Empty range shortcut for readibility.
NSRange EmptyRange() {
  return NSMakeRange(NSNotFound, 0);
}

// Sets |composition_text| as the composition text with caret placed at
// |caret_pos| and updates |caret_range|.
void SetCompositionText(ui::TextInputClient* client,
                        const base::string16& composition_text,
                        const int caret_pos,
                        NSRange* caret_range) {
  ui::CompositionText composition;
  composition.selection = gfx::Range(caret_pos);
  composition.text = composition_text;
  client->SetCompositionText(composition);
  if (caret_range)
    *caret_range = NSMakeRange(caret_pos, 0);
}

// Returns a zero width rectangle corresponding to current caret position.
gfx::Rect GetCaretBounds(const ui::TextInputClient* client) {
  gfx::Rect caret_bounds = client->GetCaretBounds();
  caret_bounds.set_width(0);
  return caret_bounds;
}

// Returns a zero width rectangle corresponding to caret bounds when it's placed
// at |caret_pos| and updates |caret_range|.
gfx::Rect GetCaretBoundsForPosition(ui::TextInputClient* client,
                                    const base::string16& composition_text,
                                    const int caret_pos,
                                    NSRange* caret_range) {
  SetCompositionText(client, composition_text, caret_pos, caret_range);
  return GetCaretBounds(client);
}

// Returns the expected boundary rectangle for characters of |composition_text|
// within the |query_range|.
gfx::Rect GetExpectedBoundsForRange(ui::TextInputClient* client,
                                    const base::string16& composition_text,
                                    NSRange query_range) {
  gfx::Rect left_caret = GetCaretBoundsForPosition(
      client, composition_text, query_range.location, nullptr);
  gfx::Rect right_caret = GetCaretBoundsForPosition(
      client, composition_text, query_range.location + query_range.length,
      nullptr);

  // The expected bounds correspond to the area between the left and right caret
  // positions.
  return gfx::Rect(left_caret.x(), left_caret.y(),
                   right_caret.x() - left_caret.x(), left_caret.height());
}

// Uses the NSTextInputClient protocol to extract a substring from |view|.
NSString* GetViewStringForRange(NSView<NSTextInputClient>* view,
                                NSRange range) {
  return [[view attributedSubstringForProposedRange:range actualRange:nullptr]
      string];
}

// The behavior of NSTextView for RTL strings is buggy for some move and select
// commands, but only when the command is received when there is a selection
// active. E.g. moveRight: moves a cursor right in an RTL string, but it moves
// to the left-end of a selection. See TestEditingCommands() for specifics.
// This is filed as rdar://27863290.
bool IsRTLMoveBuggy(SEL sel) {
  return sel == @selector(moveWordRight:) || sel == @selector(moveWordLeft:) ||
         sel == @selector(moveRight:) || sel == @selector(moveLeft:);
}
bool IsRTLSelectBuggy(SEL sel) {
  return sel == @selector(moveWordRightAndModifySelection:) ||
         sel == @selector(moveWordLeftAndModifySelection:) ||
         sel == @selector(moveRightAndModifySelection:) ||
         sel == @selector(moveLeftAndModifySelection:);
}

// Used by InterpretKeyEventsDonorForNSView to simulate IME behavior.
using InterpretKeyEventsCallback = base::RepeatingCallback<void(id)>;
InterpretKeyEventsCallback* g_fake_interpret_key_events = nullptr;

// Used by UpdateWindowsDonorForNSApp to hook -[NSApp updateWindows].
base::RepeatingClosure* g_update_windows_closure = nullptr;

// Used to provide a return value for +[NSTextInputContext currentInputContext].
NSTextInputContext* g_fake_current_input_context = nullptr;

}  // namespace

// Subclass of BridgedContentView with an override of interpretKeyEvents:. Note
// the size of the class must match BridgedContentView since the method table
// is swapped out at runtime. This is basically a mock, but mocks are banned
// under ui/views. Method swizzling causes these tests to flake when
// parallelized in the same process.
@interface InterpretKeyEventMockedBridgedContentView : BridgedContentView
@end

@implementation InterpretKeyEventMockedBridgedContentView

- (void)interpretKeyEvents:(NSArray<NSEvent*>*)eventArray {
  ASSERT_TRUE(g_fake_interpret_key_events);
  g_fake_interpret_key_events->Run(self);
}

@end

@interface UpdateWindowsDonorForNSApp : NSApplication
@end

@implementation UpdateWindowsDonorForNSApp

- (void)updateWindows {
  ASSERT_TRUE(g_update_windows_closure);
  g_update_windows_closure->Run();
}

@end
@interface CurrentInputContextDonorForNSTextInputContext : NSTextInputContext
@end

@implementation CurrentInputContextDonorForNSTextInputContext

+ (NSTextInputContext*)currentInputContext {
  return g_fake_current_input_context;
}

@end

// Class to override -[NSWindow toggleFullScreen:] to a no-op. This simulates
// NSWindow's behavior when attempting to toggle fullscreen state again, when
// the last attempt failed but Cocoa has not yet sent
// windowDidFailToEnterFullScreen:.
@interface BridgedNativeWidgetTestWindow : NativeWidgetMacNSWindow {
 @private
  BOOL ignoreToggleFullScreen_;
  int ignoredToggleFullScreenCount_;
}
@property(assign, nonatomic) BOOL ignoreToggleFullScreen;
@property(readonly, nonatomic) int ignoredToggleFullScreenCount;
@end

@implementation BridgedNativeWidgetTestWindow

@synthesize ignoreToggleFullScreen = ignoreToggleFullScreen_;
@synthesize ignoredToggleFullScreenCount = ignoredToggleFullScreenCount_;

- (void)performSelector:(SEL)aSelector
             withObject:(id)anArgument
             afterDelay:(NSTimeInterval)delay {
  // This is used in simulations without a message loop. Don't start a message
  // loop since that would expose the tests to system notifications and
  // potential flakes. Instead, just pretend the message loop is flushed here.
  if (ignoreToggleFullScreen_ && aSelector == @selector(toggleFullScreen:))
    [self toggleFullScreen:anArgument];
  else
    [super performSelector:aSelector withObject:anArgument afterDelay:delay];
}

- (void)toggleFullScreen:(id)sender {
  if (ignoreToggleFullScreen_)
    ++ignoredToggleFullScreenCount_;
  else
    [super toggleFullScreen:sender];
}

@end

namespace views {
namespace test {

// Provides the |parent| argument to construct a BridgedNativeWidgetImpl.
class MockNativeWidgetMac : public NativeWidgetMac {
 public:
  explicit MockNativeWidgetMac(internal::NativeWidgetDelegate* delegate)
      : NativeWidgetMac(delegate) {}
  using NativeWidgetMac::bridge_impl;
  using NativeWidgetMac::bridge_host_for_testing;

  // internal::NativeWidgetPrivate:
  void InitNativeWidget(const Widget::InitParams& params) override {
    ownership_ = params.ownership;

    base::scoped_nsobject<NativeWidgetMacNSWindow> window(
        [[BridgedNativeWidgetTestWindow alloc]
            initWithContentRect:ui::kWindowSizeDeterminedLater
                      styleMask:NSBorderlessWindowMask
                        backing:NSBackingStoreBuffered
                          defer:NO]);
    bridge_host_for_testing()->CreateLocalBridge(window, params.parent);
    bridge_host_for_testing()->InitWindow(params);

    // Usually the bridge gets initialized here. It is skipped to run extra
    // checks in tests, and so that a second window isn't created.
    delegate()->OnNativeWidgetCreated(true);

    // To allow events to dispatch to a view, it needs a way to get focus.
    bridge_host_for_testing()->SetFocusManager(GetWidget()->GetFocusManager());
  }

  void ReorderNativeViews() override {
    // Called via Widget::Init to set the content view. No-op in these tests.
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MockNativeWidgetMac);
};

// Helper test base to construct a BridgedNativeWidgetImpl with a valid parent.
class BridgedNativeWidgetTestBase : public ui::CocoaTest {
 public:
  struct SkipInitialization {};

  BridgedNativeWidgetTestBase()
      : widget_(new Widget),
        native_widget_mac_(new MockNativeWidgetMac(widget_.get())) {}

  explicit BridgedNativeWidgetTestBase(SkipInitialization tag)
      : native_widget_mac_(nullptr) {}

  BridgedNativeWidgetImpl* bridge() {
    return native_widget_mac_->bridge_impl();
  }
  BridgedNativeWidgetHostImpl* bridge_host() {
    return native_widget_mac_->bridge_host_for_testing();
  }

  // Overridden from testing::Test:
  void SetUp() override {
    ui::CocoaTest::SetUp();

    // MaterialDesignController leaks state across tests. See
    // http://crbug.com/656871.
    ui::test::MaterialDesignControllerTestAPI::Uninitialize();
    ui::MaterialDesignController::Initialize();

    init_params_.native_widget = native_widget_mac_;

    // Use a frameless window, otherwise Widget will try to center the window
    // before the tests covering the Init() flow are ready to do that.
    init_params_.type = Widget::InitParams::TYPE_WINDOW_FRAMELESS;

    // To control the lifetime without an actual window that must be closed,
    // tests in this file need to use WIDGET_OWNS_NATIVE_WIDGET.
    init_params_.ownership = Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;

    // Opacity defaults to "infer" which is usually updated by ViewsDelegate.
    init_params_.opacity = Widget::InitParams::OPAQUE_WINDOW;

    init_params_.bounds = gfx::Rect(100, 100, 100, 100);

    if (native_widget_mac_)
      native_widget_mac_->GetWidget()->Init(init_params_);
  }

  void TearDown() override {
    // ui::CocoaTest::TearDown will wait until all NSWindows are destroyed, so
    // be sure to destroy the widget (which will destroy its NSWindow)
    // beforehand.
    widget_.reset();
    ui::test::MaterialDesignControllerTestAPI::Uninitialize();
    ui::CocoaTest::TearDown();
  }

  NSWindow* bridge_window() const {
    if (native_widget_mac_->bridge_impl())
      return native_widget_mac_->bridge_impl()->ns_window();
    return nil;
  }

 protected:
  std::unique_ptr<Widget> widget_;
  MockNativeWidgetMac* native_widget_mac_;  // Weak. Owned by |widget_|.

  // Make the InitParams available to tests to cover initialization codepaths.
  Widget::InitParams init_params_;

 private:
  TestViewsDelegate test_views_delegate_;

  DISALLOW_COPY_AND_ASSIGN(BridgedNativeWidgetTestBase);
};

class BridgedNativeWidgetTest : public BridgedNativeWidgetTestBase,
                                public TextfieldController {
 public:
  using HandleKeyEventCallback =
      base::RepeatingCallback<bool(Textfield*, const ui::KeyEvent& key_event)>;

  BridgedNativeWidgetTest();
  ~BridgedNativeWidgetTest() override;

  // Install a textfield with input type |text_input_type| in the view hierarchy
  // and make it the text input client. Also initializes |dummy_text_view_|.
  Textfield* InstallTextField(
      const base::string16& text,
      ui::TextInputType text_input_type = ui::TEXT_INPUT_TYPE_TEXT);
  Textfield* InstallTextField(const std::string& text);

  // Returns the actual current text for |ns_view_|, or the selected substring.
  NSString* GetActualText();
  NSString* GetActualSelectedText();

  // Returns the expected current text from |dummy_text_view_|, or the selected
  // substring.
  NSString* GetExpectedText();
  NSString* GetExpectedSelectedText();

  // Returns the actual selection range for |ns_view_|.
  NSRange GetActualSelectionRange();

  // Returns the expected selection range from |dummy_text_view_|.
  NSRange GetExpectedSelectionRange();

  // Set the selection range for the installed textfield and |dummy_text_view_|.
  void SetSelectionRange(NSRange range);

  // Perform command |sel| on |ns_view_| and |dummy_text_view_|.
  void PerformCommand(SEL sel);

  // Make selection from |start| to |end| on installed views::Textfield and
  // |dummy_text_view_|. If |start| > |end|, extend selection to left from
  // |start|.
  void MakeSelection(int start, int end);

  // Helper method to set the private |keyDownEvent_| field on |ns_view_|.
  void SetKeyDownEvent(NSEvent* event);

  // Sets a callback to run on the next HandleKeyEvent().
  void SetHandleKeyEventCallback(HandleKeyEventCallback callback);

  // testing::Test:
  void SetUp() override;
  void TearDown() override;

  // TextfieldController:
  bool HandleKeyEvent(Textfield* sender,
                      const ui::KeyEvent& key_event) override;

 protected:
  // Test delete to beginning of line or paragraph based on |sel|. |sel| can be
  // either deleteToBeginningOfLine: or deleteToBeginningOfParagraph:.
  void TestDeleteBeginning(SEL sel);

  // Test delete to end of line or paragraph based on |sel|. |sel| can be
  // either deleteToEndOfLine: or deleteToEndOfParagraph:.
  void TestDeleteEnd(SEL sel);

  // Test editing commands in |selectors| against the expectations set by
  // |dummy_text_view_|. This is done by selecting every substring within a set
  // of test strings (both RTL and non-RTL by default) and performing every
  // selector on both the NSTextView and the BridgedContentView hosting a
  // focused views::TextField to ensure the resulting text and selection ranges
  // match. |selectors| is an NSArray of NSStrings. |cases| determines whether
  // RTL strings are to be tested.
  void TestEditingCommands(NSArray* selectors);

  std::unique_ptr<views::View> view_;

  // Weak. Owned by bridge().
  BridgedContentView* ns_view_;

  // An NSTextView which helps set the expectations for our tests.
  base::scoped_nsobject<NSTextView> dummy_text_view_;

  HandleKeyEventCallback handle_key_event_callback_;

  base::test::ScopedTaskEnvironment scoped_task_environment_;

 private:
  DISALLOW_COPY_AND_ASSIGN(BridgedNativeWidgetTest);
};

// Class that counts occurrences of a VKEY_RETURN accelerator, marking them
// processed.
class EnterAcceleratorView : public View {
 public:
  EnterAcceleratorView() { AddAccelerator({ui::VKEY_RETURN, 0}); }
  int count() const { return count_; }

  // View:
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override {
    ++count_;
    return true;
  }

 private:
  int count_ = 0;
};

BridgedNativeWidgetTest::BridgedNativeWidgetTest()
    : scoped_task_environment_(
          base::test::ScopedTaskEnvironment::MainThreadType::UI) {}

BridgedNativeWidgetTest::~BridgedNativeWidgetTest() {
}

Textfield* BridgedNativeWidgetTest::InstallTextField(
    const base::string16& text,
    ui::TextInputType text_input_type) {
  Textfield* textfield = new Textfield();
  textfield->SetText(text);
  textfield->SetTextInputType(text_input_type);
  textfield->set_controller(this);
  view_->RemoveAllChildViews(true);
  view_->AddChildView(textfield);
  textfield->SetBoundsRect(init_params_.bounds);

  // Request focus so the InputMethod can dispatch events to the RootView, and
  // have them delivered to the textfield. Note that focusing a textfield
  // schedules a task to flash the cursor, so this requires |message_loop_|.
  textfield->RequestFocus();

  [ns_view_ setTextInputClient:textfield];

  // Initialize the dummy text view. Initializing this with NSZeroRect causes
  // weird NSTextView behavior on OSX 10.9.
  dummy_text_view_.reset(
      [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)]);
  [dummy_text_view_ setString:SysUTF16ToNSString(text)];
  return textfield;
}

Textfield* BridgedNativeWidgetTest::InstallTextField(const std::string& text) {
  return InstallTextField(base::ASCIIToUTF16(text));
}

NSString* BridgedNativeWidgetTest::GetActualText() {
  return GetViewStringForRange(ns_view_, NSMakeRange(0, NSUIntegerMax));
}

NSString* BridgedNativeWidgetTest::GetActualSelectedText() {
  return GetViewStringForRange(ns_view_, [ns_view_ selectedRange]);
}

NSString* BridgedNativeWidgetTest::GetExpectedText() {
  return GetViewStringForRange(dummy_text_view_, NSMakeRange(0, NSUIntegerMax));
}

NSString* BridgedNativeWidgetTest::GetExpectedSelectedText() {
  return GetViewStringForRange(dummy_text_view_,
                               [dummy_text_view_ selectedRange]);
}

NSRange BridgedNativeWidgetTest::GetActualSelectionRange() {
  return [ns_view_ selectedRange];
}

NSRange BridgedNativeWidgetTest::GetExpectedSelectionRange() {
  return [dummy_text_view_ selectedRange];
}

void BridgedNativeWidgetTest::SetSelectionRange(NSRange range) {
  ui::TextInputClient* client = [ns_view_ textInputClient];
  client->SetSelectionRange(gfx::Range(range));

  [dummy_text_view_ setSelectedRange:range];
}

void BridgedNativeWidgetTest::PerformCommand(SEL sel) {
  [ns_view_ doCommandBySelector:sel];
  [dummy_text_view_ doCommandBySelector:sel];
}

void BridgedNativeWidgetTest::MakeSelection(int start, int end) {
  ui::TextInputClient* client = [ns_view_ textInputClient];
  const gfx::Range range(start, end);

  // Although a gfx::Range is directed, the underlying model will not choose an
  // affinity until the cursor is moved.
  client->SetSelectionRange(range);

  // Set the range without an affinity. The first @selector sent to the text
  // field determines the affinity. Note that Range::ToNSRange() may discard
  // the direction since NSRange has no direction.
  [dummy_text_view_ setSelectedRange:range.ToNSRange()];
}

void BridgedNativeWidgetTest::SetKeyDownEvent(NSEvent* event) {
  [ns_view_ setValue:event forKey:@"keyDownEvent_"];
}

void BridgedNativeWidgetTest::SetHandleKeyEventCallback(
    HandleKeyEventCallback callback) {
  handle_key_event_callback_ = std::move(callback);
}

void BridgedNativeWidgetTest::SetUp() {
  BridgedNativeWidgetTestBase::SetUp();

  view_.reset(new views::internal::RootView(widget_.get()));
  base::scoped_nsobject<NSWindow> window([bridge_window() retain]);

  // The delegate should exist before setting the root view.
  EXPECT_TRUE([window delegate]);
  bridge_host()->SetRootView(view_.get());
  bridge()->CreateContentView(view_->bounds());
  ns_view_ = bridge()->ns_view();

  // Pretend it has been shown via NativeWidgetMac::Show().
  [window orderFront:nil];
  [window makeFirstResponder:bridge()->ns_view()];
}

void BridgedNativeWidgetTest::TearDown() {
  // Clear kill buffer so that no state persists between tests.
  TextfieldModel::ClearKillBuffer();

  if (bridge_host()) {
    bridge_host()->SetRootView(nullptr);
    bridge()->DestroyContentView();
  }
  view_.reset();
  BridgedNativeWidgetTestBase::TearDown();
}

bool BridgedNativeWidgetTest::HandleKeyEvent(Textfield* sender,
                                             const ui::KeyEvent& key_event) {
  if (handle_key_event_callback_)
    return handle_key_event_callback_.Run(sender, key_event);
  return false;
}

void BridgedNativeWidgetTest::TestDeleteBeginning(SEL sel) {
  InstallTextField("foo bar baz");
  EXPECT_EQ_RANGE_3(NSMakeRange(11, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move the caret to the beginning of the line.
  SetSelectionRange(NSMakeRange(0, 0));
  // Verify no deletion takes place.
  PerformCommand(sel);
  EXPECT_NSEQ_3(@"foo bar baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move the caret as- "foo| bar baz".
  SetSelectionRange(NSMakeRange(3, 0));
  PerformCommand(sel);
  // Verify state is "| bar baz".
  EXPECT_NSEQ_3(@" bar baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Make a selection as- " bar |baz|".
  SetSelectionRange(NSMakeRange(5, 3));
  PerformCommand(sel);
  // Verify only the selection is deleted so that the state is " bar |".
  EXPECT_NSEQ_3(@" bar ", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(5, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Verify yanking inserts the deleted text.
  PerformCommand(@selector(yank:));
  EXPECT_NSEQ_3(@" bar baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(8, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
}

void BridgedNativeWidgetTest::TestDeleteEnd(SEL sel) {
  InstallTextField("foo bar baz");
  EXPECT_EQ_RANGE_3(NSMakeRange(11, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Caret is at the end of the line. Verify no deletion takes place.
  PerformCommand(sel);
  EXPECT_NSEQ_3(@"foo bar baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(11, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move the caret as- "foo bar| baz".
  SetSelectionRange(NSMakeRange(7, 0));
  PerformCommand(sel);
  // Verify state is "foo bar|".
  EXPECT_NSEQ_3(@"foo bar", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(7, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Make a selection as- "|foo |bar".
  SetSelectionRange(NSMakeRange(0, 4));
  PerformCommand(sel);
  // Verify only the selection is deleted so that the state is "|bar".
  EXPECT_NSEQ_3(@"bar", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Verify yanking inserts the deleted text.
  PerformCommand(@selector(yank:));
  EXPECT_NSEQ_3(@"foo bar", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(4, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
}

void BridgedNativeWidgetTest::TestEditingCommands(NSArray* selectors) {
  struct {
    base::string16 test_string;
    bool is_rtl;
  } test_cases[] = {
      {base::WideToUTF16(L"ab c"), false},
      {base::WideToUTF16(L"\x0634\x0632 \x064A"), true},
  };

  for (const auto& test_case : test_cases) {
    for (NSString* selector_string in selectors) {
      SEL sel = NSSelectorFromString(selector_string);
      const int len = test_case.test_string.length();
      for (int i = 0; i <= len; i++) {
        for (int j = 0; j <= len; j++) {
          SCOPED_TRACE(base::StringPrintf(
              "Testing range [%d-%d] for case %s and selector %s\n", i, j,
              base::UTF16ToUTF8(test_case.test_string).c_str(),
              base::SysNSStringToUTF8(selector_string).c_str()));

          InstallTextField(test_case.test_string);
          MakeSelection(i, j);

          // Sanity checks for MakeSelection().
          EXPECT_NSEQ(GetExpectedSelectedText(), GetActualSelectedText());
          EXPECT_EQ_RANGE_3(NSMakeRange(std::min(i, j), std::abs(i - j)),
                            GetExpectedSelectionRange(),
                            GetActualSelectionRange());

          // Bail out early for selection-modifying commands that are buggy in
          // Cocoa, since the selected text will not match.
          if (test_case.is_rtl && i != j && IsRTLSelectBuggy(sel))
            continue;

          PerformCommand(sel);
          EXPECT_NSEQ(GetExpectedSelectedText(), GetActualSelectedText());
          EXPECT_NSEQ(GetExpectedText(), GetActualText());

          // Spot-check some Cocoa RTL bugs. These only manifest when there is a
          // selection (i != j), not for regular cursor moves.
          if (test_case.is_rtl && i != j && IsRTLMoveBuggy(sel)) {
            if (sel == @selector(moveRight:)) {
              // Surely moving right with an rtl string moves to the start of a
              // range (i.e. min). But Cocoa moves to the end.
              EXPECT_EQ_RANGE(NSMakeRange(std::max(i, j), 0),
                              GetExpectedSelectionRange());
              EXPECT_EQ_RANGE(NSMakeRange(std::min(i, j), 0),
                              GetActualSelectionRange());
            } else if (sel == @selector(moveLeft:)) {
              EXPECT_EQ_RANGE(NSMakeRange(std::min(i, j), 0),
                              GetExpectedSelectionRange());
              EXPECT_EQ_RANGE(NSMakeRange(std::max(i, j), 0),
                              GetActualSelectionRange());
            }
            continue;
          }

          EXPECT_EQ_RANGE(GetExpectedSelectionRange(),
                          GetActualSelectionRange());
        }
      }
    }
  }
}

// The TEST_VIEW macro expects the view it's testing to have a superview. In
// these tests, the NSView bridge is a contentView, at the root. These mimic
// what TEST_VIEW usually does.
TEST_F(BridgedNativeWidgetTest, BridgedNativeWidgetTest_TestViewAddRemove) {
  base::scoped_nsobject<BridgedContentView> view([bridge()->ns_view() retain]);
  base::scoped_nsobject<NSWindow> window([bridge_window() retain]);
  EXPECT_NSEQ([window contentView], view);
  EXPECT_NSEQ(window, [view window]);

  // The superview of a contentView is an NSNextStepFrame.
  EXPECT_TRUE([view superview]);
  EXPECT_TRUE([view bridge]);

  // Ensure the tracking area to propagate mouseMoved: events to the RootView is
  // installed.
  EXPECT_EQ(1u, [[view trackingAreas] count]);

  // Closing the window should tear down the C++ bridge, remove references to
  // any C++ objects in the ObjectiveC object, and remove it from the hierarchy.
  [window close];
  EXPECT_FALSE([view bridge]);
  EXPECT_FALSE([view superview]);
  EXPECT_FALSE([view window]);
  EXPECT_EQ(0u, [[view trackingAreas] count]);
  EXPECT_FALSE([window contentView]);
  EXPECT_FALSE([window delegate]);
}

TEST_F(BridgedNativeWidgetTest, BridgedNativeWidgetTest_TestViewDisplay) {
  [bridge()->ns_view() display];
}

// Test that resizing the window resizes the root view appropriately.
TEST_F(BridgedNativeWidgetTest, ViewSizeTracksWindow) {
  const int kTestNewWidth = 400;
  const int kTestNewHeight = 300;

  // |bridge_window()| is borderless, so these should align.
  NSSize window_size = [bridge_window() frame].size;
  EXPECT_EQ(view_->width(), static_cast<int>(window_size.width));
  EXPECT_EQ(view_->height(), static_cast<int>(window_size.height));

  // Make sure a resize actually occurs.
  EXPECT_NE(kTestNewWidth, view_->width());
  EXPECT_NE(kTestNewHeight, view_->height());

  [bridge_window() setFrame:NSMakeRect(0, 0, kTestNewWidth, kTestNewHeight)
                    display:NO];
  EXPECT_EQ(kTestNewWidth, view_->width());
  EXPECT_EQ(kTestNewHeight, view_->height());
}

TEST_F(BridgedNativeWidgetTest, GetInputMethodShouldNotReturnNull) {
  EXPECT_TRUE(bridge_host()->GetInputMethod());
}

// A simpler test harness for testing initialization flows.
class BridgedNativeWidgetInitTest : public BridgedNativeWidgetTestBase {
 public:
  BridgedNativeWidgetInitTest()
      : BridgedNativeWidgetTestBase(SkipInitialization()) {}

  // Prepares a new |window_| and |widget_| for a call to PerformInit().
  void CreateNewWidgetToInit() {
    widget_.reset(new Widget);
    native_widget_mac_ = new MockNativeWidgetMac(widget_.get());
    init_params_.native_widget = native_widget_mac_;
  }

  void PerformInit() {
    widget_->Init(init_params_);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(BridgedNativeWidgetInitTest);
};

// Test that BridgedNativeWidgetImpl remains sane if Init() is never called.
TEST_F(BridgedNativeWidgetInitTest, InitNotCalled) {
  // Don't use a Widget* as the delegate. ~Widget() checks for Widget::
  // |native_widget_destroyed_| being set to true. That can only happen with a
  // non-null WidgetDelegate, which is only set in Widget::Init(). Then, since
  // neither Widget nor NativeWidget take ownership, use a unique_ptr.
  std::unique_ptr<MockNativeWidgetMac> native_widget(
      new MockNativeWidgetMac(nullptr));
  native_widget_mac_ = native_widget.get();
  EXPECT_FALSE(bridge());
  EXPECT_FALSE(bridge_host()->GetLocalNSWindow());
}

// Tests the shadow type given in InitParams.
TEST_F(BridgedNativeWidgetInitTest, ShadowType) {
  // Verify Widget::InitParam defaults and arguments added from SetUp().
  EXPECT_EQ(Widget::InitParams::TYPE_WINDOW_FRAMELESS, init_params_.type);
  EXPECT_EQ(Widget::InitParams::OPAQUE_WINDOW, init_params_.opacity);
  EXPECT_EQ(Widget::InitParams::SHADOW_TYPE_DEFAULT, init_params_.shadow_type);

  CreateNewWidgetToInit();
  EXPECT_FALSE(
      [bridge_window() hasShadow]);  // Default for NSBorderlessWindowMask.
  PerformInit();

  // Borderless is 0, so isn't really a mask. Check that nothing is set.
  EXPECT_EQ(NSBorderlessWindowMask, [bridge_window() styleMask]);
  EXPECT_TRUE(
      [bridge_window() hasShadow]);  // SHADOW_TYPE_DEFAULT means a shadow.

  CreateNewWidgetToInit();
  init_params_.shadow_type = Widget::InitParams::SHADOW_TYPE_NONE;
  PerformInit();
  EXPECT_FALSE([bridge_window() hasShadow]);  // Preserves lack of shadow.

  // Default for Widget::InitParams::TYPE_WINDOW.
  CreateNewWidgetToInit();
  PerformInit();
  EXPECT_FALSE(
      [bridge_window() hasShadow]);  // SHADOW_TYPE_NONE removes shadow.

  init_params_.shadow_type = Widget::InitParams::SHADOW_TYPE_DEFAULT;
  CreateNewWidgetToInit();
  PerformInit();
  EXPECT_TRUE([bridge_window() hasShadow]);  // Preserves shadow.

  widget_.reset();
}

// Ensure a nil NSTextInputContext is returned when the ui::TextInputClient is
// not editable, a password field, or unset.
TEST_F(BridgedNativeWidgetTest, InputContext) {
  const base::string16 test_string = base::ASCIIToUTF16("test_str");
  InstallTextField(test_string, ui::TEXT_INPUT_TYPE_PASSWORD);
  EXPECT_FALSE([ns_view_ inputContext]);
  InstallTextField(test_string, ui::TEXT_INPUT_TYPE_TEXT);
  EXPECT_TRUE([ns_view_ inputContext]);
  [ns_view_ setTextInputClient:nil];
  EXPECT_FALSE([ns_view_ inputContext]);
  InstallTextField(test_string, ui::TEXT_INPUT_TYPE_NONE);
  EXPECT_FALSE([ns_view_ inputContext]);
}

// Test getting complete string using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_GetCompleteString) {
  const std::string test_string = "foo bar baz";
  InstallTextField(test_string);

  NSRange range = NSMakeRange(0, test_string.size());
  NSRange actual_range;

  NSAttributedString* actual_text =
      [ns_view_ attributedSubstringForProposedRange:range
                                        actualRange:&actual_range];

  NSRange expected_range;
  NSAttributedString* expected_text =
      [dummy_text_view_ attributedSubstringForProposedRange:range
                                                actualRange:&expected_range];

  EXPECT_NSEQ_3(@"foo bar baz", [expected_text string], [actual_text string]);
  EXPECT_EQ_RANGE_3(range, expected_range, actual_range);
}

// Test getting middle substring using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_GetMiddleSubstring) {
  const std::string test_string = "foo bar baz";
  InstallTextField(test_string);

  NSRange range = NSMakeRange(4, 3);
  NSRange actual_range;
  NSAttributedString* actual_text =
      [ns_view_ attributedSubstringForProposedRange:range
                                        actualRange:&actual_range];

  NSRange expected_range;
  NSAttributedString* expected_text =
      [dummy_text_view_ attributedSubstringForProposedRange:range
                                                actualRange:&expected_range];

  EXPECT_NSEQ_3(@"bar", [expected_text string], [actual_text string]);
  EXPECT_EQ_RANGE_3(range, expected_range, actual_range);
}

// Test getting ending substring using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_GetEndingSubstring) {
  const std::string test_string = "foo bar baz";
  InstallTextField(test_string);

  NSRange range = NSMakeRange(8, 100);
  NSRange actual_range;
  NSAttributedString* actual_text =
      [ns_view_ attributedSubstringForProposedRange:range
                                        actualRange:&actual_range];
  NSRange expected_range;
  NSAttributedString* expected_text =
      [dummy_text_view_ attributedSubstringForProposedRange:range
                                                actualRange:&expected_range];

  EXPECT_NSEQ_3(@"baz", [expected_text string], [actual_text string]);
  EXPECT_EQ_RANGE_3(NSMakeRange(8, 3), expected_range, actual_range);
}

// Test getting empty substring using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_GetEmptySubstring) {
  const std::string test_string = "foo bar baz";
  InstallTextField(test_string);

  // Try with EmptyRange(). This behaves specially and should return the
  // complete string and the corresponding text range.
  NSRange range = EmptyRange();
  NSRange actual_range = EmptyRange();
  NSRange expected_range = EmptyRange();
  NSAttributedString* actual_text =
      [ns_view_ attributedSubstringForProposedRange:range
                                        actualRange:&actual_range];
  NSAttributedString* expected_text =
      [dummy_text_view_ attributedSubstringForProposedRange:range
                                                actualRange:&expected_range];
  EXPECT_NSEQ_3(@"foo bar baz", [expected_text string], [actual_text string]);
  EXPECT_EQ_RANGE_3(NSMakeRange(0, test_string.length()), expected_range,
                    actual_range);

  // Try with a valid empty range.
  range = NSMakeRange(2, 0);
  actual_range = EmptyRange();
  expected_range = EmptyRange();
  actual_text = [ns_view_ attributedSubstringForProposedRange:range
                                                  actualRange:&actual_range];
  expected_text =
      [dummy_text_view_ attributedSubstringForProposedRange:range
                                                actualRange:&expected_range];
  EXPECT_NSEQ_3(nil, [expected_text string], [actual_text string]);
  EXPECT_EQ_RANGE_3(range, expected_range, actual_range);

  // Try with an out of bounds empty range.
  range = NSMakeRange(20, 0);
  actual_range = EmptyRange();
  expected_range = EmptyRange();
  actual_text = [ns_view_ attributedSubstringForProposedRange:range
                                                  actualRange:&actual_range];
  expected_text =
      [dummy_text_view_ attributedSubstringForProposedRange:range
                                                actualRange:&expected_range];

  EXPECT_NSEQ_3(nil, [expected_text string], [actual_text string]);
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), expected_range, actual_range);
}

// Test inserting text using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_InsertText) {
  const std::string test_string = "foo";
  InstallTextField(test_string);

  [ns_view_ insertText:SysUTF8ToNSString(test_string)
      replacementRange:EmptyRange()];
  [dummy_text_view_ insertText:SysUTF8ToNSString(test_string)
              replacementRange:EmptyRange()];

  EXPECT_NSEQ_3(@"foofoo", GetExpectedText(), GetActualText());
}

// Test replacing text using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_ReplaceText) {
  const std::string test_string = "foo bar";
  InstallTextField(test_string);

  [ns_view_ insertText:@"baz" replacementRange:NSMakeRange(4, 3)];
  [dummy_text_view_ insertText:@"baz" replacementRange:NSMakeRange(4, 3)];

  EXPECT_NSEQ_3(@"foo baz", GetExpectedText(), GetActualText());
}

// Test IME composition using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_Compose) {
  const std::string test_string = "foo ";
  InstallTextField(test_string);

  EXPECT_EQ_3(NO, [dummy_text_view_ hasMarkedText], [ns_view_ hasMarkedText]);

  // As per NSTextInputClient documentation, markedRange should return
  // {NSNotFound, 0} iff hasMarked returns false. However, NSTextView returns
  // {text_length, text_length} for this case. We maintain consistency with the
  // documentation, hence the EXPECT_FALSE check.
  EXPECT_FALSE(
      NSEqualRanges([dummy_text_view_ markedRange], [ns_view_ markedRange]));
  EXPECT_EQ_RANGE(EmptyRange(), [ns_view_ markedRange]);

  // Start composition.
  NSString* compositionText = @"bar";
  NSUInteger compositionLength = [compositionText length];
  [ns_view_ setMarkedText:compositionText
            selectedRange:NSMakeRange(0, 2)
         replacementRange:EmptyRange()];
  [dummy_text_view_ setMarkedText:compositionText
                    selectedRange:NSMakeRange(0, 2)
                 replacementRange:EmptyRange()];
  EXPECT_EQ_3(YES, [dummy_text_view_ hasMarkedText], [ns_view_ hasMarkedText]);
  EXPECT_EQ_RANGE_3(NSMakeRange(test_string.size(), compositionLength),
                    [dummy_text_view_ markedRange], [ns_view_ markedRange]);
  EXPECT_EQ_RANGE_3(NSMakeRange(test_string.size(), 2),
                    GetExpectedSelectionRange(), GetActualSelectionRange());

  // Confirm composition.
  [ns_view_ unmarkText];
  [dummy_text_view_ unmarkText];

  EXPECT_EQ_3(NO, [dummy_text_view_ hasMarkedText], [ns_view_ hasMarkedText]);
  EXPECT_FALSE(
      NSEqualRanges([dummy_text_view_ markedRange], [ns_view_ markedRange]));
  EXPECT_EQ_RANGE(EmptyRange(), [ns_view_ markedRange]);
  EXPECT_NSEQ_3(@"foo bar", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange([GetActualText() length], 0),
                    GetExpectedSelectionRange(), GetActualSelectionRange());
}

// Test IME composition for accented characters.
TEST_F(BridgedNativeWidgetTest, TextInput_AccentedCharacter) {
  InstallTextField("abc");

  // Simulate action messages generated when the key 'a' is pressed repeatedly
  // and leads to the showing of an IME candidate window. To simulate an event,
  // set the private keyDownEvent field on the BridgedContentView.

  // First an insertText: message with key 'a' is generated.
  SetKeyDownEvent(cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_A, 0));
  [ns_view_ insertText:@"a" replacementRange:EmptyRange()];
  [dummy_text_view_ insertText:@"a" replacementRange:EmptyRange()];
  SetKeyDownEvent(nil);
  EXPECT_EQ_3(NO, [dummy_text_view_ hasMarkedText], [ns_view_ hasMarkedText]);
  EXPECT_NSEQ_3(@"abca", GetExpectedText(), GetActualText());

  // Next the IME popup appears. On selecting the accented character using arrow
  // keys, setMarkedText action message is generated which replaces the earlier
  // inserted 'a'.
  SetKeyDownEvent(cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_RIGHT, 0));
  [ns_view_ setMarkedText:@"à"
            selectedRange:NSMakeRange(0, 1)
         replacementRange:NSMakeRange(3, 1)];
  [dummy_text_view_ setMarkedText:@"à"
                    selectedRange:NSMakeRange(0, 1)
                 replacementRange:NSMakeRange(3, 1)];
  SetKeyDownEvent(nil);
  EXPECT_EQ_3(YES, [dummy_text_view_ hasMarkedText], [ns_view_ hasMarkedText]);
  EXPECT_EQ_RANGE_3(NSMakeRange(3, 1), [dummy_text_view_ markedRange],
                    [ns_view_ markedRange]);
  EXPECT_EQ_RANGE_3(NSMakeRange(3, 1), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
  EXPECT_NSEQ_3(@"abcà", GetExpectedText(), GetActualText());

  // On pressing enter, the marked text is confirmed.
  SetKeyDownEvent(cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_RETURN, 0));
  [ns_view_ insertText:@"à" replacementRange:EmptyRange()];
  [dummy_text_view_ insertText:@"à" replacementRange:EmptyRange()];
  SetKeyDownEvent(nil);
  EXPECT_EQ_3(NO, [dummy_text_view_ hasMarkedText], [ns_view_ hasMarkedText]);
  EXPECT_EQ_RANGE_3(NSMakeRange(4, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
  EXPECT_NSEQ_3(@"abcà", GetExpectedText(), GetActualText());
}

// Test moving the caret left and right using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_MoveLeftRight) {
  InstallTextField("foo");
  EXPECT_EQ_RANGE_3(NSMakeRange(3, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move right not allowed, out of range.
  PerformCommand(@selector(moveRight:));
  EXPECT_EQ_RANGE_3(NSMakeRange(3, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move left.
  PerformCommand(@selector(moveLeft:));
  EXPECT_EQ_RANGE_3(NSMakeRange(2, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move right.
  PerformCommand(@selector(moveRight:));
  EXPECT_EQ_RANGE_3(NSMakeRange(3, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
}

// Test backward delete using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_DeleteBackward) {
  InstallTextField("a");
  EXPECT_EQ_RANGE_3(NSMakeRange(1, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Delete one character.
  PerformCommand(@selector(deleteBackward:));
  EXPECT_NSEQ_3(nil, GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Verify that deletion did not modify the kill buffer.
  PerformCommand(@selector(yank:));
  EXPECT_NSEQ_3(nil, GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Try to delete again on an empty string.
  PerformCommand(@selector(deleteBackward:));
  EXPECT_NSEQ_3(nil, GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
}

// Test forward delete using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_DeleteForward) {
  InstallTextField("a");
  EXPECT_EQ_RANGE_3(NSMakeRange(1, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // At the end of the string, can't delete forward.
  PerformCommand(@selector(deleteForward:));
  EXPECT_NSEQ_3(@"a", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(1, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Should succeed after moving left first.
  PerformCommand(@selector(moveLeft:));
  PerformCommand(@selector(deleteForward:));
  EXPECT_NSEQ_3(nil, GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Verify that deletion did not modify the kill buffer.
  PerformCommand(@selector(yank:));
  EXPECT_NSEQ_3(nil, GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
}

// Test forward word deletion using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_DeleteWordForward) {
  InstallTextField("foo bar baz");
  EXPECT_EQ_RANGE_3(NSMakeRange(11, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Caret is at the end of the line. Verify no deletion takes place.
  PerformCommand(@selector(deleteWordForward:));
  EXPECT_NSEQ_3(@"foo bar baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(11, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move the caret as- "foo b|ar baz".
  SetSelectionRange(NSMakeRange(5, 0));
  PerformCommand(@selector(deleteWordForward:));
  // Verify state is "foo b| baz"
  EXPECT_NSEQ_3(@"foo b baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(5, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Make a selection as- "|fo|o b baz".
  SetSelectionRange(NSMakeRange(0, 2));
  PerformCommand(@selector(deleteWordForward:));
  // Verify only the selection is deleted and state is "|o b baz".
  EXPECT_NSEQ_3(@"o b baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Verify that deletion did not modify the kill buffer.
  PerformCommand(@selector(yank:));
  EXPECT_NSEQ_3(@"o b baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
}

// Test backward word deletion using text input protocol.
TEST_F(BridgedNativeWidgetTest, TextInput_DeleteWordBackward) {
  InstallTextField("foo bar baz");
  EXPECT_EQ_RANGE_3(NSMakeRange(11, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move the caret to the beginning of the line.
  SetSelectionRange(NSMakeRange(0, 0));
  // Verify no deletion takes place.
  PerformCommand(@selector(deleteWordBackward:));
  EXPECT_NSEQ_3(@"foo bar baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(0, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Move the caret as- "foo ba|r baz".
  SetSelectionRange(NSMakeRange(6, 0));
  PerformCommand(@selector(deleteWordBackward:));
  // Verify state is "foo |r baz".
  EXPECT_NSEQ_3(@"foo r baz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(4, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Make a selection as- "f|oo r b|az".
  SetSelectionRange(NSMakeRange(1, 6));
  PerformCommand(@selector(deleteWordBackward:));
  // Verify only the selection is deleted and state is "f|az"
  EXPECT_NSEQ_3(@"faz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(1, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());

  // Verify that deletion did not modify the kill buffer.
  PerformCommand(@selector(yank:));
  EXPECT_NSEQ_3(@"faz", GetExpectedText(), GetActualText());
  EXPECT_EQ_RANGE_3(NSMakeRange(1, 0), GetExpectedSelectionRange(),
                    GetActualSelectionRange());
}

// Test deleting to beginning/end of line/paragraph using text input protocol.

TEST_F(BridgedNativeWidgetTest, TextInput_DeleteToBeginningOfLine) {
  TestDeleteBeginning(@selector(deleteToBeginningOfLine:));
}

TEST_F(BridgedNativeWidgetTest, TextInput_DeleteToEndOfLine) {
  TestDeleteEnd(@selector(deleteToEndOfLine:));
}

TEST_F(BridgedNativeWidgetTest, TextInput_DeleteToBeginningOfParagraph) {
  TestDeleteBeginning(@selector(deleteToBeginningOfParagraph:));
}

TEST_F(BridgedNativeWidgetTest, TextInput_DeleteToEndOfParagraph) {
  TestDeleteEnd(@selector(deleteToEndOfParagraph:));
}

// Test move commands against expectations set by |dummy_text_view_|.
TEST_F(BridgedNativeWidgetTest, TextInput_MoveEditingCommands) {
  TestEditingCommands(kMoveActions);
}

// Test move and select commands against expectations set by |dummy_text_view_|.
TEST_F(BridgedNativeWidgetTest, TextInput_MoveAndSelectEditingCommands) {
  TestEditingCommands(kSelectActions);
}

// Test delete commands against expectations set by |dummy_text_view_|.
TEST_F(BridgedNativeWidgetTest, TextInput_DeleteCommands) {
  TestEditingCommands(kDeleteActions);
}

// Test that we don't crash during an action message even if the TextInputClient
// is nil. Regression test for crbug.com/615745.
TEST_F(BridgedNativeWidgetTest, NilTextInputClient) {
  [ns_view_ setTextInputClient:nil];
  NSMutableArray* selectors = [NSMutableArray array];
  [selectors addObjectsFromArray:kMoveActions];
  [selectors addObjectsFromArray:kSelectActions];
  [selectors addObjectsFromArray:kDeleteActions];
  [selectors addObjectsFromArray:kMiscActions];

  for (NSString* selector in selectors)
    [ns_view_ doCommandBySelector:NSSelectorFromString(selector)];
}

// Test transpose command against expectations set by |dummy_text_view_|.
TEST_F(BridgedNativeWidgetTest, TextInput_Transpose) {
  TestEditingCommands(@[ @"transpose:" ]);
}

// Test firstRectForCharacterRange:actualRange for cases where query range is
// empty or outside composition range.
TEST_F(BridgedNativeWidgetTest, TextInput_FirstRectForCharacterRange_Caret) {
  InstallTextField("");
  ui::TextInputClient* client = [ns_view_ textInputClient];

  // No composition. Ensure bounds and range corresponding to the current caret
  // position are returned.
  // Initially selection range will be [0, 0].
  NSRange caret_range = NSMakeRange(0, 0);
  NSRange query_range = NSMakeRange(1, 1);
  NSRange actual_range;
  NSRect rect = [ns_view_ firstRectForCharacterRange:query_range
                                         actualRange:&actual_range];
  EXPECT_EQ(GetCaretBounds(client), gfx::ScreenRectFromNSRect(rect));
  EXPECT_EQ_RANGE(caret_range, actual_range);

  // Set composition with caret before second character ('e').
  const base::string16 test_string = base::ASCIIToUTF16("test_str");
  const size_t kTextLength = 8;
  SetCompositionText(client, test_string, 1, &caret_range);

  // Test bounds returned for empty ranges within composition range. As per
  // Apple's documentation for firstRectForCharacterRange:actualRange:, for an
  // empty query range, the returned rectangle should coincide with the
  // insertion point and have zero width. However in our implementation, if the
  // empty query range lies within the composition range, we return a zero width
  // rectangle corresponding to the query range location.

  // Test bounds returned for empty range before second character ('e') are same
  // as caret bounds when placed before second character.
  query_range = NSMakeRange(1, 0);
  rect = [ns_view_ firstRectForCharacterRange:query_range
                                  actualRange:&actual_range];
  EXPECT_EQ(GetCaretBoundsForPosition(client, test_string, 1, &caret_range),
            gfx::ScreenRectFromNSRect(rect));
  EXPECT_EQ_RANGE(query_range, actual_range);

  // Test bounds returned for empty range after the composition text are same as
  // caret bounds when placed after the composition text.
  query_range = NSMakeRange(kTextLength, 0);
  rect = [ns_view_ firstRectForCharacterRange:query_range
                                  actualRange:&actual_range];
  EXPECT_NE(GetCaretBoundsForPosition(client, test_string, 1, &caret_range),
            gfx::ScreenRectFromNSRect(rect));
  EXPECT_EQ(
      GetCaretBoundsForPosition(client, test_string, kTextLength, &caret_range),
      gfx::ScreenRectFromNSRect(rect));
  EXPECT_EQ_RANGE(query_range, actual_range);

  // Query outside composition range. Ensure bounds and range corresponding to
  // the current caret position are returned.
  query_range = NSMakeRange(kTextLength + 1, 0);
  rect = [ns_view_ firstRectForCharacterRange:query_range
                                  actualRange:&actual_range];
  EXPECT_EQ(GetCaretBounds(client), gfx::ScreenRectFromNSRect(rect));
  EXPECT_EQ_RANGE(caret_range, actual_range);

  // Make sure not crashing by passing null pointer instead of actualRange.
  rect = [ns_view_ firstRectForCharacterRange:query_range actualRange:nullptr];
}

// Test firstRectForCharacterRange:actualRange for non-empty query ranges within
// the composition range.
TEST_F(BridgedNativeWidgetTest, TextInput_FirstRectForCharacterRange) {
  InstallTextField("");
  ui::TextInputClient* client = [ns_view_ textInputClient];

  const base::string16 test_string = base::ASCIIToUTF16("test_str");
  const size_t kTextLength = 8;
  SetCompositionText(client, test_string, 1, nullptr);

  // Query bounds for the whole composition string.
  NSRange query_range = NSMakeRange(0, kTextLength);
  NSRange actual_range;
  NSRect rect = [ns_view_ firstRectForCharacterRange:query_range
                                         actualRange:&actual_range];
  EXPECT_EQ(GetExpectedBoundsForRange(client, test_string, query_range),
            gfx::ScreenRectFromNSRect(rect));
  EXPECT_EQ_RANGE(query_range, actual_range);

  // Query bounds for the substring "est_".
  query_range = NSMakeRange(1, 4);
  rect = [ns_view_ firstRectForCharacterRange:query_range
                                  actualRange:&actual_range];
  EXPECT_EQ(GetExpectedBoundsForRange(client, test_string, query_range),
            gfx::ScreenRectFromNSRect(rect));
  EXPECT_EQ_RANGE(query_range, actual_range);
}

// Test simulated codepaths for IMEs that do not always "mark" text. E.g.
// phonetic languages such as Korean and Vietnamese.
TEST_F(BridgedNativeWidgetTest, TextInput_SimulatePhoneticIme) {
  Textfield* textfield = InstallTextField("");
  EXPECT_TRUE([ns_view_ textInputClient]);

  object_setClass(ns_view_, [InterpretKeyEventMockedBridgedContentView class]);

  // Sequence of calls (and corresponding keyDown events) obtained via tracing
  // with 2-Set Korean IME and pressing q, o, then Enter on the keyboard.
  NSEvent* q_in_ime = cocoa_test_event_utils::KeyEventWithKeyCode(
      12, [@"ㅂ" characterAtIndex:0], NSKeyDown, 0);
  InterpretKeyEventsCallback handle_q_in_ime = base::BindRepeating([](id view) {
    [view insertText:@"ㅂ" replacementRange:NSMakeRange(NSNotFound, 0)];
  });

  NSEvent* o_in_ime = cocoa_test_event_utils::KeyEventWithKeyCode(
      31, [@"ㅐ" characterAtIndex:0], NSKeyDown, 0);
  InterpretKeyEventsCallback handle_o_in_ime = base::BindRepeating([](id view) {
    [view insertText:@"배" replacementRange:NSMakeRange(0, 1)];
  });

  NSEvent* return_in_ime = cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_RETURN, 0);
  InterpretKeyEventsCallback handle_return_in_ime =
      base::BindRepeating([](id view) {
        // When confirming the composition, AppKit repeats itself.
        [view insertText:@"배" replacementRange:NSMakeRange(0, 1)];
        [view insertText:@"배" replacementRange:NSMakeRange(0, 1)];
        // Note: there is no insertText:@"\r", which we would normally see when
        // not in an IME context for VKEY_RETURN.
      });

  // Add a hook for the KeyEvent being received by the TextfieldController. E.g.
  // this is where the Omnibox would start to search when Return is pressed.
  bool saw_vkey_return = false;
  SetHandleKeyEventCallback(base::BindRepeating(
      [](bool* saw_return, Textfield* textfield, const ui::KeyEvent& event) {
        if (event.key_code() == ui::VKEY_RETURN) {
          EXPECT_FALSE(*saw_return);
          *saw_return = true;
          EXPECT_EQ(base::SysNSStringToUTF16(@"배"), textfield->text());
        }
        return false;
      },
      &saw_vkey_return));

  EXPECT_EQ(base::UTF8ToUTF16(""), textfield->text());

  g_fake_interpret_key_events = &handle_q_in_ime;
  [ns_view_ keyDown:q_in_ime];
  EXPECT_EQ(base::SysNSStringToUTF16(@"ㅂ"), textfield->text());
  EXPECT_FALSE(saw_vkey_return);

  g_fake_interpret_key_events = &handle_o_in_ime;
  [ns_view_ keyDown:o_in_ime];
  EXPECT_EQ(base::SysNSStringToUTF16(@"배"), textfield->text());
  EXPECT_FALSE(saw_vkey_return);

  // Note the "Enter" should not replace the replacement range, even though a
  // replacement range was set.
  g_fake_interpret_key_events = &handle_return_in_ime;
  [ns_view_ keyDown:return_in_ime];
  EXPECT_EQ(base::SysNSStringToUTF16(@"배"), textfield->text());

  // VKEY_RETURN should be seen by via the unhandled key event handler (but not
  // via -insertText:.
  EXPECT_TRUE(saw_vkey_return);

  g_fake_interpret_key_events = nullptr;
}

// Simulate 'a', Enter in Hiragana. This should just insert "あ", suppressing
// accelerators.
TEST_F(BridgedNativeWidgetTest, TextInput_NoAcceleratorEnterComposition) {
  Textfield* textfield = InstallTextField("");
  EXPECT_TRUE([ns_view_ textInputClient]);

  EnterAcceleratorView* enter_view = new EnterAcceleratorView();
  textfield->parent()->AddChildView(enter_view);

  // Sequence of calls (and corresponding keyDown events) obtained via tracing
  // with Hiragana IME and pressing 'a', then Enter on the keyboard.
  NSEvent* a_in_ime = cocoa_test_event_utils::KeyEventWithKeyCode(
      0, [@"a" characterAtIndex:0], NSKeyDown, 0);
  InterpretKeyEventsCallback handle_a_in_ime = base::BindRepeating([](id view) {
    // TODO(crbug/612675): |text| should be an NSAttributedString.
    [view setMarkedText:@"あ"
           selectedRange:NSMakeRange(1, 0)
        replacementRange:NSMakeRange(NSNotFound, 0)];
  });

  NSEvent* return_event = cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_RETURN, 0);
  InterpretKeyEventsCallback handle_return_in_ime =
      base::BindRepeating([](id view) {
        [view insertText:@"あ" replacementRange:NSMakeRange(NSNotFound, 0)];
      });

  EXPECT_EQ(base::UTF8ToUTF16(""), textfield->text());
  EXPECT_EQ(0, enter_view->count());

  object_setClass(ns_view_, [InterpretKeyEventMockedBridgedContentView class]);
  g_fake_interpret_key_events = &handle_a_in_ime;
  [ns_view_ keyDown:a_in_ime];
  EXPECT_EQ(base::SysNSStringToUTF16(@"あ"), textfield->text());
  EXPECT_EQ(0, enter_view->count());

  g_fake_interpret_key_events = &handle_return_in_ime;
  [ns_view_ keyDown:return_event];
  EXPECT_EQ(base::SysNSStringToUTF16(@"あ"), textfield->text());
  EXPECT_EQ(0, enter_view->count());  // Not seen as an accelerator.

  // IME Window is dismissed here and there is no marked text, so remove the
  // swizzler.
  object_setClass(ns_view_, [BridgedContentView class]);

  [ns_view_ keyDown:return_event];  // Sanity check: send Enter again.
  EXPECT_EQ(base::SysNSStringToUTF16(@"あ"), textfield->text());  // No change.
  EXPECT_EQ(1, enter_view->count());  // Now we see the accelerator.
}

// Simulate 'a', Tab, Enter, Enter in Hiragana. This should just insert "a",
// suppressing accelerators.
TEST_F(BridgedNativeWidgetTest, TextInput_NoAcceleratorTabEnterComposition) {
  Textfield* textfield = InstallTextField("");
  EXPECT_TRUE([ns_view_ textInputClient]);

  EnterAcceleratorView* enter_view = new EnterAcceleratorView();
  textfield->parent()->AddChildView(enter_view);

  // Sequence of calls (and corresponding keyDown events) obtained via tracing
  // with Hiragana IME and pressing 'a', Tab, then Enter on the keyboard.
  NSEvent* a_in_ime = cocoa_test_event_utils::KeyEventWithKeyCode(
      0, [@"a" characterAtIndex:0], NSKeyDown, 0);
  InterpretKeyEventsCallback handle_a_in_ime = base::BindRepeating([](id view) {
    // TODO(crbug/612675): |text| should have an underline.
    [view setMarkedText:@"あ"
           selectedRange:NSMakeRange(1, 0)
        replacementRange:NSMakeRange(NSNotFound, 0)];
  });

  NSEvent* tab_in_ime = cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_TAB, 0);
  InterpretKeyEventsCallback handle_tab_in_ime =
      base::BindRepeating([](id view) {
        // TODO(crbug/612675): |text| should be an NSAttributedString (now with
        // a different underline color).
        [view setMarkedText:@"a"
               selectedRange:NSMakeRange(0, 1)
            replacementRange:NSMakeRange(NSNotFound, 0)];
      });

  NSEvent* return_event = cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_RETURN, 0);
  InterpretKeyEventsCallback handle_first_return_in_ime =
      base::BindRepeating([](id view) {
        // Do *nothing*. Enter does not confirm nor change the composition, it
        // just dismisses the IME window, leaving the text marked.
      });
  InterpretKeyEventsCallback handle_second_return_in_ime =
      base::BindRepeating([](id view) {
        // The second return will confirm the composition.
        [view insertText:@"a" replacementRange:NSMakeRange(NSNotFound, 0)];
      });

  EXPECT_EQ(base::UTF8ToUTF16(""), textfield->text());
  EXPECT_EQ(0, enter_view->count());

  object_setClass(ns_view_, [InterpretKeyEventMockedBridgedContentView class]);
  g_fake_interpret_key_events = &handle_a_in_ime;
  [ns_view_ keyDown:a_in_ime];
  EXPECT_EQ(base::SysNSStringToUTF16(@"あ"), textfield->text());
  EXPECT_EQ(0, enter_view->count());

  g_fake_interpret_key_events = &handle_tab_in_ime;
  [ns_view_ keyDown:tab_in_ime];
  // Tab will switch to a Romanji (Latin) character.
  EXPECT_EQ(base::SysNSStringToUTF16(@"a"), textfield->text());
  EXPECT_EQ(0, enter_view->count());

  g_fake_interpret_key_events = &handle_first_return_in_ime;
  [ns_view_ keyDown:return_event];
  // Enter just dismisses the IME window. The composition is still active.
  EXPECT_EQ(base::SysNSStringToUTF16(@"a"), textfield->text());
  EXPECT_EQ(0, enter_view->count());  // Not seen as an accelerator.

  g_fake_interpret_key_events = &handle_second_return_in_ime;
  [ns_view_ keyDown:return_event];
  // Enter now confirms the composition (unmarks text). Note there is still no
  // IME window visible but, since there is marked text, IME is still active.
  EXPECT_EQ(base::SysNSStringToUTF16(@"a"), textfield->text());
  EXPECT_EQ(0, enter_view->count());  // Not seen as an accelerator.

  // No marked text, no IME window. We could remove the swizzler here, but
  // that is equivalent to the "do nothing" case, so set than handler again.
  g_fake_interpret_key_events = &handle_first_return_in_ime;
  [ns_view_ keyDown:return_event];  // Send Enter a _third_ time.
  EXPECT_EQ(base::SysNSStringToUTF16(@"a"), textfield->text());  // No change.
  EXPECT_EQ(1, enter_view->count());  // Now we see the accelerator.
}

// Test a codepath that could hypothetically cause [NSApp updateWindows] to be
// called recursively due to IME dismissal during teardown triggering a focus
// change. Twice.
TEST_F(BridgedNativeWidgetTest, TextInput_RecursiveUpdateWindows) {
  Textfield* textfield = InstallTextField("");
  EXPECT_TRUE([ns_view_ textInputClient]);

  object_setClass(ns_view_, [InterpretKeyEventMockedBridgedContentView class]);
  base::mac::ScopedObjCClassSwizzler update_windows_swizzler(
      [NSApplication class], [UpdateWindowsDonorForNSApp class],
      @selector(updateWindows));
  base::mac::ScopedObjCClassSwizzler current_input_context_swizzler(
      [NSTextInputContext class],
      [CurrentInputContextDonorForNSTextInputContext class],
      @selector(currentInputContext));

  int vkey_return_count = 0;

  // Everything happens with this one event.
  NSEvent* return_with_fake_ime = cocoa_test_event_utils::SynthesizeKeyEvent(
      widget_->GetNativeWindow(), true, ui::VKEY_RETURN, 0);

  InterpretKeyEventsCallback generate_return_and_fake_ime = base::BindRepeating(
      [](int* saw_return_count, id view) {
        EXPECT_EQ(0, *saw_return_count);
        // First generate the return to simulate an input context change.
        [view insertText:@"\r" replacementRange:NSMakeRange(NSNotFound, 0)];

        EXPECT_EQ(1, *saw_return_count);
      },
      &vkey_return_count);

  bool saw_update_windows = false;
  base::RepeatingClosure update_windows_closure = base::BindRepeating(
      [](bool* saw_update_windows, BridgedContentView* view,
         Textfield* textfield) {
        // Ensure updateWindows is not invoked recursively.
        EXPECT_FALSE(*saw_update_windows);
        *saw_update_windows = true;

        // Inside updateWindows, assume the IME got dismissed and wants to
        // insert its last bit of text for the old input context.
        [view insertText:@"배" replacementRange:NSMakeRange(0, 1)];

        // This is triggered by the setTextInputClient:nullptr in
        // SetHandleKeyEventCallback(), so -inputContext should also be nil.
        EXPECT_FALSE([view inputContext]);

        // Ensure we can't recursively call updateWindows. A TextInputClient
        // reacting to InsertChar could theoretically do this, but toolkit-views
        // DCHECKs if there is recursive event dispatch, so call
        // setTextInputClient directly.
        [view setTextInputClient:textfield];

        // Finally simulate what -[NSApp updateWindows] should _actually_ do,
        // which is to update the input context (from the first responder).
        g_fake_current_input_context = [view inputContext];

        // Now, the |textfield| set above should have been set again.
        EXPECT_TRUE(g_fake_current_input_context);
      },
      &saw_update_windows, ns_view_, textfield);

  SetHandleKeyEventCallback(base::BindRepeating(
      [](int* saw_return_count, BridgedContentView* view, Textfield* textfield,
         const ui::KeyEvent& event) {
        if (event.key_code() == ui::VKEY_RETURN) {
          *saw_return_count += 1;
          // Simulate Textfield::OnBlur() by clearing the input method.
          // Textfield needs to be in a Widget to do this normally.
          [view setTextInputClient:nullptr];
        }
        return false;
      },
      &vkey_return_count, ns_view_));

  // Starting text (just insert it).
  [ns_view_ insertText:@"ㅂ" replacementRange:NSMakeRange(NSNotFound, 0)];

  EXPECT_EQ(base::SysNSStringToUTF16(@"ㅂ"), textfield->text());

  g_fake_interpret_key_events = &generate_return_and_fake_ime;
  g_update_windows_closure = &update_windows_closure;
  g_fake_current_input_context = [ns_view_ inputContext];
  EXPECT_TRUE(g_fake_current_input_context);
  [ns_view_ keyDown:return_with_fake_ime];

  // We should see one VKEY_RETURNs and one updateWindows. In particular, note
  // that there is not a second VKEY_RETURN seen generated by keyDown: thinking
  // the event has been unhandled. This is because it was handled when the fake
  // IME sent \r.
  EXPECT_TRUE(saw_update_windows);
  EXPECT_EQ(1, vkey_return_count);

  // The text inserted during updateWindows should have been inserted, even
  // though we were trying to change the input context.
  EXPECT_EQ(base::SysNSStringToUTF16(@"배"), textfield->text());

  EXPECT_TRUE(g_fake_current_input_context);

  g_fake_current_input_context = nullptr;
  g_fake_interpret_key_events = nullptr;
  g_update_windows_closure = nullptr;
}

typedef BridgedNativeWidgetTestBase BridgedNativeWidgetSimulateFullscreenTest;

// Simulate the notifications that AppKit would send out if a fullscreen
// operation begins, and then fails and must abort. This notification sequence
// was determined by posting delayed tasks to toggle fullscreen state and then
// mashing Ctrl+Left/Right to keep OSX in a transition between Spaces to cause
// the fullscreen transition to fail.
TEST_F(BridgedNativeWidgetSimulateFullscreenTest, FailToEnterAndExit) {
  BridgedNativeWidgetTestWindow* window =
      base::mac::ObjCCastStrict<BridgedNativeWidgetTestWindow>(
          widget_->GetNativeWindow());
  [window setIgnoreToggleFullScreen:YES];
  widget_->Show();

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];

  EXPECT_FALSE(bridge()->target_fullscreen_state());

  // Simulate an initial toggleFullScreen: (user- or Widget-initiated).
  [center postNotificationName:NSWindowWillEnterFullScreenNotification
                        object:window];

  // On a failure, Cocoa starts by sending an unexpected *exit* fullscreen, and
  // BridgedNativeWidgetImpl will think it's just a delayed transition and try
  // to go back into fullscreen but get ignored by Cocoa.
  EXPECT_EQ(0, [window ignoredToggleFullScreenCount]);
  EXPECT_TRUE(bridge()->target_fullscreen_state());
  [center postNotificationName:NSWindowDidExitFullScreenNotification
                        object:window];
  EXPECT_EQ(1, [window ignoredToggleFullScreenCount]);
  EXPECT_FALSE(bridge()->target_fullscreen_state());

  // Cocoa follows up with a failure message sent to the NSWindowDelegate (there
  // is no equivalent notification for failure).
  ViewsNSWindowDelegate* window_delegate =
      base::mac::ObjCCast<ViewsNSWindowDelegate>([window delegate]);
  [window_delegate windowDidFailToEnterFullScreen:window];
  EXPECT_FALSE(bridge()->target_fullscreen_state());

  // Now perform a successful fullscreen operation.
  [center postNotificationName:NSWindowWillEnterFullScreenNotification
                        object:window];
  EXPECT_TRUE(bridge()->target_fullscreen_state());
  [center postNotificationName:NSWindowDidEnterFullScreenNotification
                        object:window];
  EXPECT_TRUE(bridge()->target_fullscreen_state());

  // And try to get out.
  [center postNotificationName:NSWindowWillExitFullScreenNotification
                        object:window];
  EXPECT_FALSE(bridge()->target_fullscreen_state());

  // On a failure, Cocoa sends a failure message, but then just dumps the window
  // out of fullscreen anyway (in that order).
  [window_delegate windowDidFailToExitFullScreen:window];
  EXPECT_FALSE(bridge()->target_fullscreen_state());
  [center postNotificationName:NSWindowDidExitFullScreenNotification
                        object:window];
  EXPECT_EQ(1, [window ignoredToggleFullScreenCount]);  // No change.
  EXPECT_FALSE(bridge()->target_fullscreen_state());

  widget_->CloseNow();
}

}  // namespace test
}  // namespace views
