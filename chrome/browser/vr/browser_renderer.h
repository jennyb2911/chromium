// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_VR_BROWSER_RENDERER_H_
#define CHROME_BROWSER_VR_BROWSER_RENDERER_H_

#include <memory>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/vr/gl_texture_location.h"
#include "chrome/browser/vr/graphics_delegate.h"
#include "chrome/browser/vr/scheduler_browser_renderer_interface.h"
#include "chrome/browser/vr/sliding_average.h"
#include "chrome/browser/vr/vr_export.h"
#include "device/vr/public/mojom/isolated_xr_service.mojom.h"
#include "device/vr/public/mojom/vr_service.mojom.h"

namespace base {
class TimeDelta;
class TimeTicks;
}  // namespace base

namespace vr {

enum class VrUiTestActivityResult;
class BrowserUiInterface;
class InputDelegate;
class PlatformInputHandler;
class PlatformUiInputDelegate;
class BrowserRendererBrowserInterface;
class SchedulerDelegate;
class UiInterface;
struct ControllerTestInput;
struct RenderInfo;
struct UiTestActivityExpectation;
struct UiTestState;

// The BrowserRenderer handles all input/output activities during a frame.
// This includes head movement, controller movement and input, audio output and
// rendering of the frame.
class VR_EXPORT BrowserRenderer : public SchedulerBrowserRendererInterface {
 public:
  BrowserRenderer(std::unique_ptr<UiInterface> ui,
                  std::unique_ptr<SchedulerDelegate> scheduler_delegate,
                  std::unique_ptr<GraphicsDelegate> graphics_delegate,
                  std::unique_ptr<InputDelegate> input_delegate,
                  BrowserRendererBrowserInterface* browser,
                  size_t sliding_time_size);
  ~BrowserRenderer() override;

  void OnPause();
  void OnResume();

  void OnExitPresent();
  void OnTriggerEvent(bool pressed);
  void SetWebXrMode(bool enabled);
  void OnSwapContents(int new_content_id);
  void EnableAlertDialog(PlatformInputHandler* input_handler,
                         float width,
                         float height);
  void DisableAlertDialog();
  void SetAlertDialogSize(float width, float height);
  void SetDialogLocation(float x, float y);
  void SetDialogFloating(bool floating);
  void ShowToast(const base::string16& text);
  void CancelToast();
  void ResumeContentRendering();
  void ContentBoundsChanged(int width, int height);
  void BufferBoundsChanged(const gfx::Size& content_buffer_size,
                           const gfx::Size& overlay_buffer_size);

  base::WeakPtr<BrowserRenderer> GetWeakPtr();
  base::WeakPtr<BrowserUiInterface> GetBrowserUiWeakPtr();

  void PerformControllerActionForTesting(ControllerTestInput controller_input);
  void SetUiExpectingActivityForTesting(
      UiTestActivityExpectation ui_expectation);
  void SaveNextFrameBufferToDiskForTesting(std::string filepath_base);
  void AcceptDoffPromptForTesting();
  void ConnectPresentingService(
      device::mojom::VRDisplayInfoPtr display_info,
      device::mojom::XRRuntimeSessionOptionsPtr options);

 private:
  // SchedulerBrowserRendererInterface implementation.
  void DrawBrowserFrame(base::TimeTicks current_time) override;
  void DrawWebXrFrame(base::TimeTicks current_time,
                      const gfx::Transform& head_pose) override;
  void ProcessControllerInputForWebXr(base::TimeTicks current_time) override;

  void Draw(FrameType frame_type,
            base::TimeTicks current_time,
            const gfx::Transform& head_pose);

  // Position, hide and/or show UI elements, process input and update textures.
  // Returns true if the scene changed.
  void UpdateUi(const RenderInfo& render_info,
                base::TimeTicks currrent_time,
                FrameType frame_type);
  void DrawWebXr();
  void DrawWebXrOverlay(const RenderInfo& render_info);
  void DrawContentQuad();
  void DrawBrowserUi(const RenderInfo& render_info);
  base::TimeDelta ProcessControllerInput(const RenderInfo& render_info,
                                         base::TimeTicks current_time);

  void ReportUiStatusForTesting(const base::TimeTicks& current_time,
                                bool ui_updated);
  void ReportUiActivityResultForTesting(VrUiTestActivityResult result);
  void ReportFrameBufferDumpForTesting();

  std::unique_ptr<UiInterface> ui_;
  std::unique_ptr<SchedulerDelegate> scheduler_delegate_;
  std::unique_ptr<GraphicsDelegate> graphics_delegate_;
  std::unique_ptr<InputDelegate> input_delegate_;
  std::unique_ptr<InputDelegate> input_delegate_for_testing_;
  bool using_input_delegate_for_testing_ = false;
  std::string frame_buffer_dump_filepath_base_;

  std::unique_ptr<PlatformUiInputDelegate> vr_dialog_input_delegate_;

  BrowserRendererBrowserInterface* browser_;

  std::unique_ptr<UiTestState> ui_test_state_;
  SlidingTimeDeltaAverage ui_processing_time_;
  SlidingTimeDeltaAverage ui_controller_update_time_;

  base::WeakPtrFactory<BrowserRenderer> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(BrowserRenderer);
};

}  // namespace vr

#endif  // CHROME_BROWSER_VR_BROWSER_RENDERER_H_
