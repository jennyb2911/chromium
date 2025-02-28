// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_WEB_PACKAGE_SIGNED_EXCHANGE_SIGNATURE_VERIFIER_H_
#define CONTENT_BROWSER_WEB_PACKAGE_SIGNED_EXCHANGE_SIGNATURE_VERIFIER_H_

#include "base/memory/ref_counted.h"
#include "base/optional.h"
#include "content/common/content_export.h"
#include "net/cert/x509_certificate.h"

namespace base {
class Time;
}  // namespace base

namespace content {

class SignedExchangeEnvelope;
class SignedExchangeDevToolsProxy;

// SignedExchangeSignatureVerifier verifies the signature of the given
// signed exchange. This is done by reconstructing the signed message
// and verifying the cryptographic signature enclosed in "Signature" response
// header (given as |input.signature|).
//
// Note that SignedExchangeSignatureVerifier does not ensure the validity
// of the certificate used to generate the signature, which can't be done
// synchronously. (See SignedExchangeCertFetcher for this logic.)
//
// https://wicg.github.io/webpackage/draft-yasskin-httpbis-origin-signed-exchanges-impl.html#signature-validity
class CONTENT_EXPORT SignedExchangeSignatureVerifier final {
 public:
  // This enum is used for recording histograms. Treat as append-only.
  enum class Result {
    kSuccess,
    kErrNoCertificate,
    kErrNoCertificateSHA256,
    kErrCertificateSHA256Mismatch,
    kErrInvalidSignatureFormat,
    kErrSignatureVerificationFailed,
    kErrInvalidSignatureIntegrity,
    kErrInvalidTimestamp,
    kErrUnsupportedCertType,
    kMaxValue = kErrUnsupportedCertType
  };

  static Result Verify(const SignedExchangeEnvelope& envelope,
                       scoped_refptr<net::X509Certificate> certificate,
                       const base::Time& verification_time,
                       SignedExchangeDevToolsProxy* devtools_proxy);
};

}  // namespace content

#endif  // CONTENT_BROWSER_WEB_PACKAGE_SIGNED_EXCHANGE_SIGNATURE_VERIFIER_H_
