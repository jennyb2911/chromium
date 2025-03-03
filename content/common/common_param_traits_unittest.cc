// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/common/common_param_traits.h"

#include <stddef.h>
#include <string.h>

#include <memory>
#include <utility>

#include "base/macros.h"
#include "base/values.h"
#include "components/viz/common/surfaces/surface_info.h"
#include "content/common/resource_messages.h"
#include "content/public/common/content_constants.h"
#include "ipc/ipc_message.h"
#include "ipc/ipc_message_utils.h"
#include "net/base/host_port_pair.h"
#include "net/cert/ct_policy_status.h"
#include "net/ssl/ssl_info.h"
#include "net/test/cert_test_util.h"
#include "net/test/test_data_directory.h"
#include "printing/backend/print_backend.h"
#include "printing/page_range.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/ipc/gfx_param_traits.h"
#include "ui/gfx/ipc/skia/gfx_skia_param_traits.h"

// Tests std::pair serialization
TEST(IPCMessageTest, Pair) {
  typedef std::pair<std::string, std::string> TestPair;

  TestPair input("foo", "bar");
  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  IPC::ParamTraits<TestPair>::Write(&msg, input);

  TestPair output;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(IPC::ParamTraits<TestPair>::Read(&msg, &iter, &output));
  EXPECT_EQ(output.first, "foo");
  EXPECT_EQ(output.second, "bar");
}

// Tests bitmap serialization.
TEST(IPCMessageTest, Bitmap) {
  SkBitmap bitmap;

  bitmap.allocN32Pixels(10, 5);
  memset(bitmap.getPixels(), 'A', bitmap.computeByteSize());

  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  IPC::ParamTraits<SkBitmap>::Write(&msg, bitmap);

  SkBitmap output;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(IPC::ParamTraits<SkBitmap>::Read(&msg, &iter, &output));

  EXPECT_EQ(bitmap.colorType(), output.colorType());
  EXPECT_EQ(bitmap.width(), output.width());
  EXPECT_EQ(bitmap.height(), output.height());
  EXPECT_EQ(bitmap.rowBytes(), output.rowBytes());
  EXPECT_EQ(bitmap.computeByteSize(), output.computeByteSize());
  EXPECT_EQ(
      memcmp(bitmap.getPixels(), output.getPixels(), bitmap.computeByteSize()),
      0);

  // Also test the corrupt case.
  IPC::Message bad_msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  // Copy the first message block over to |bad_msg|.
  const char* fixed_data;
  int fixed_data_size;
  iter = base::PickleIterator(msg);
  EXPECT_TRUE(iter.ReadData(&fixed_data, &fixed_data_size));
  bad_msg.WriteData(fixed_data, fixed_data_size);
  // Add some bogus pixel data.
  const size_t bogus_pixels_size = bitmap.computeByteSize() * 2;
  std::unique_ptr<char[]> bogus_pixels(new char[bogus_pixels_size]);
  memset(bogus_pixels.get(), 'B', bogus_pixels_size);
  bad_msg.WriteData(bogus_pixels.get(), bogus_pixels_size);
  // Make sure we don't read out the bitmap!
  SkBitmap bad_output;
  iter = base::PickleIterator(bad_msg);
  EXPECT_FALSE(IPC::ParamTraits<SkBitmap>::Read(&bad_msg, &iter, &bad_output));
}

TEST(IPCMessageTest, ListValue) {
  base::ListValue input;
  input.AppendDouble(42.42);
  input.AppendString("forty");
  input.Append(std::make_unique<base::Value>());

  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  IPC::WriteParam(&msg, input);

  base::ListValue output;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(IPC::ReadParam(&msg, &iter, &output));

  EXPECT_TRUE(input.Equals(&output));

  // Also test the corrupt case.
  IPC::Message bad_msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  bad_msg.WriteInt(99);
  iter = base::PickleIterator(bad_msg);
  EXPECT_FALSE(IPC::ReadParam(&bad_msg, &iter, &output));
}

TEST(IPCMessageTest, DictionaryValue) {
  base::DictionaryValue input;
  input.Set("null", std::make_unique<base::Value>());
  input.SetBoolean("bool", true);
  input.SetInteger("int", 42);

  auto subdict = std::make_unique<base::DictionaryValue>();
  subdict->SetString("str", "forty two");
  subdict->SetBoolean("bool", false);

  auto sublist = std::make_unique<base::ListValue>();
  sublist->AppendDouble(42.42);
  sublist->AppendString("forty");
  sublist->AppendString("two");
  subdict->Set("list", std::move(sublist));

  input.Set("dict", std::move(subdict));

  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  IPC::WriteParam(&msg, input);

  base::DictionaryValue output;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(IPC::ReadParam(&msg, &iter, &output));

  EXPECT_TRUE(input.Equals(&output));

  // Also test the corrupt case.
  IPC::Message bad_msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  bad_msg.WriteInt(99);
  iter = base::PickleIterator(bad_msg);
  EXPECT_FALSE(IPC::ReadParam(&bad_msg, &iter, &output));
}

// Tests net::HostPortPair serialization
TEST(IPCMessageTest, HostPortPair) {
  net::HostPortPair input("host.com", 12345);

  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  IPC::ParamTraits<net::HostPortPair>::Write(&msg, input);

  net::HostPortPair output;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(IPC::ParamTraits<net::HostPortPair>::Read(&msg, &iter, &output));
  EXPECT_EQ(input.host(), output.host());
  EXPECT_EQ(input.port(), output.port());
}

// Tests net::SSLInfo serialization
TEST(IPCMessageTest, SSLInfo) {
  // Build a SSLInfo. Avoid false for booleans as that's the default value.
  net::SSLInfo in;
  in.cert =
      net::ImportCertFromFile(net::GetTestCertsDirectory(), "ok_cert.pem");
  in.unverified_cert = net::ImportCertFromFile(net::GetTestCertsDirectory(),
                                               "ok_cert_by_intermediate.pem");
  in.cert_status = net::CERT_STATUS_COMMON_NAME_INVALID;
  in.security_bits = 0x100;
  in.key_exchange_group = 1024;
  in.connection_status = 0x300039;  // TLS_DHE_RSA_WITH_AES_256_CBC_SHA
  in.is_issued_by_known_root = true;
  in.pkp_bypassed = true;
  in.client_cert_sent = true;
  in.channel_id_sent = true;
  in.token_binding_negotiated = true;
  in.token_binding_key_param = net::TB_PARAM_ECDSAP256;
  in.handshake_type = net::SSLInfo::HANDSHAKE_FULL;
  const net::SHA256HashValue kCertPublicKeyHashValue = {{0x01, 0x02}};
  in.public_key_hashes.push_back(net::HashValue(kCertPublicKeyHashValue));
  in.pinning_failure_log = "foo";

  scoped_refptr<net::ct::SignedCertificateTimestamp> sct(
      new net::ct::SignedCertificateTimestamp());
  sct->version = net::ct::SignedCertificateTimestamp::V1;
  sct->log_id = "unknown_log_id";
  sct->extensions = "extensions";
  sct->timestamp = base::Time::Now();
  sct->signature.hash_algorithm = net::ct::DigitallySigned::HASH_ALGO_MD5;
  sct->signature.signature_algorithm = net::ct::DigitallySigned::SIG_ALGO_RSA;
  sct->signature.signature_data = "signature";
  sct->origin = net::ct::SignedCertificateTimestamp::SCT_EMBEDDED;
  in.signed_certificate_timestamps.push_back(
      net::SignedCertificateTimestampAndStatus(
          sct, net::ct::SCT_STATUS_LOG_UNKNOWN));

  in.ct_policy_compliance =
      net::ct::CTPolicyCompliance::CT_POLICY_NOT_ENOUGH_SCTS;
  in.ocsp_result.response_status = net::OCSPVerifyResult::PROVIDED;
  in.ocsp_result.revocation_status = net::OCSPRevocationStatus::REVOKED;

  // Now serialize and deserialize.
  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  IPC::ParamTraits<net::SSLInfo>::Write(&msg, in);

  net::SSLInfo out;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(IPC::ParamTraits<net::SSLInfo>::Read(&msg, &iter, &out));

  // Now verify they're equal.
  ASSERT_TRUE(in.cert->EqualsIncludingChain(out.cert.get()));
  ASSERT_TRUE(
      in.unverified_cert->EqualsIncludingChain(out.unverified_cert.get()));
  ASSERT_EQ(in.security_bits, out.security_bits);
  ASSERT_EQ(in.key_exchange_group, out.key_exchange_group);
  ASSERT_EQ(in.connection_status, out.connection_status);
  ASSERT_EQ(in.is_issued_by_known_root, out.is_issued_by_known_root);
  ASSERT_EQ(in.pkp_bypassed, out.pkp_bypassed);
  ASSERT_EQ(in.client_cert_sent, out.client_cert_sent);
  ASSERT_EQ(in.channel_id_sent, out.channel_id_sent);
  ASSERT_EQ(in.token_binding_negotiated, out.token_binding_negotiated);
  ASSERT_EQ(in.token_binding_key_param, out.token_binding_key_param);
  ASSERT_EQ(in.handshake_type, out.handshake_type);
  ASSERT_EQ(in.public_key_hashes, out.public_key_hashes);
  ASSERT_EQ(in.pinning_failure_log, out.pinning_failure_log);

  ASSERT_EQ(in.signed_certificate_timestamps.size(),
            out.signed_certificate_timestamps.size());
  ASSERT_EQ(in.signed_certificate_timestamps[0].status,
            out.signed_certificate_timestamps[0].status);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->version,
            out.signed_certificate_timestamps[0].sct->version);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->log_id,
            out.signed_certificate_timestamps[0].sct->log_id);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->timestamp,
            out.signed_certificate_timestamps[0].sct->timestamp);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->extensions,
            out.signed_certificate_timestamps[0].sct->extensions);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->signature.hash_algorithm,
            out.signed_certificate_timestamps[0].sct->signature.hash_algorithm);
  ASSERT_EQ(
      in.signed_certificate_timestamps[0].sct->signature.signature_algorithm,
      out.signed_certificate_timestamps[0].sct->signature.signature_algorithm);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->signature.signature_data,
            out.signed_certificate_timestamps[0].sct->signature.signature_data);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->origin,
            out.signed_certificate_timestamps[0].sct->origin);
  ASSERT_EQ(in.signed_certificate_timestamps[0].sct->log_description,
            out.signed_certificate_timestamps[0].sct->log_description);

  ASSERT_EQ(in.ct_policy_compliance, out.ct_policy_compliance);
  ASSERT_EQ(in.ocsp_result, out.ocsp_result);
}

TEST(IPCMessageTest, RenderWidgetSurfaceProperties) {
  content::RenderWidgetSurfaceProperties input;
  input.size = gfx::Size(23, 45);
  input.device_scale_factor = 0.8;
  input.top_controls_height = 16.5;
  input.top_controls_shown_ratio = 0.4;
#ifdef OS_ANDROID
  input.bottom_controls_height = 23.4;
  input.bottom_controls_shown_ratio = 0.8;
  input.selection.start.set_type(gfx::SelectionBound::Type::CENTER);
  input.has_transparent_background = true;
#endif

  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  IPC::ParamTraits<content::RenderWidgetSurfaceProperties>::Write(&msg, input);

  content::RenderWidgetSurfaceProperties output;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(IPC::ParamTraits<content::RenderWidgetSurfaceProperties>::Read(
      &msg, &iter, &output));

  EXPECT_EQ(input.size, output.size);
  EXPECT_EQ(input.device_scale_factor, output.device_scale_factor);
  EXPECT_EQ(input.top_controls_height, output.top_controls_height);
  EXPECT_EQ(input.top_controls_shown_ratio, output.top_controls_shown_ratio);
#ifdef OS_ANDROID
  EXPECT_EQ(input.bottom_controls_height, output.bottom_controls_height);
  EXPECT_EQ(input.bottom_controls_shown_ratio,
            output.bottom_controls_shown_ratio);
  EXPECT_EQ(input.selection, output.selection);
  EXPECT_EQ(input.has_transparent_background,
            output.has_transparent_background);
#endif
}

static constexpr viz::FrameSinkId kArbitraryFrameSinkId(1, 1);

TEST(IPCMessageTest, SurfaceInfo) {
  IPC::Message msg(1, 2, IPC::Message::PRIORITY_NORMAL);
  const viz::SurfaceId kArbitrarySurfaceId(
      kArbitraryFrameSinkId,
      viz::LocalSurfaceId(3, base::UnguessableToken::Create()));
  constexpr float kArbitraryDeviceScaleFactor = 0.9f;
  const gfx::Size kArbitrarySize(65, 321);
  const viz::SurfaceInfo surface_info_in(
      kArbitrarySurfaceId, kArbitraryDeviceScaleFactor, kArbitrarySize);
  IPC::ParamTraits<viz::SurfaceInfo>::Write(&msg, surface_info_in);

  viz::SurfaceInfo surface_info_out;
  base::PickleIterator iter(msg);
  EXPECT_TRUE(
      IPC::ParamTraits<viz::SurfaceInfo>::Read(&msg, &iter, &surface_info_out));

  ASSERT_EQ(surface_info_in, surface_info_out);
}
