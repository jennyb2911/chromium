// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "content/browser/frame_host/navigation_handle_impl.h"
#include "content/browser/web_package/signed_exchange_handler.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_paths.h"
#include "content/public/common/page_type.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/public/test/content_cert_verifier_browser_test.h"
#include "content/public/test/test_navigation_throttle.h"
#include "content/public/test/url_loader_interceptor.h"
#include "content/shell/browser/shell.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/test/cert_test_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/test_data_directory.h"
#include "net/test/url_request/url_request_mock_http_job.h"
#include "net/url_request/url_request_filter.h"
#include "net/url_request/url_request_interceptor.h"
#include "services/network/public/cpp/features.h"
#include "testing/gmock/include/gmock/gmock-matchers.h"

namespace content {

namespace {

const uint64_t kSignatureHeaderDate = 1520834000;  // 2018-03-12T05:53:20Z

class RedirectObserver : public WebContentsObserver {
 public:
  explicit RedirectObserver(WebContents* web_contents)
      : WebContentsObserver(web_contents) {}
  ~RedirectObserver() override = default;

  void DidRedirectNavigation(NavigationHandle* handle) override {
    const net::HttpResponseHeaders* response = handle->GetResponseHeaders();
    if (response)
      response_code_ = response->response_code();
  }

  const base::Optional<int>& response_code() const { return response_code_; }

 private:
  base::Optional<int> response_code_;

  DISALLOW_COPY_AND_ASSIGN(RedirectObserver);
};

class AssertNavigationHandleFlagObserver : public WebContentsObserver {
 public:
  explicit AssertNavigationHandleFlagObserver(WebContents* web_contents)
      : WebContentsObserver(web_contents) {}
  ~AssertNavigationHandleFlagObserver() override = default;

  void DidFinishNavigation(NavigationHandle* handle) override {
    EXPECT_TRUE(static_cast<NavigationHandleImpl*>(handle)->IsSignedExchangeInnerResponse());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(AssertNavigationHandleFlagObserver);
};

}  // namespace

class SignedExchangeRequestHandlerBrowserTest : public CertVerifierBrowserTest {
 public:
  SignedExchangeRequestHandlerBrowserTest() {
    // This installs "root_ca_cert.pem" from which our test certificates are
    // created. (Needed for the tests that use real certificate, i.e.
    // RealCertVerifier)
    net::EmbeddedTestServer::RegisterTestCerts();
  }

  void SetUp() override {
    SignedExchangeHandler::SetVerificationTimeForTesting(
        base::Time::UnixEpoch() +
        base::TimeDelta::FromSeconds(kSignatureHeaderDate));
    SetUpFeatures();
    CertVerifierBrowserTest::SetUp();
  }

  void TearDownOnMainThread() override {
    interceptor_.reset();
    SignedExchangeHandler::SetVerificationTimeForTesting(
        base::Optional<base::Time>());
  }

 protected:
  virtual void SetUpFeatures() {
    feature_list_.InitWithFeatures({features::kSignedHTTPExchange}, {});
  }

  static scoped_refptr<net::X509Certificate> LoadCertificate(
      const std::string& cert_file) {
    base::ScopedAllowBlockingForTesting allow_io;
    base::FilePath dir_path;
    base::PathService::Get(content::DIR_TEST_DATA, &dir_path);
    dir_path = dir_path.AppendASCII("sxg");

    return net::CreateCertificateChainFromFile(
        dir_path, cert_file, net::X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
  }

  void InstallUrlInterceptor(const GURL& url, const std::string& data_path) {
    if (base::FeatureList::IsEnabled(network::features::kNetworkService)) {
      if (!interceptor_) {
        interceptor_ =
            std::make_unique<URLLoaderInterceptor>(base::BindRepeating(
                &SignedExchangeRequestHandlerBrowserTest::OnInterceptCallback,
                base::Unretained(this)));
      }
      interceptor_data_path_map_[url] = data_path;
    } else {
      base::PostTaskWithTraits(
          FROM_HERE, {BrowserThread::IO},
          base::BindOnce(&InstallMockInterceptors, url, data_path));
    }
  }

  base::test::ScopedFeatureList feature_list_;
  const base::HistogramTester histogram_tester_;

 private:
  static void InstallMockInterceptors(const GURL& url,
                                      const std::string& data_path) {
    base::FilePath root_path;
    CHECK(base::PathService::Get(base::DIR_SOURCE_ROOT, &root_path));
    net::URLRequestFilter::GetInstance()->AddUrlInterceptor(
        url, net::URLRequestMockHTTPJob::CreateInterceptorForSingleFile(
                 root_path.AppendASCII(data_path)));
  }

  bool OnInterceptCallback(URLLoaderInterceptor::RequestParams* params) {
    const auto it = interceptor_data_path_map_.find(params->url_request.url);
    if (it == interceptor_data_path_map_.end())
      return false;
    URLLoaderInterceptor::WriteResponse(it->second, params->client.get());
    return true;
  }

  std::unique_ptr<URLLoaderInterceptor> interceptor_;
  std::map<GURL, std::string> interceptor_data_path_map_;

  DISALLOW_COPY_AND_ASSIGN(SignedExchangeRequestHandlerBrowserTest);
};

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerBrowserTest, Simple) {
  InstallUrlInterceptor(
      GURL("https://cert.example.org/cert.msg"),
      "content/test/data/sxg/test.example.org.public.pem.cbor");

  // Make the MockCertVerifier treat the certificate
  // "prime256v1-sha256.public.pem" as valid for "test.example.org".
  scoped_refptr<net::X509Certificate> original_cert =
      LoadCertificate("prime256v1-sha256.public.pem");
  net::CertVerifyResult dummy_result;
  dummy_result.verified_cert = original_cert;
  dummy_result.cert_status = net::OK;
  dummy_result.ocsp_result.response_status = net::OCSPVerifyResult::PROVIDED;
  dummy_result.ocsp_result.revocation_status = net::OCSPRevocationStatus::GOOD;
  mock_cert_verifier()->AddResultForCertAndHost(
      original_cert, "test.example.org", dummy_result, net::OK);

  embedded_test_server()->RegisterRequestMonitor(
      base::BindRepeating([](const net::test_server::HttpRequest& request) {
        if (request.relative_url == "/sxg/test.example.org_test.sxg") {
          const auto& accept_value = request.headers.find("accept")->second;
          EXPECT_THAT(accept_value,
                      ::testing::HasSubstr("application/signed-exchange;v=b2"));
        }
      }));
  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");
  base::string16 title = base::ASCIIToUTF16("https://test.example.org/test/");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  RedirectObserver redirect_observer(shell()->web_contents());
  AssertNavigationHandleFlagObserver assert_navigation_handle_flag_observer(
      shell()->web_contents());

  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(303, redirect_observer.response_code());

  NavigationEntry* entry =
      shell()->web_contents()->GetController().GetVisibleEntry();
  EXPECT_TRUE(entry->GetSSL().initialized);
  EXPECT_FALSE(!!(entry->GetSSL().content_status &
                  SSLStatus::DISPLAYED_INSECURE_CONTENT));
  ASSERT_TRUE(entry->GetSSL().certificate);

  // "test.example.org.public.pem.cbor" is generated from
  // "prime256v1-sha256.public.pem". So the SHA256 of the certificates must
  // match.
  const net::SHA256HashValue fingerprint =
      net::X509Certificate::CalculateFingerprint256(
          entry->GetSSL().certificate->cert_buffer());
  const net::SHA256HashValue original_fingerprint =
      net::X509Certificate::CalculateFingerprint256(
          original_cert->cert_buffer());
  EXPECT_EQ(original_fingerprint, fingerprint);
  histogram_tester_.ExpectUniqueSample("SignedExchange.LoadResult",
                                       SignedExchangeLoadResult::kSuccess, 1);
}

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerBrowserTest,
                       InvalidContentType) {
  InstallUrlInterceptor(
      GURL("https://cert.example.org/cert.msg"),
      "content/test/data/sxg/test.example.org.public.pem.cbor");
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");

  // Make the MockCertVerifier treat the certificate
  // "prime256v1-sha256.public.pem" as valid for "test.example.org".
  scoped_refptr<net::X509Certificate> original_cert =
      LoadCertificate("prime256v1-sha256.public.pem");
  net::CertVerifyResult dummy_result;
  dummy_result.verified_cert = original_cert;
  dummy_result.cert_status = net::OK;
  dummy_result.ocsp_result.response_status = net::OCSPVerifyResult::PROVIDED;
  dummy_result.ocsp_result.revocation_status = net::OCSPRevocationStatus::GOOD;
  mock_cert_verifier()->AddResultForCertAndHost(
      original_cert, "test.example.org", dummy_result, net::OK);

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL(
      "/sxg/test.example.org_test_invalid_content_type.sxg");

  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  RedirectObserver redirect_observer(shell()->web_contents());
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(303, redirect_observer.response_code());
  histogram_tester_.ExpectUniqueSample(
      "SignedExchange.LoadResult", SignedExchangeLoadResult::kVersionMismatch,
      1);
}

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerBrowserTest,
                       RedirectBrokenSignedExchanges) {
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());

  constexpr const char* kBrokenExchanges[] = {
      "/sxg/test.example.org_test_invalid_magic_string.sxg",
      "/sxg/test.example.org_test_invalid_cbor_header.sxg",
  };

  for (const auto* broken_exchange : kBrokenExchanges) {
    SCOPED_TRACE(testing::Message()
                 << "testing broken exchange: " << broken_exchange);

    GURL url = embedded_test_server()->GetURL(broken_exchange);

    base::string16 title = base::ASCIIToUTF16("Fallback URL response");
    TitleWatcher title_watcher(shell()->web_contents(), title);
    NavigateToURL(shell(), url);
    EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  }
  histogram_tester_.ExpectTotalCount("SignedExchange.LoadResult", 2);
  histogram_tester_.ExpectBucketCount(
      "SignedExchange.LoadResult", SignedExchangeLoadResult::kVersionMismatch,
      1);
  histogram_tester_.ExpectBucketCount(
      "SignedExchange.LoadResult", SignedExchangeLoadResult::kHeaderParseError,
      1);
}

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerBrowserTest, CertNotFound) {
  InstallUrlInterceptor(GURL("https://cert.example.org/cert.msg"),
                        "content/test/data/sxg/404.msg");
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");

  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  histogram_tester_.ExpectUniqueSample(
      "SignedExchange.LoadResult", SignedExchangeLoadResult::kCertFetchError,
      1);
}

class SignedExchangeRequestHandlerRealCertVerifierBrowserTest
    : public SignedExchangeRequestHandlerBrowserTest {
 public:
  SignedExchangeRequestHandlerRealCertVerifierBrowserTest() {
    // Use "real" CertVerifier.
    disable_mock_cert_verifier();
  }
};

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerRealCertVerifierBrowserTest,
                       Basic) {
  InstallUrlInterceptor(
      GURL("https://cert.example.org/cert.msg"),
      "content/test/data/sxg/test.example.org.public.pem.cbor");
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");

  embedded_test_server()->RegisterRequestMonitor(
      base::BindRepeating([](const net::test_server::HttpRequest& request) {
        if (request.relative_url == "/sxg/test.example.org_test.sxg") {
          const auto& accept_value = request.headers.find("accept")->second;
          EXPECT_THAT(accept_value,
                      ::testing::HasSubstr("application/signed-exchange;v=b2"));
        }
      }));
  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");

  // "test.example.org_test.sxg" should pass CertVerifier::Verify() and then
  // fail at SignedExchangeHandler::CheckOCSPStatus() because of the dummy OCSP
  // response.
  // TODO(https://crbug.com/815024): Make this test pass the OCSP check. We'll
  // need to either generate an OCSP response on the fly, or override the OCSP
  // verification time.
  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  // Verify that it failed at the OCSP check step.
  histogram_tester_.ExpectUniqueSample("SignedExchange.LoadResult",
                                       SignedExchangeLoadResult::kOCSPError, 1);
}

}  // namespace content
