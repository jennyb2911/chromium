// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_VIEWS_COCOA_BRIDGED_NATIVE_WIDGET_H_
#define UI_VIEWS_COCOA_BRIDGED_NATIVE_WIDGET_H_

#import <Cocoa/Cocoa.h>

#include <memory>
#include <vector>

#import "base/mac/scoped_nsobject.h"
#include "base/macros.h"
#include "components/viz/common/surfaces/parent_local_surface_id_allocator.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "ui/accelerated_widget_mac/ca_transaction_observer.h"
#include "ui/accelerated_widget_mac/display_ca_layer_tree.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/display/display_observer.h"
#import "ui/views/cocoa/bridged_native_widget_owner.h"
#include "ui/views/views_export.h"
#import "ui/views_bridge_mac/cocoa_mouse_capture_delegate.h"
#include "ui/views_bridge_mac/mojo/bridged_native_widget.mojom.h"

@class BridgedContentView;
@class ModalShowAnimationWithLayer;
@class NativeWidgetMacNSWindow;
@class ViewsNSWindowDelegate;

namespace views_bridge_mac {

namespace mojom {
class BridgedNativeWidgetHost;
}  // namespace mojom

class BridgedNativeWidgetHostHelper;
class CocoaMouseCapture;
class DragDropClient;

}  // namespace views_bridge_mac

namespace views {
namespace test {
class BridgedNativeWidgetTestApi;
}

class CocoaWindowMoveLoop;

using views_bridge_mac::mojom::BridgedNativeWidgetHost;
using views_bridge_mac::BridgedNativeWidgetHostHelper;
using views_bridge_mac::CocoaMouseCapture;
using views_bridge_mac::CocoaMouseCaptureDelegate;

// A bridge to an NSWindow managed by an instance of NativeWidgetMac or
// DesktopNativeWidgetMac. Serves as a helper class to bridge requests from the
// NativeWidgetMac to the Cocoa window. Behaves a bit like an aura::Window.
class VIEWS_EXPORT BridgedNativeWidgetImpl
    : public views_bridge_mac::mojom::BridgedNativeWidget,
      public display::DisplayObserver,
      public ui::CATransactionCoordinator::PreCommitObserver,
      public CocoaMouseCaptureDelegate,
      public BridgedNativeWidgetOwner {
 public:
  // Return the size that |window| will take for the given client area |size|,
  // based on its current style mask.
  static gfx::Size GetWindowSizeForClientSize(NSWindow* window,
                                              const gfx::Size& size);

  // Retrieves the bridge associated with the given NSWindow. Returns null if
  // the supplied handle has no associated Widget.
  static BridgedNativeWidgetImpl* GetFromNativeWindow(NSWindow* window);

  // Retrieve a BridgedNativeWidgetImpl* from its id.
  static BridgedNativeWidgetImpl* GetFromId(uint64_t bridged_native_widget_id);

  // Create an NSWindow for the specified parameters.
  static base::scoped_nsobject<NativeWidgetMacNSWindow> CreateNSWindow(
      views_bridge_mac::mojom::CreateWindowParams* params);

  // Creates one side of the bridge. |host| and |parent| must not be NULL.
  BridgedNativeWidgetImpl(uint64_t bridged_native_widget_id,
                          BridgedNativeWidgetHost* host,
                          BridgedNativeWidgetHostHelper* host_helper);
  ~BridgedNativeWidgetImpl() override;
  void BindRequest(views_bridge_mac::mojom::BridgedNativeWidgetRequest request);

  // Initialize the NSWindow by taking ownership of the specified object.
  // TODO(ccameron): When a BridgedNativeWidgetImpl is allocated across a
  // process boundary, it will not be possible to explicitly set an NSWindow in
  // this way.
  void SetWindow(base::scoped_nsobject<NativeWidgetMacNSWindow> window);

  // Set the parent NSView for the widget.
  // TODO(ccameron): Like SetWindow, this will need to pass a handle instead of
  // an NSView across processes.
  void SetParent(NSView* parent);

  // Changes the bounds of the window and the hosted layer if present. The
  // origin is a location in screen coordinates except for "child" windows,
  // which are positioned relative to their parent(). SetBounds() considers a
  // "child" window to be one initialized with InitParams specifying all of:
  // a |parent| NSWindow, the |child| attribute, and a |type| that
  // views::GetAuraWindowTypeForWidgetType does not consider a "popup" type.
  void SetBounds(const gfx::Rect& new_bounds);

  // Start moving the window, pinned to the mouse cursor, and monitor events.
  // Return true on mouse up or false on premature termination via EndMoveLoop()
  // or when window is destroyed during the drag.
  bool RunMoveLoop(const gfx::Vector2d& drag_offset);
  void EndMoveLoop();

  // Sets the cursor associated with the NSWindow. Retains |cursor|.
  void SetCursor(NSCursor* cursor);

  // Called internally by the NSWindowDelegate when the window is closing.
  void OnWindowWillClose();

  // Called by the NSWindowDelegate when a fullscreen operation begins. If
  // |target_fullscreen_state| is true, the target state is fullscreen.
  // Otherwise, a transition has begun to come out of fullscreen.
  void OnFullscreenTransitionStart(bool target_fullscreen_state);

  // Called when a fullscreen transition completes. If target_fullscreen_state()
  // does not match |actual_fullscreen_state|, a new transition will begin.
  void OnFullscreenTransitionComplete(bool actual_fullscreen_state);

  // Transition the window into or out of fullscreen. This will immediately
  // invert the value of target_fullscreen_state().
  void ToggleDesiredFullscreenState(bool async = false);

  // Called by the NSWindowDelegate when the size of the window changes.
  void OnSizeChanged();

  // Called once by the NSWindowDelegate when the position of the window has
  // changed.
  void OnPositionChanged();

  // Called by the NSWindowDelegate when the visibility of the window may have
  // changed. For example, due to a (de)miniaturize operation, or the window
  // being reordered in (or out of) the screen list.
  void OnVisibilityChanged();

  // Called by the NSWindowDelegate when the system control tint changes.
  void OnSystemControlTintChanged();

  // Called by the NSWindowDelegate on a scale factor or color space change.
  void OnBackingPropertiesChanged();

  // Called by the NSWindowDelegate when the window becomes or resigns key.
  void OnWindowKeyStatusChangedTo(bool is_key);

  // Called by the window show animation when it completes and wants to destroy
  // itself.
  void OnShowAnimationComplete();
  // Sort child NSViews according to their ranking in |rank|.
  void SortSubviews(std::map<NSView*, int> rank);

  BridgedContentView* ns_view() { return bridged_view_; }
  BridgedNativeWidgetHost* host() { return host_; }
  BridgedNativeWidgetHostHelper* host_helper() { return host_helper_; }
  NSWindow* ns_window();

  views_bridge_mac::DragDropClient* drag_drop_client();
  bool is_translucent_window() const { return is_translucent_window_; }

  // The parent widget specified in Widget::InitParams::parent. If non-null, the
  // parent will close children before the parent closes, and children will be
  // raised above their parent when window z-order changes.
  BridgedNativeWidgetOwner* parent() { return parent_; }
  const std::vector<BridgedNativeWidgetImpl*>& child_windows() {
    return child_windows_;
  }

  // Re-parent a |native_view| in this Widget to be a child of |new_parent|.
  // |native_view| must either be |ns_view()| or a descendant of |ns_view()|.
  // |native_view| is added as a subview of |new_parent| unless it is the
  // contentView of a top-level Widget. If |native_view| is |ns_view()|, |this|
  // also becomes a child window of |new_parent|'s NSWindow.
  void ReparentNativeView(NSView* native_view, NSView* new_parent);

  bool target_fullscreen_state() const { return target_fullscreen_state_; }
  bool window_visible() const { return window_visible_; }
  bool wants_to_be_visible() const { return wants_to_be_visible_; }
  bool in_fullscreen_transition() const { return in_fullscreen_transition_; }

  // Enables or disables all window animations.
  void SetAnimationEnabled(bool animate);

  // Whether to run a custom animation for the provided |transition|.
  bool ShouldRunCustomAnimationFor(
      views_bridge_mac::mojom::VisibilityTransition transition) const;

  // display::DisplayObserver:
  void OnDisplayMetricsChanged(const display::Display& display,
                               uint32_t metrics) override;

  // ui::CATransactionCoordinator::PreCommitObserver:
  bool ShouldWaitInPreCommit() override;
  base::TimeDelta PreCommitTimeout() override;

  // views_bridge_mac::mojom::BridgedNativeWidget:
  void CreateWindow(views_bridge_mac::mojom::CreateWindowParamsPtr params,
                    uint64_t parent_id) override;
  void InitWindow(views_bridge_mac::mojom::BridgedNativeWidgetInitParamsPtr
                      params) override;
  void InitCompositorView() override;
  void CreateContentView(const gfx::Rect& bounds) override;
  void DestroyContentView() override;
  void CloseWindow() override;
  void CloseWindowNow() override;
  void SetInitialBounds(const gfx::Rect& new_bounds,
                        const gfx::Size& minimum_content_size,
                        const gfx::Vector2d& parent_offset) override;
  void SetBounds(const gfx::Rect& new_bounds,
                 const gfx::Size& minimum_content_size) override;
  void SetVisibilityState(
      views_bridge_mac::mojom::WindowVisibilityState new_state) override;
  void SetTransitionsToAnimate(
      views_bridge_mac::mojom::VisibilityTransition transitions) override;
  void SetVisibleOnAllSpaces(bool always_visible) override;
  void SetFullscreen(bool fullscreen) override;
  void SetMiniaturized(bool miniaturized) override;
  void SetSizeConstraints(const gfx::Size& min_size,
                          const gfx::Size& max_size,
                          bool is_resizable,
                          bool is_maximizable) override;
  void SetOpacity(float opacity) override;
  void SetContentAspectRatio(const gfx::SizeF& aspect_ratio) override;
  void SetCALayerParams(const gfx::CALayerParams& ca_layer_params) override;
  void SetWindowTitle(const base::string16& title) override;
  void MakeFirstResponder() override;
  void ClearTouchBar() override;
  void UpdateTooltip() override;
  void AcquireCapture() override;
  void ReleaseCapture() override;

  // TODO(ccameron): This method exists temporarily as we move all direct access
  // of TextInputClient out of BridgedContentView.
  void SetTextInputClient(ui::TextInputClient* text_input_client);

  // Compute the window and content size, and forward them to |host_|. This will
  // update widget and compositor size.
  void UpdateWindowGeometry();

 private:
  friend class test::BridgedNativeWidgetTestApi;

  // Closes all child windows. BridgedNativeWidgetImpl children will be
  // destroyed.
  void RemoveOrDestroyChildren();

  // Notify descendants of a visibility change.
  void NotifyVisibilityChangeDown();

  // Query the display properties of the monitor that |window_| is on, and
  // forward them to |host_|.
  void UpdateWindowDisplay();

  // Return true if the delegate's modal type is window-modal. These display as
  // a native window "sheet", and have a different lifetime to regular windows.
  bool IsWindowModalSheet() const;

  // Show the window using -[NSApp beginSheet:..], modal for the parent window.
  void ShowAsModalSheet();

  // Returns true if capture exists and is currently active.
  bool HasCapture();

  // CocoaMouseCaptureDelegate:
  void PostCapturedEvent(NSEvent* event) override;
  void OnMouseCaptureLost() override;
  NSWindow* GetWindow() const override;

  // BridgedNativeWidgetOwner:
  NSWindow* GetNSWindow() override;
  gfx::Vector2d GetChildWindowOffset() const override;
  bool IsVisibleParent() const override;
  void RemoveChildWindow(BridgedNativeWidgetImpl* child) override;

  const uint64_t id_;
  BridgedNativeWidgetHost* const host_;               // Weak. Owns this.
  BridgedNativeWidgetHostHelper* const host_helper_;  // Weak, owned by |host_|.
  base::scoped_nsobject<NativeWidgetMacNSWindow> window_;
  base::scoped_nsobject<ViewsNSWindowDelegate> window_delegate_;
  base::scoped_nsobject<BridgedContentView> bridged_view_;
  base::scoped_nsobject<ModalShowAnimationWithLayer> show_animation_;
  std::unique_ptr<CocoaMouseCapture> mouse_capture_;
  std::unique_ptr<CocoaWindowMoveLoop> window_move_loop_;
  ui::ModalType modal_type_ = ui::MODAL_TYPE_NONE;
  bool is_translucent_window_ = false;
  bool widget_is_top_level_ = false;

  BridgedNativeWidgetOwner* parent_ = nullptr;  // Weak. If non-null, owns this.
  std::vector<BridgedNativeWidgetImpl*> child_windows_;

  // The size of the content area of the window most recently sent to |host_|
  // (and its compositor).
  gfx::Size content_dip_size_;

  // The size of the frame most recently *received from* the compositor. Note
  // that during resize (and showing new windows), this will lag behind
  // |content_dip_size_|, which is the frame size most recently *sent to* the
  // compositor.
  gfx::Size compositor_frame_dip_size_;
  std::unique_ptr<ui::DisplayCALayerTree> display_ca_layer_tree_;

  // Tracks the bounds when the window last started entering fullscreen. Used to
  // provide an answer for GetRestoredBounds(), but not ever sent to Cocoa (it
  // has its own copy, but doesn't provide access to it).
  gfx::Rect bounds_before_fullscreen_;

  // The transition types to animate when not relying on native NSWindow
  // animation behaviors.
  views_bridge_mac::mojom::VisibilityTransition transitions_to_animate_ =
      views_bridge_mac::mojom::VisibilityTransition::kBoth;

  // Whether this window wants to be fullscreen. If a fullscreen animation is in
  // progress then it might not be actually fullscreen.
  bool target_fullscreen_state_ = false;

  // Whether this window is in a fullscreen transition, and the fullscreen state
  // can not currently be changed.
  bool in_fullscreen_transition_ = false;

  // Stores the value last read from -[NSWindow isVisible], to detect visibility
  // changes.
  bool window_visible_ = false;

  // If true, the window is either visible, or wants to be visible but is
  // currently hidden due to having a hidden parent.
  bool wants_to_be_visible_ = false;

  // If true, then ignore interactions with CATransactionCoordinator until the
  // first frame arrives.
  bool ca_transaction_sync_suppressed_ = false;

  // If true, the window has been made visible or changed shape and the window
  // shadow needs to be invalidated when a frame is received for the new shape.
  bool invalidate_shadow_on_frame_swap_ = false;

  mojo::Binding<views_bridge_mac::mojom::BridgedNativeWidget>
      bridge_mojo_binding_;
  DISALLOW_COPY_AND_ASSIGN(BridgedNativeWidgetImpl);
};

}  // namespace views

#endif  // UI_VIEWS_COCOA_BRIDGED_NATIVE_WIDGET_H_
