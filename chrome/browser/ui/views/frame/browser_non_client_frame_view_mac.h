// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_FRAME_BROWSER_NON_CLIENT_FRAME_VIEW_MAC_H_
#define CHROME_BROWSER_UI_VIEWS_FRAME_BROWSER_NON_CLIENT_FRAME_VIEW_MAC_H_

#include "base/mac/scoped_nsobject.h"
#include "base/macros.h"
#include "chrome/browser/ui/views/frame/avatar_button_manager.h"
#include "chrome/browser/ui/views/frame/browser_non_client_frame_view.h"
#include "chrome/browser/ui/views/profiles/profile_indicator_icon.h"
#include "components/prefs/pref_change_registrar.h"

@class FullscreenToolbarControllerViews;

class BrowserNonClientFrameViewMac : public BrowserNonClientFrameView {
 public:
  // Mac implementation of BrowserNonClientFrameView.
  BrowserNonClientFrameViewMac(BrowserFrame* frame, BrowserView* browser_view);
  ~BrowserNonClientFrameViewMac() override;

  // BrowserNonClientFrameView:
  void OnFullscreenStateChanged() override;
  bool CaptionButtonsOnLeadingEdge() const override;
  gfx::Rect GetBoundsForTabStrip(views::View* tabstrip) const override;
  int GetTopInset(bool restored) const override;
  int GetThemeBackgroundXInset() const override;
  void UpdateFullscreenTopUI(bool needs_check_tab_fullscreen) override;
  bool ShouldHideTopUIForFullscreen() const override;
  void UpdateThrobber(bool running) override;
  int GetTabStripLeftInset() const override;

  // views::NonClientFrameView:
  gfx::Rect GetBoundsForClientView() const override;
  gfx::Rect GetWindowBoundsForClientBounds(
      const gfx::Rect& client_bounds) const override;
  int NonClientHitTest(const gfx::Point& point) override;
  void GetWindowMask(const gfx::Size& size, gfx::Path* window_mask) override;
  void ResetWindowControls() override;
  void UpdateWindowIcon() override;
  void UpdateWindowTitle() override;
  void SizeConstraintsChanged() override;

  // views::View:
  void Layout() override;
  gfx::Size GetMinimumSize() const override;

 protected:
  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

  // BrowserNonClientFrameView:
  AvatarButtonStyle GetAvatarButtonStyle() const override;

 private:
  void PaintThemedFrame(gfx::Canvas* canvas);

  // Returns the width taken by any items after the tabstrip, to the edge of the
  // window.  Does not include any padding between the tabstrip and these items.
  int GetAfterTabstripItemWidth() const;
  CGFloat FullscreenBackingBarHeight() const;

  // Calculate the y offset the top UI needs to shift down due to showing the
  // slide down menu bar at the very top in full screen.
  int TopUIFullscreenYOffset() const;

  // Used to keep track of the update of kShowFullscreenToolbar preference.
  PrefChangeRegistrar pref_registrar_;

  base::scoped_nsobject<FullscreenToolbarControllerViews>
      fullscreen_toolbar_controller_;

  DISALLOW_COPY_AND_ASSIGN(BrowserNonClientFrameViewMac);
};

#endif  // CHROME_BROWSER_UI_VIEWS_FRAME_BROWSER_NON_CLIENT_FRAME_VIEW_MAC_H_
