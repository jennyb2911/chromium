// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_TABS_TAB_H_
#define CHROME_BROWSER_UI_VIEWS_TABS_TAB_H_

#include <list>
#include <memory>
#include <string>

#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "cc/paint/paint_record.h"
#include "chrome/browser/ui/views/tabs/glow_hover_controller.h"
#include "chrome/browser/ui/views/tabs/tab_renderer_data.h"
#include "ui/base/layout.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/paint_throbber.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/masked_targeter_delegate.h"
#include "ui/views/view.h"

class AlertIndicatorButton;
class TabCloseButton;
class TabController;
class TabIcon;

namespace gfx {
class Animation;
class AnimationContainer;
class LinearAnimation;
class ThrobAnimation;
}
namespace views {
class Label;
}

///////////////////////////////////////////////////////////////////////////////
//
//  A View that renders a Tab in a TabStrip.
//
///////////////////////////////////////////////////////////////////////////////
class Tab : public gfx::AnimationDelegate,
            public views::ButtonListener,
            public views::ContextMenuController,
            public views::MaskedTargeterDelegate,
            public views::View {
 public:
  // The Tab's class name.
  static const char kViewClassName[];

  // Thickness in DIPs of the separator painted on the left and right edges of
  // the tab.
  static constexpr int kSeparatorThickness = 1;

  // When the content's width of the tab shrinks to below this size we should
  // hide the close button on inactive tabs. Any smaller and they're too easy
  // to hit on accident.
  static constexpr int kMinimumContentsWidthForCloseButtons = 68;
  static constexpr int kTouchableMinimumContentsWidthForCloseButtons = 100;

  Tab(TabController* controller, gfx::AnimationContainer* container);
  ~Tab() override;

  // gfx::AnimationDelegate:
  void AnimationEnded(const gfx::Animation* animation) override;
  void AnimationProgressed(const gfx::Animation* animation) override;
  void AnimationCanceled(const gfx::Animation* animation) override;

  // views::ButtonListener:
  void ButtonPressed(views::Button* sender, const ui::Event& event) override;

  // views::ContextMenuController:
  void ShowContextMenuForView(views::View* source,
                              const gfx::Point& point,
                              ui::MenuSourceType source_type) override;

  // views::MaskedTargeterDelegate:
  bool GetHitTestMask(gfx::Path* mask) const override;

  // views::View:
  void Layout() override;
  const char* GetClassName() const override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseEntered(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  void OnGestureEvent(ui::GestureEvent* event) override;
  bool GetTooltipText(const gfx::Point& p,
                      base::string16* tooltip) const override;
  bool GetTooltipTextOrigin(const gfx::Point& p,
                            gfx::Point* origin) const override;
  void GetAccessibleNodeData(ui::AXNodeData* node_data) override;
  gfx::Size CalculatePreferredSize() const override;
  void PaintChildren(const views::PaintInfo& info) override;
  void OnPaint(gfx::Canvas* canvas) override;
  void AddedToWidget() override;
  void OnThemeChanged() override;

  TabController* controller() const { return controller_; }

  // Used to set/check whether this Tab is being animated closed.
  void SetClosing(bool closing);
  bool closing() const { return closing_; }

  // See description above field.
  void set_dragging(bool dragging) { dragging_ = dragging; }
  bool dragging() const { return dragging_; }

  // Used to mark the tab as having been detached.  Once this has happened, the
  // tab should be invisibly closed.  This is irreversible.
  void set_detached() { detached_ = true; }
  bool detached() const { return detached_; }

  // Returns the color used for the alert indicator icon.
  SkColor GetAlertIndicatorColor(TabAlertState state) const;

  // Returns true if this tab is the active tab.
  bool IsActive() const;

  // Notifies the AlertIndicatorButton that the active state of this tab has
  // changed.
  void ActiveStateChanged();

  // Called when the alert indicator has changed states.
  void AlertStateChanged();

  // Called when the frame state color changes.
  void FrameColorsChanged();

  // Called when the selected state changes.
  void SelectedStateChanged();

  // Returns true if the tab is selected.
  bool IsSelected() const;

  // Sets the data this tabs displays. Should only be called after Tab is added
  // to widget hierarchy.
  void SetData(TabRendererData data);
  const TabRendererData& data() const { return data_; }

  // Redraws the loading animation if one is visible. Otherwise, no-op.
  void StepLoadingAnimation();

  // Starts/Stops a pulse animation.
  void StartPulse();
  void StopPulse();

  // Sets the visibility of the indicator shown when the tab needs to indicate
  // to the user that it needs their attention.
  void SetTabNeedsAttention(bool attention);

  // Set the background X offset used to match the image in the inactive tab
  // to the frame image.
  void set_background_offset(int offset) { background_offset_ = offset; }

  // Returns true if this tab became the active tab selected in
  // response to the last ui::ET_TAP_DOWN gesture dispatched to
  // this tab. Only used for collecting UMA metrics.
  // See ash/touch/touch_uma.cc.
  bool tab_activated_with_last_tap_down() const {
    return tab_activated_with_last_tap_down_;
  }

  GlowHoverController* hover_controller() { return &hover_controller_; }

  bool mouse_hovered() const { return mouse_hovered_; }

  // Returns the thickness of the stroke drawn around the top and sides of the
  // tab.  Only active tabs may have a stroke, and not in all cases.  If there
  // is no stroke, returns 0.  If |should_paint_as_active| is true, the tab is
  // treated as an active tab regardless of its true current state.
  float GetStrokeThickness(bool should_paint_as_active = false) const;

  // Returns the thickness of the stroke drawn below the tab.
  float GetBottomStrokeThickness(bool should_paint_as_active = false) const;

  // Returns the width of the largest part of the tab that is available for the
  // user to click to select/activate the tab.
  int GetWidthOfLargestSelectableRegion() const;

  // Returns the insets to use for laying out tab contents.
  gfx::Insets GetContentsInsets() const;

  // Returns the horizontal insets to use for laying out tab contents.
  static gfx::Insets GetContentsHorizontalInsets();

  // Returns the minimum possible width of a single unselected Tab.
  static int GetMinimumInactiveWidth();

  // Returns the minimum possible width of a selected Tab. Selected tabs must
  // always show a close button, and thus have a larger minimum size than
  // unselected tabs.
  static int GetMinimumActiveWidth();

  // Returns the preferred width of a single Tab, assuming space is
  // available.
  static int GetStandardWidth();

  // Returns the width for pinned tabs. Pinned tabs always have this width.
  static int GetPinnedWidth();

  // Returns the height of the separator between tabs.
  static int GetTabSeparatorHeight();

  // Returns the radius of the outer corners of the tab shape.
  static int GetCornerRadius();

  // Returns an offset into the leading edge of the tab which delineates the
  // "main body" of the tab from the user's perspective; dragging based on this
  // point feels better than dragging based on the tab's actual leading edge.
  static int GetDragInset();

  // Returns the overlap between adjacent tabs.
  static int GetOverlap();

 private:
  friend class AlertIndicatorButtonTest;
  friend class TabTest;
  friend class TabStripTest;
  FRIEND_TEST_ALL_PREFIXES(TabStripTest, TabCloseButtonVisibilityWhenStacked);
  FRIEND_TEST_ALL_PREFIXES(TabStripTest,
                           TabCloseButtonVisibilityWhenNotStacked);
  FRIEND_TEST_ALL_PREFIXES(TabTest, TitleTextHasSufficientContrast);

  // Contains values 0..1 representing the opacity of the corresponding
  // separators.  These are physical and not logical, so "left" is the left
  // separator in both LTR and RTL.
  struct SeparatorOpacities {
    float left = 0, right = 0;
  };

  // Invoked from Layout to adjust the position of the favicon or alert
  // indicator for pinned tabs. The visual_width parameter is how wide the
  // icon looks (rather than how wide the bounds are).
  void MaybeAdjustLeftForPinnedTab(gfx::Rect* bounds, int visual_width) const;

  // Paints with the normal tab style.  If |clip| is non-empty, the tab border
  // should be clipped against it.
  void PaintTab(gfx::Canvas* canvas, const gfx::Path& clip);

  // Paints the background of an inactive tab.
  void PaintInactiveTabBackground(gfx::Canvas* canvas, const gfx::Path& clip);

  // Paints a tab background using the image defined by |fill_id| at the
  // provided offset. If |fill_id| is 0, it will fall back to using the solid
  // color defined by the theme provider and ignore the offset.
  void PaintTabBackground(gfx::Canvas* canvas,
                          bool active,
                          int fill_id,
                          int y_inset,
                          const gfx::Path* clip);

  // Helper methods for PaintTabBackground.
  void PaintTabBackgroundFill(gfx::Canvas* canvas,
                              const gfx::Path& fill_path,
                              bool active,
                              bool hover,
                              SkColor active_color,
                              SkColor inactive_color,
                              int fill_id,
                              int y_inset);
  void PaintTabBackgroundStroke(gfx::Canvas* canvas,
                                const gfx::Path& fill_path,
                                const gfx::Path& stroke_path,
                                bool active,
                                SkColor color);

  // Paints the separator lines on the left and right edge of the tab.
  void PaintSeparators(gfx::Canvas* canvas);

  // Computes which icons are visible in the tab. Should be called everytime
  // before layout is performed.
  void UpdateIconVisibility();

  // Returns whether the tab should be rendered as a normal tab as opposed to a
  // pinned tab.
  bool ShouldRenderAsNormalTab() const;

  // Returns the opacities of the separators.  If |for_layout| is true, returns
  // the "layout" opacities, which ignore the effects of surrounding tabs' hover
  // effects and consider only the current tab's state.
  SeparatorOpacities GetSeparatorOpacities(bool for_layout) const;

  // Returns the final hover opacity for this tab (considers tab width).
  float GetHoverOpacity() const;

  // Gets the throb value for the tab. When a tab is not selected the active
  // background is drawn at GetThrobValue() * 100%. This is used for hover, mini
  // tab title change and pulsing.
  float GetThrobValue() const;

  // Updates the blocked attention state of the |icon_|. This only updates
  // state; it is the responsibility of the caller to request a paint.
  void UpdateTabIconNeedsAttentionBlocked();

  // Selects, generates, and applies colors for various foreground elements to
  // ensure proper contrast. Elements affected include title text, close button
  // and alert icon.
  void UpdateForegroundColors();

  // The controller, never NULL.
  TabController* const controller_;

  TabRendererData data_;

  // True if the tab is being animated closed.
  bool closing_ = false;

  // True if the tab is being dragged.
  bool dragging_ = false;

  // True if the tab has been detached.
  bool detached_ = false;

  // Whole-tab throbbing "pulse" animation.
  gfx::ThrobAnimation pulse_animation_;

  scoped_refptr<gfx::AnimationContainer> animation_container_;

  TabIcon* icon_ = nullptr;
  AlertIndicatorButton* alert_indicator_button_ = nullptr;
  TabCloseButton* close_button_ = nullptr;

  views::Label* title_;
  // The title's bounds are animated when switching between showing and hiding
  // the tab's favicon/throbber.
  gfx::Rect start_title_bounds_;
  gfx::Rect target_title_bounds_;
  gfx::LinearAnimation title_animation_;

  bool tab_activated_with_last_tap_down_ = false;

  GlowHoverController hover_controller_;

  // The offset used to paint the inactive background image.
  int background_offset_;

  // For narrow tabs, we show the favicon even if it won't completely fit.
  // In this case, we need to center the favicon within the tab; it will be
  // clipped to fit.
  bool center_favicon_ = false;

  // Whether we're showing the icon. It is cached so that we can detect when it
  // changes and layout appropriately.
  bool showing_icon_ = false;

  // Whether we're showing the alert indicator. It is cached so that we can
  // detect when it changes and layout appropriately.
  bool showing_alert_indicator_ = false;

  // Whether we are showing the close button. It is cached so that we can
  // detect when it changes and layout appropriately.
  bool showing_close_button_ = false;

  // If there's room, we add additional padding to the left of the favicon to
  // balance the whitespace inside the non-hovered close button image;
  // otherwise, the tab contents look too close to the left edge. Once the tabs
  // get too small, we let the tab contents take the full width, to maximize
  // visible area.
  bool extra_padding_before_content_ = false;

  // When both the close button and alert indicator are visible, we add extra
  // padding between them to space them out visually.
  bool extra_alert_indicator_padding_ = false;

  // The current color of the alert indicator and close button icons.
  SkColor button_color_ = SK_ColorTRANSPARENT;

  // Indicates whether the mouse is currently hovered over the tab. This is
  // different from View::IsMouseHovered() which does a naive intersection with
  // the view bounds.
  bool mouse_hovered_ = false;

  class BackgroundCache {
   public:
    BackgroundCache();
    ~BackgroundCache();

    bool CacheKeyMatches(float scale,
                         const gfx::Size& size,
                         SkColor active_color,
                         SkColor inactive_color,
                         SkColor stroke_color,
                         float stroke_thickness) {
      return scale_ == scale && size_ == size &&
             active_color_ == active_color &&
             inactive_color_ == inactive_color &&
             stroke_color_ == stroke_color &&
             stroke_thickness_ == stroke_thickness;
    }

    void SetCacheKey(float scale,
                     const gfx::Size& size,
                     SkColor active_color,
                     SkColor inactive_color,
                     SkColor stroke_color,
                     float stroke_thickness) {
      scale_ = scale;
      size_ = size;
      active_color_ = active_color;
      inactive_color_ = inactive_color;
      stroke_color_ = stroke_color;
      stroke_thickness_ = stroke_thickness;
    }

    // The PaintRecords being cached based on the input parameters.
    sk_sp<cc::PaintRecord> fill_record;
    sk_sp<cc::PaintRecord> stroke_record;

   private:
    // Parameters used to construct the PaintRecords.
    float scale_ = 0.f;
    gfx::Size size_;
    SkColor active_color_ = 0;
    SkColor inactive_color_ = 0;
    SkColor stroke_color_ = 0;

    // The stroke thickness needs to be recorded because tabs may switch between
    // a zero and non-zero stroke thickness depending on their state.  This
    // changes the "stroke_thickness > 0" logic in tab.cc which changes if
    // |stroke_record| gets recorded.
    float stroke_thickness_ = 0.f;
  };

  // Cache of the paint output for tab backgrounds.
  BackgroundCache background_active_cache_;
  BackgroundCache background_inactive_cache_;

  DISALLOW_COPY_AND_ASSIGN(Tab);
};

#endif  // CHROME_BROWSER_UI_VIEWS_TABS_TAB_H_
