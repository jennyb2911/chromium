// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PRINTING_PRINT_PREVIEW_MESSAGE_HANDLER_H_
#define CHROME_BROWSER_PRINTING_PRINT_PREVIEW_MESSAGE_HANDLER_H_

#include "base/macros.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/weak_ptr.h"
#include "chrome/services/printing/public/mojom/pdf_nup_converter.mojom.h"
#include "components/services/pdf_compositor/public/interfaces/pdf_compositor.mojom.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

class PrintPreviewUI;
struct PrintHostMsg_DidPreviewDocument_Params;
struct PrintHostMsg_DidPreviewPage_Params;
struct PrintHostMsg_DidStartPreview_Params;
struct PrintHostMsg_PreviewIds;
struct PrintHostMsg_RequestPrintPreview_Params;
struct PrintHostMsg_SetOptionsFromDocument_Params;

namespace base {
class RefCountedMemory;
}

namespace content {
class RenderFrameHost;
class WebContents;
}

namespace gfx {
class Rect;
class Size;
}

namespace printing {

struct PageSizeMargins;

// Manages the print preview handling for a WebContents.
class PrintPreviewMessageHandler
    : public content::WebContentsObserver,
      public content::WebContentsUserData<PrintPreviewMessageHandler> {
 public:
  ~PrintPreviewMessageHandler() override;

  // content::WebContentsObserver implementation.
  bool OnMessageReceived(const IPC::Message& message,
                         content::RenderFrameHost* render_frame_host) override;

  // Gets a symmetrical printable area.  It is defined as a static function to
  // make writing unit tests easier.
  static gfx::Rect GetSymmetricalPrintableArea(const gfx::Size& page_size,
                                               const gfx::Rect& printable_area);

 private:
  friend class content::WebContentsUserData<PrintPreviewMessageHandler>;

  explicit PrintPreviewMessageHandler(content::WebContents* web_contents);

  // Gets the print preview dialog associated with the WebContents being
  // observed.
  content::WebContents* GetPrintPreviewDialog();

  // Gets the PrintPreviewUI associated with the WebContents being observed and
  // verifies that its id matches |preview_ui_id|.
  PrintPreviewUI* GetPrintPreviewUI(int preview_ui_id);

  // Message handlers.
  void OnRequestPrintPreview(
      content::RenderFrameHost* render_frame_host,
      const PrintHostMsg_RequestPrintPreview_Params& params);
  void OnDidGetDefaultPageLayout(const PageSizeMargins& page_layout_in_points,
                                 const gfx::Rect& printable_area_in_points,
                                 bool has_custom_page_size_style,
                                 const PrintHostMsg_PreviewIds& ids);
  void OnDidStartPreview(const PrintHostMsg_DidStartPreview_Params& params,
                         const PrintHostMsg_PreviewIds& ids);
  void OnDidPreviewPage(content::RenderFrameHost* render_frame_host,
                        const PrintHostMsg_DidPreviewPage_Params& params,
                        const PrintHostMsg_PreviewIds& ids);
  void OnMetafileReadyForPrinting(
      content::RenderFrameHost* render_frame_host,
      const PrintHostMsg_DidPreviewDocument_Params& params,
      const PrintHostMsg_PreviewIds& ids);
  void OnPrintPreviewFailed(int document_cookie,
                            const PrintHostMsg_PreviewIds& ids);
  void OnPrintPreviewCancelled(int document_cookie,
                               const PrintHostMsg_PreviewIds& ids);
  void OnInvalidPrinterSettings(int document_cookie,
                                const PrintHostMsg_PreviewIds& ids);
  void OnSetOptionsFromDocument(
      const PrintHostMsg_SetOptionsFromDocument_Params& params,
      const PrintHostMsg_PreviewIds& ids);

  void NotifyUIPreviewPageReady(
      PrintPreviewUI* print_preview_ui,
      int page_number,
      const PrintHostMsg_PreviewIds& ids,
      scoped_refptr<base::RefCountedMemory> data_bytes);
  void NotifyUIPreviewDocumentReady(
      PrintPreviewUI* print_preview_ui,
      int page_count,
      const PrintHostMsg_PreviewIds& ids,
      scoped_refptr<base::RefCountedMemory> data_bytes);

  // Callbacks for pdf compositor client.
  void OnCompositePdfPageDone(int page_number,
                              int document_cookie,
                              const PrintHostMsg_PreviewIds& ids,
                              mojom::PdfCompositor::Status status,
                              base::ReadOnlySharedMemoryRegion region);
  void OnCompositePdfDocumentDone(int page_count,
                                  int document_cookie,
                                  const PrintHostMsg_PreviewIds& ids,
                                  mojom::PdfCompositor::Status status,
                                  base::ReadOnlySharedMemoryRegion region);

  void OnNupPdfConvertDone(int page_number,
                           const PrintHostMsg_PreviewIds& ids,
                           mojom::PdfNupConverter::Status status,
                           base::ReadOnlySharedMemoryRegion region);
  void OnNupPdfDocumentConvertDone(int page_count,
                                   const PrintHostMsg_PreviewIds& ids,
                                   mojom::PdfNupConverter::Status status,
                                   base::ReadOnlySharedMemoryRegion region);

  base::WeakPtrFactory<PrintPreviewMessageHandler> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(PrintPreviewMessageHandler);
};

}  // namespace printing

#endif  // CHROME_BROWSER_PRINTING_PRINT_PREVIEW_MESSAGE_HANDLER_H_
