// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_VIEWS_BUBBLE_BUBBLE_DIALOG_DELEGATE_VIEW_H_
#define UI_VIEWS_BUBBLE_BUBBLE_DIALOG_DELEGATE_VIEW_H_

#include <memory>

#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "build/build_config.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/dialog_delegate.h"

#if defined(OS_MACOSX)
#include "ui/base/cocoa/bubble_closer.h"
#endif

namespace gfx {
class Rect;
}

namespace views {

class BubbleFrameView;
class Button;

// BubbleDialogDelegateView is a special DialogDelegateView for bubbles.
class VIEWS_EXPORT BubbleDialogDelegateView : public DialogDelegateView,
                                              public WidgetObserver {
 public:
  // Internal class name.
  static const char kViewClassName[];

  enum class CloseReason {
    DEACTIVATION,
    CLOSE_BUTTON,
    UNKNOWN,
  };

  ~BubbleDialogDelegateView() override;

  // Create and initialize the bubble Widget(s) with proper bounds.
  static Widget* CreateBubble(BubbleDialogDelegateView* bubble_delegate);

  // DialogDelegateView:
  BubbleDialogDelegateView* AsBubbleDialogDelegate() override;
  bool ShouldShowCloseButton() const override;
  ClientView* CreateClientView(Widget* widget) override;
  NonClientFrameView* CreateNonClientFrameView(Widget* widget) override;
  const char* GetClassName() const override;

  // WidgetObserver:
  void OnWidgetDestroying(Widget* widget) override;
  void OnWidgetVisibilityChanging(Widget* widget, bool visible) override;
  void OnWidgetVisibilityChanged(Widget* widget, bool visible) override;
  void OnWidgetActivationChanged(Widget* widget, bool active) override;
  void OnWidgetBoundsChanged(Widget* widget,
                             const gfx::Rect& new_bounds) override;

  bool close_on_deactivate() const { return close_on_deactivate_; }
  void set_close_on_deactivate(bool close) { close_on_deactivate_ = close; }

  View* GetAnchorView() const;
  Widget* anchor_widget() const { return anchor_widget_; }

  void SetHighlightedButton(Button* highlighted_button);

  // The anchor rect is used in the absence of an assigned anchor view.
  const gfx::Rect& anchor_rect() const { return anchor_rect_; }

  BubbleBorder::Arrow arrow() const { return arrow_; }
  void SetArrow(BubbleBorder::Arrow arrow);

  void set_mirror_arrow_in_rtl(bool mirror) { mirror_arrow_in_rtl_ = mirror; }

  BubbleBorder::Shadow GetShadow() const;
  void set_shadow(BubbleBorder::Shadow shadow) { shadow_ = shadow; }

  SkColor color() const { return color_; }
  void set_color(SkColor color) {
    color_ = color;
    color_explicitly_set_ = true;
  }

  void set_title_margins(const gfx::Insets& title_margins) {
    title_margins_ = title_margins;
  }

  // TODO(pbos): Remove by overriding Views::GetAnchorBoundsInScreen() instead.
  // See https://crbug.com/869928.
  const gfx::Insets& anchor_view_insets() const { return anchor_view_insets_; }
  void set_anchor_view_insets(const gfx::Insets& i) { anchor_view_insets_ = i; }

  gfx::NativeView parent_window() const { return parent_window_; }
  void set_parent_window(gfx::NativeView window) { parent_window_ = window; }

  bool accept_events() const { return accept_events_; }
  void set_accept_events(bool accept_events) { accept_events_ = accept_events; }

  bool adjust_if_offscreen() const { return adjust_if_offscreen_; }
  void set_adjust_if_offscreen(bool adjust) { adjust_if_offscreen_ = adjust; }

  // Get the arrow's anchor rect in screen space.
  virtual gfx::Rect GetAnchorRect() const;

  // Allows delegates to provide custom parameters before widget initialization.
  virtual void OnBeforeBubbleWidgetInit(Widget::InitParams* params,
                                        Widget* widget) const;

  // Sets the content margins to a default picked for smaller bubbles.
  void UseCompactMargins();

  // Call this method when the anchor bounds have changed to reposition the
  // bubble. The bubble is automatically repositioned when the anchor view
  // bounds change as a result of the widget's bounds changing.
  void OnAnchorBoundsChanged();

  // If this is called, enables focus to traverse from the anchor view
  // to inside this dialog and back out. This may become the default in
  // the future.
  void EnableFocusTraversalFromAnchorView();

 protected:
  BubbleDialogDelegateView();
  // |shadow| usually doesn't need to be explicitly set, just uses the default
  // argument. Unless on Mac when the bubble needs to use Views base shadow,
  // override it with suitable bubble border type.
  BubbleDialogDelegateView(
      View* anchor_view,
      BubbleBorder::Arrow arrow,
      BubbleBorder::Shadow shadow = BubbleBorder::DIALOG_SHADOW);

  // Get bubble bounds from the anchor rect and client view's preferred size.
  virtual gfx::Rect GetBubbleBounds();

  // DialogDelegateView:
  ax::mojom::Role GetAccessibleWindowRole() const override;

  // Disallow overrides of GetMinimumSize and GetMaximumSize(). These would only
  // be called by the FrameView, but the BubbleFrameView ignores these. Bubbles
  // are not user-sizable and always size to their preferred size (plus any
  // border / frame).
  gfx::Size GetMinimumSize() const final;
  gfx::Size GetMaximumSize() const final;

  void OnNativeThemeChanged(const ui::NativeTheme* theme) override;

  // Perform view initialization on the contents for bubble sizing.
  virtual void Init();

  // Sets the anchor view or rect and repositions the bubble. Note that if a
  // valid view gets passed, the anchor rect will get ignored. If the view gets
  // deleted, but no new view gets set, the last known anchor postion will get
  // returned.
  void SetAnchorView(View* anchor_view);
  void SetAnchorRect(const gfx::Rect& rect);

  // Resize and potentially move the bubble to fit the content's preferred size.
  virtual void SizeToContents();

  BubbleFrameView* GetBubbleFrameView() const;

 private:
  friend class BubbleBorderDelegate;
  friend class BubbleWindowTargeter;

  FRIEND_TEST_ALL_PREFIXES(BubbleDelegateTest, CreateDelegate);
  FRIEND_TEST_ALL_PREFIXES(BubbleDelegateTest, NonClientHitTest);

  // Update the bubble color from |theme|, unless it was explicitly set.
  void UpdateColorsFromTheme(const ui::NativeTheme* theme);

  // Handles widget visibility changes.
  void HandleVisibilityChanged(Widget* widget, bool visible);

  // Called when a deactivation is detected.
  void OnDeactivate();

  // When a bubble is visible, the anchor widget should always render as active.
  void UpdateAnchorWidgetRenderState(bool visible);

  // Update the button highlight, which may be the anchor view or an explicit
  // view set in |highlighted_button_tracker_|.
  void UpdateHighlightedButton(bool highlighted);

  // A flag controlling bubble closure on deactivation.
  bool close_on_deactivate_;

  // The view and widget to which this bubble is anchored. Since an anchor view
  // can be deleted without notice, we store it in a ViewTracker and retrieve
  // it from there. It will make sure that the view is still valid.
  std::unique_ptr<ViewTracker> anchor_view_tracker_;
  Widget* anchor_widget_;

  // If provided, this button should be highlighted while the bubble is visible.
  // If not provided, the anchor_view will attempt to be highlighted. A
  // ViewTracker is used because the view can be deleted.
  ViewTracker highlighted_button_tracker_;

  // The anchor rect used in the absence of an anchor view.
  mutable gfx::Rect anchor_rect_;

  // The arrow's location on the bubble.
  BubbleBorder::Arrow arrow_;

  // Automatically mirror the arrow in RTL layout.
  bool mirror_arrow_in_rtl_;

  // Bubble border shadow to use.
  BubbleBorder::Shadow shadow_;

  // The background color of the bubble; and flag for when it's explicitly set.
  SkColor color_;
  bool color_explicitly_set_;

  // The margins around the title.
  // TODO(tapted): Investigate deleting this when MD is default.
  gfx::Insets title_margins_;

  // Insets applied to the |anchor_view_| bounds.
  gfx::Insets anchor_view_insets_;

  // Specifies whether the bubble (or its border) handles mouse events, etc.
  bool accept_events_;

  // If true (defaults to true), the arrow may be mirrored and moved to fit the
  // bubble on screen better. It would be a no-op if the bubble has no arrow.
  bool adjust_if_offscreen_;

  // Parent native window of the bubble.
  gfx::NativeView parent_window_;

#if defined(OS_MACOSX)
  // Special handler for close_on_deactivate() on Mac. Window (de)activation is
  // suppressed by the WindowServer when clicking rapidly, so the bubble must
  // monitor clicks as well for the desired behavior.
  std::unique_ptr<ui::BubbleCloser> mac_bubble_closer_;
#endif

  DISALLOW_COPY_AND_ASSIGN(BubbleDialogDelegateView);
};

}  // namespace views

#endif  // UI_VIEWS_BUBBLE_BUBBLE_DIALOG_DELEGATE_VIEW_H_
