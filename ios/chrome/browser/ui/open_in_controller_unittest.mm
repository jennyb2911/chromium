// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Foundation/Foundation.h>

#include <memory>

#include "base/files/file_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/test/scoped_task_environment.h"
#import "ios/chrome/browser/tabs/tab.h"
#import "ios/chrome/browser/ui/open_in_controller.h"
#import "ios/chrome/browser/ui/open_in_controller_testing.h"
#include "ios/web/public/test/test_web_thread.h"
#import "ios/web/web_state/ui/crw_web_controller.h"
#include "net/url_request/test_url_fetcher_factory.h"
#include "net/url_request/url_fetcher_delegate.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/platform_test.h"
#import "third_party/ocmock/OCMock/OCMock.h"
#include "third_party/ocmock/gtest_support.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace {

class OpenInControllerTest : public PlatformTest {
 public:
  OpenInControllerTest()
      : test_shared_url_loader_factory_(
            base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
                &test_url_loader_factory_)) {}

  void TearDown() override { PlatformTest::TearDown(); }

  void SetUp() override {
    PlatformTest::SetUp();

    // Set up the directory it downloads the file to.
    // Note that the value of kDocumentsTempPath must match the one in
    // open_in_controller.mm
    {
      NSString* const kDocumentsTempPath = @"OpenIn";
      NSString* tempDirPath = [NSTemporaryDirectory()
          stringByAppendingPathComponent:kDocumentsTempPath];
      base::FilePath directory(base::SysNSStringToUTF8(tempDirPath));
      EXPECT_TRUE(base::CreateDirectory(directory));
    }

    GURL documentURL = GURL("http://www.test.com/doc.pdf");
    parent_view_ = [[UIView alloc] init];
    id webController = [OCMockObject niceMockForClass:[CRWWebController class]];
    open_in_controller_ = [[OpenInController alloc]
        initWithURLLoaderFactory:test_shared_url_loader_factory_
                   webController:webController];
    [open_in_controller_ enableWithDocumentURL:documentURL
                             suggestedFilename:@"doc.pdf"];
  }

  base::test::ScopedTaskEnvironment scoped_task_environment_;
  network::TestURLLoaderFactory test_url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory>
      test_shared_url_loader_factory_;

  net::TestURLFetcherFactory factory_;
  OpenInController* open_in_controller_;
  UIView* parent_view_;
};

TEST_F(OpenInControllerTest, TestDisplayOpenInMenu) {
  id documentController =
      [OCMockObject niceMockForClass:[UIDocumentInteractionController class]];
  [open_in_controller_ setDocumentInteractionController:documentController];
  [open_in_controller_ startDownload];
  [[[documentController expect] andReturnValue:[NSNumber numberWithBool:YES]]
      presentOpenInMenuFromRect:CGRectMake(0, 0, 0, 0)
                         inView:OCMOCK_ANY
                       animated:YES];

  auto* pending_request = test_url_loader_factory_.GetPendingRequest(0);
  DCHECK(pending_request);
  // Set the response for the set URLFetcher to be a blank PDF.
  NSMutableData* pdfData = [NSMutableData data];
  UIGraphicsBeginPDFContextToData(pdfData, CGRectMake(0, 0, 100, 100), @{});
  UIGraphicsBeginPDFPage();
  UIGraphicsEndPDFContext();
  unsigned char* array = (unsigned char*)[pdfData bytes];

  test_url_loader_factory_.SimulateResponseForPendingRequest(
      pending_request->request.url.spec(),
      std::string((char*)array, sizeof(pdfData)));
  scoped_task_environment_.RunUntilIdle();

  EXPECT_OCMOCK_VERIFY(documentController);
}

}  // namespace
