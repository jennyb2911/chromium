// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_USB_USB_CHOOSER_CONTROLLER_H_
#define CHROME_BROWSER_USB_USB_CHOOSER_CONTROLLER_H_

#include <unordered_map>
#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observer.h"
#include "chrome/browser/chooser_controller/chooser_controller.h"
#include "chrome/browser/usb/usb_chooser_context.h"
#include "device/usb/public/mojom/device.mojom.h"
#include "third_party/blink/public/mojom/usb/web_usb_service.mojom.h"
#include "url/gurl.h"

namespace content {
class RenderFrameHost;
class WebContents;
}

// UsbChooserController creates a chooser for WebUSB.
// It is owned by ChooserBubbleDelegate.
class UsbChooserController : public ChooserController,
                             public UsbChooserContext::Observer {
 public:
  UsbChooserController(
      content::RenderFrameHost* render_frame_host,
      std::vector<device::mojom::UsbDeviceFilterPtr> device_filters,
      blink::mojom::WebUsbService::GetPermissionCallback callback);
  ~UsbChooserController() override;

  // ChooserController:
  base::string16 GetNoOptionsText() const override;
  base::string16 GetOkButtonLabel() const override;
  size_t NumOptions() const override;
  base::string16 GetOption(size_t index) const override;
  bool IsPaired(size_t index) const override;
  void Select(const std::vector<size_t>& indices) override;
  void Cancel() override;
  void Close() override;
  void OpenHelpCenterUrl() const override;

  // UsbChooserContext::Observer implementation:
  void OnDeviceAdded(device::mojom::UsbDeviceInfoPtr device_info) override;
  void OnDeviceRemoved(device::mojom::UsbDeviceInfoPtr device_info) override;
  void OnDeviceManagerConnectionError() override;

 private:
  void GotUsbDeviceList(std::vector<device::mojom::UsbDeviceInfoPtr> devices);
  bool DisplayDevice(const device::mojom::UsbDeviceInfo& device) const;

  std::vector<device::mojom::UsbDeviceFilterPtr> filters_;
  blink::mojom::WebUsbService::GetPermissionCallback callback_;
  GURL requesting_origin_;
  GURL embedding_origin_;

  content::WebContents* const web_contents_;
  base::WeakPtr<UsbChooserContext> chooser_context_;
  ScopedObserver<UsbChooserContext, UsbChooserContext::Observer> observer_;

  // Each pair is a (device, device name).
  std::vector<std::pair<device::mojom::UsbDeviceInfoPtr, base::string16>>
      devices_;
  // Maps from device name to number of devices.
  std::unordered_map<base::string16, int> device_name_map_;
  base::WeakPtrFactory<UsbChooserController> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(UsbChooserController);
};

#endif  // CHROME_BROWSER_USB_USB_CHOOSER_CONTROLLER_H_
