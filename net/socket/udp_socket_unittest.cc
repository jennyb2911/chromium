// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/udp_socket.h"

#include <algorithm>

#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/network_interfaces.h"
#include "net/base/test_completion_callback.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_source.h"
#include "net/log/test_net_log.h"
#include "net/log/test_net_log_entry.h"
#include "net/log/test_net_log_util.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/udp_client_socket.h"
#include "net/socket/udp_server_socket.h"
#include "net/test/gtest_util.h"
#include "net/test/test_with_scoped_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"

#if defined(OS_ANDROID)
#include "base/android/build_info.h"
#include "net/android/network_change_notifier_factory_android.h"
#include "net/base/network_change_notifier.h"
#endif

#if defined(OS_IOS)
#include <TargetConditionals.h>
#endif

using net::test::IsError;
using net::test::IsOk;
using testing::Not;

namespace net {

namespace {

// Creates an address from ip address and port and writes it to |*address|.
bool CreateUDPAddress(const std::string& ip_str,
                      uint16_t port,
                      IPEndPoint* address) {
  IPAddress ip_address;
  if (!ip_address.AssignFromIPLiteral(ip_str))
    return false;

  *address = IPEndPoint(ip_address, port);
  return true;
}

class UDPSocketTest : public PlatformTest, public WithScopedTaskEnvironment {
 public:
  UDPSocketTest() : buffer_(base::MakeRefCounted<IOBufferWithSize>(kMaxRead)) {}

  // Blocks until data is read from the socket.
  std::string RecvFromSocket(UDPServerSocket* socket) {
    TestCompletionCallback callback;

    int rv = socket->RecvFrom(
        buffer_.get(), kMaxRead, &recv_from_address_, callback.callback());
    rv = callback.GetResult(rv);
    if (rv < 0)
      return std::string();
    return std::string(buffer_->data(), rv);
  }

  // Sends UDP packet.
  // If |address| is specified, then it is used for the destination
  // to send to. Otherwise, will send to the last socket this server
  // received from.
  int SendToSocket(UDPServerSocket* socket, const std::string& msg) {
    return SendToSocket(socket, msg, recv_from_address_);
  }

  int SendToSocket(UDPServerSocket* socket,
                   std::string msg,
                   const IPEndPoint& address) {
    scoped_refptr<StringIOBuffer> io_buffer =
        base::MakeRefCounted<StringIOBuffer>(msg);
    TestCompletionCallback callback;
    int rv = socket->SendTo(io_buffer.get(), io_buffer->size(), address,
                            callback.callback());
    return callback.GetResult(rv);
  }

  std::string ReadSocket(UDPClientSocket* socket) {
    TestCompletionCallback callback;

    int rv = socket->Read(buffer_.get(), kMaxRead, callback.callback());
    rv = callback.GetResult(rv);
    if (rv < 0)
      return std::string();
    return std::string(buffer_->data(), rv);
  }

  // Writes specified message to the socket.
  int WriteSocket(UDPClientSocket* socket, const std::string& msg) {
    scoped_refptr<StringIOBuffer> io_buffer =
        base::MakeRefCounted<StringIOBuffer>(msg);
    TestCompletionCallback callback;
    int rv = socket->Write(io_buffer.get(), io_buffer->size(),
                           callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS);
    return callback.GetResult(rv);
  }

  void WriteSocketIgnoreResult(UDPClientSocket* socket,
                               const std::string& msg) {
    WriteSocket(socket, msg);
  }

  // And again for a bare socket
  int SendToSocket(UDPSocket* socket,
                   std::string msg,
                   const IPEndPoint& address) {
    scoped_refptr<StringIOBuffer> io_buffer = new StringIOBuffer(msg);
    TestCompletionCallback callback;
    int rv = socket->SendTo(io_buffer.get(), io_buffer->size(), address,
                            callback.callback());
    return callback.GetResult(rv);
  }

  // Run unit test for a connection test.
  // |use_nonblocking_io| is used to switch between overlapped and non-blocking
  // IO on Windows. It has no effect in other ports.
  void ConnectTest(bool use_nonblocking_io);

 protected:
  static const int kMaxRead = 1024;
  scoped_refptr<IOBufferWithSize> buffer_;
  IPEndPoint recv_from_address_;
};

const int UDPSocketTest::kMaxRead;

void ReadCompleteCallback(int* result_out, base::Closure callback, int result) {
  *result_out = result;
  callback.Run();
}

void UDPSocketTest::ConnectTest(bool use_nonblocking_io) {
  const uint16_t kPort = 9999;
  std::string simple_message("hello world!");

  // Setup the server to listen.
  IPEndPoint server_address(IPAddress::IPv4Localhost(), kPort);
  TestNetLog server_log;
  std::unique_ptr<UDPServerSocket> server(
      new UDPServerSocket(&server_log, NetLogSource()));
  if (use_nonblocking_io)
    server->UseNonBlockingIO();
  server->AllowAddressReuse();
  int rv = server->Listen(server_address);
  ASSERT_THAT(rv, IsOk());

  // Setup the client.
  TestNetLog client_log;
  auto client = std::make_unique<UDPClientSocket>(DatagramSocket::DEFAULT_BIND,
                                                  &client_log, NetLogSource());
  if (use_nonblocking_io)
    client->UseNonBlockingIO();

  rv = client->Connect(server_address);
  EXPECT_THAT(rv, IsOk());

  // Client sends to the server.
  rv = WriteSocket(client.get(), simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // Server waits for message.
  std::string str = RecvFromSocket(server.get());
  EXPECT_EQ(simple_message, str);

  // Server echoes reply.
  rv = SendToSocket(server.get(), simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // Client waits for response.
  str = ReadSocket(client.get());
  EXPECT_EQ(simple_message, str);

  // Test asynchronous read. Server waits for message.
  base::RunLoop run_loop;
  int read_result = 0;
  rv = server->RecvFrom(
      buffer_.get(), kMaxRead, &recv_from_address_,
      base::Bind(&ReadCompleteCallback, &read_result, run_loop.QuitClosure()));
  EXPECT_THAT(rv, IsError(ERR_IO_PENDING));

  // Client sends to the server.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::Bind(&UDPSocketTest::WriteSocketIgnoreResult,
                 base::Unretained(this), client.get(), simple_message));
  run_loop.Run();
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(read_result));
  EXPECT_EQ(simple_message, std::string(buffer_->data(), read_result));

  // Delete sockets so they log their final events.
  server.reset();
  client.reset();

  // Check the server's log.
  TestNetLogEntry::List server_entries;
  server_log.GetEntries(&server_entries);
  EXPECT_EQ(5u, server_entries.size());
  EXPECT_TRUE(
      LogContainsBeginEvent(server_entries, 0, NetLogEventType::SOCKET_ALIVE));
  EXPECT_TRUE(LogContainsEvent(server_entries, 1,
                               NetLogEventType::UDP_BYTES_RECEIVED,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(LogContainsEvent(server_entries, 2,
                               NetLogEventType::UDP_BYTES_SENT,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(LogContainsEvent(server_entries, 3,
                               NetLogEventType::UDP_BYTES_RECEIVED,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(
      LogContainsEndEvent(server_entries, 4, NetLogEventType::SOCKET_ALIVE));

  // Check the client's log.
  TestNetLogEntry::List client_entries;
  client_log.GetEntries(&client_entries);
  EXPECT_EQ(7u, client_entries.size());
  EXPECT_TRUE(
      LogContainsBeginEvent(client_entries, 0, NetLogEventType::SOCKET_ALIVE));
  EXPECT_TRUE(
      LogContainsBeginEvent(client_entries, 1, NetLogEventType::UDP_CONNECT));
  EXPECT_TRUE(
      LogContainsEndEvent(client_entries, 2, NetLogEventType::UDP_CONNECT));
  EXPECT_TRUE(LogContainsEvent(client_entries, 3,
                               NetLogEventType::UDP_BYTES_SENT,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(LogContainsEvent(client_entries, 4,
                               NetLogEventType::UDP_BYTES_RECEIVED,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(LogContainsEvent(client_entries, 5,
                               NetLogEventType::UDP_BYTES_SENT,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(
      LogContainsEndEvent(client_entries, 6, NetLogEventType::SOCKET_ALIVE));
}

TEST_F(UDPSocketTest, Connect) {
  // The variable |use_nonblocking_io| has no effect in non-Windows ports.
  ConnectTest(false);
}

#if defined(OS_WIN)
TEST_F(UDPSocketTest, ConnectNonBlocking) {
  ConnectTest(true);
}
#endif

TEST_F(UDPSocketTest, PartialRecv) {
  UDPServerSocket server_socket(nullptr, NetLogSource());
  ASSERT_THAT(server_socket.Listen(IPEndPoint(IPAddress::IPv4Localhost(), 0)),
              IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server_socket.GetLocalAddress(&server_address), IsOk());

  UDPClientSocket client_socket(DatagramSocket::DEFAULT_BIND, nullptr,
                                NetLogSource());
  ASSERT_THAT(client_socket.Connect(server_address), IsOk());

  std::string test_packet("hello world!");
  ASSERT_EQ(static_cast<int>(test_packet.size()),
            WriteSocket(&client_socket, test_packet));

  TestCompletionCallback recv_callback;

  // Read just 2 bytes. Read() is expected to return the first 2 bytes from the
  // packet and discard the rest.
  const int kPartialReadSize = 2;
  scoped_refptr<IOBuffer> buffer =
      base::MakeRefCounted<IOBuffer>(kPartialReadSize);
  int rv =
      server_socket.RecvFrom(buffer.get(), kPartialReadSize,
                             &recv_from_address_, recv_callback.callback());
  rv = recv_callback.GetResult(rv);

  EXPECT_EQ(rv, ERR_MSG_TOO_BIG);

  // Send a different message again.
  std::string second_packet("Second packet");
  ASSERT_EQ(static_cast<int>(second_packet.size()),
            WriteSocket(&client_socket, second_packet));

  // Read whole packet now.
  std::string received = RecvFromSocket(&server_socket);
  EXPECT_EQ(second_packet, received);
}

#if defined(OS_MACOSX) || defined(OS_ANDROID) || defined(OS_FUCHSIA) || \
    defined(OS_CHROMEOS)
// - MacOS: requires root permissions on OSX 10.7+.
// - Android: devices attached to testbots don't have default network, so
// broadcasting to 255.255.255.255 returns error -109 (Address not reachable).
// crbug.com/139144.
// - Fuchsia: TODO(fuchsia): broadcast support is not implemented yet.
// - ChromeOS: QEMU's user-mode networking doesn't handle broadcasts.
//   https://crbug.com/852590
#define MAYBE_LocalBroadcast DISABLED_LocalBroadcast
#else
#define MAYBE_LocalBroadcast LocalBroadcast
#endif
TEST_F(UDPSocketTest, MAYBE_LocalBroadcast) {
  const uint16_t kPort = 9999;
  std::string first_message("first message"), second_message("second message");

  IPEndPoint broadcast_address;
  ASSERT_TRUE(CreateUDPAddress("255.255.255.255", kPort, &broadcast_address));
  IPEndPoint listen_address;
  ASSERT_TRUE(CreateUDPAddress("0.0.0.0", kPort, &listen_address));

  TestNetLog server1_log, server2_log;
  std::unique_ptr<UDPServerSocket> server1(
      new UDPServerSocket(&server1_log, NetLogSource()));
  std::unique_ptr<UDPServerSocket> server2(
      new UDPServerSocket(&server2_log, NetLogSource()));
  server1->AllowAddressReuse();
  server1->AllowBroadcast();
  server2->AllowAddressReuse();
  server2->AllowBroadcast();

  int rv = server1->Listen(listen_address);
  EXPECT_THAT(rv, IsOk());
  rv = server2->Listen(listen_address);
  EXPECT_THAT(rv, IsOk());

  rv = SendToSocket(server1.get(), first_message, broadcast_address);
  ASSERT_EQ(static_cast<int>(first_message.size()), rv);
  std::string str = RecvFromSocket(server1.get());
  ASSERT_EQ(first_message, str);
  str = RecvFromSocket(server2.get());
  ASSERT_EQ(first_message, str);

  rv = SendToSocket(server2.get(), second_message, broadcast_address);
  ASSERT_EQ(static_cast<int>(second_message.size()), rv);
  str = RecvFromSocket(server1.get());
  ASSERT_EQ(second_message, str);
  str = RecvFromSocket(server2.get());
  ASSERT_EQ(second_message, str);
}

// ConnectRandomBind verifies RANDOM_BIND is handled correctly. It connects
// 1000 sockets and then verifies that the allocated port numbers satisfy the
// following 2 conditions:
//  1. Range from min port value to max is greater than 10000.
//  2. There is at least one port in the 5 buckets in the [min, max] range.
//
// These conditions are not enough to verify that the port numbers are truly
// random, but they are enough to protect from most common non-random port
// allocation strategies (e.g. counter, pool of available ports, etc.) False
// positive result is theoretically possible, but its probability is negligible.
TEST_F(UDPSocketTest, ConnectRandomBind) {
  const int kIterations = 1000;

  std::vector<int> used_ports;
  for (int i = 0; i < kIterations; ++i) {
    UDPClientSocket socket(DatagramSocket::RANDOM_BIND, nullptr,
                           NetLogSource());
    EXPECT_THAT(socket.Connect(IPEndPoint(IPAddress::IPv4Localhost(), 53)),
                IsOk());

    IPEndPoint client_address;
    EXPECT_THAT(socket.GetLocalAddress(&client_address), IsOk());
    used_ports.push_back(client_address.port());
  }

  int min_port = *std::min_element(used_ports.begin(), used_ports.end());
  int max_port = *std::max_element(used_ports.begin(), used_ports.end());
  int range = max_port - min_port + 1;

  // Verify that the range of ports used by the random port allocator is wider
  // than 10k. Assuming that socket implementation limits port range to 16k
  // ports (default on Fuchsia) probability of false negative is below
  // 10^-200.
  static int kMinRange = 10000;
  EXPECT_GT(range, kMinRange);

  static int kBuckets = 5;
  std::vector<int> bucket_sizes(kBuckets, 0);
  for (int port : used_ports) {
    bucket_sizes[(port - min_port) * kBuckets / range] += 1;
  }

  // Verify that there is at least one value in each bucket. Probability of
  // false negative is below (kBuckets * (1 - 1 / kBuckets) ^ kIterations),
  // which is less than 10^-96.
  for (int size : bucket_sizes) {
    EXPECT_GT(size, 0);
  }
}

#if defined(OS_FUCHSIA)
// Currently the test fails on Fuchsia because netstack allows to connect IPv4
// socket to IPv6 address. This issue is tracked by NET-596.
#define MAYBE_ConnectFail DISABLED_ConnectFail
#else
#define MAYBE_ConnectFail ConnectFail
#endif
TEST_F(UDPSocketTest, MAYBE_ConnectFail) {
  UDPSocket socket(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());

  EXPECT_THAT(socket.Open(ADDRESS_FAMILY_IPV4), IsOk());

  // Connect to an IPv6 address should fail since the socket was created for
  // IPv4.
  EXPECT_THAT(socket.Connect(net::IPEndPoint(IPAddress::IPv6Localhost(), 53)),
              Not(IsOk()));

  // Make sure that UDPSocket actually closed the socket.
  EXPECT_FALSE(socket.is_connected());
}

// In this test, we verify that connect() on a socket will have the effect
// of filtering reads on this socket only to data read from the destination
// we connected to.
//
// The purpose of this test is that some documentation indicates that connect
// binds the client's sends to send to a particular server endpoint, but does
// not bind the client's reads to only be from that endpoint, and that we need
// to always use recvfrom() to disambiguate.
TEST_F(UDPSocketTest, VerifyConnectBindsAddr) {
  const uint16_t kPort1 = 9999;
  const uint16_t kPort2 = 10000;
  std::string simple_message("hello world!");
  std::string foreign_message("BAD MESSAGE TO GET!!");

  // Setup the first server to listen.
  IPEndPoint server1_address(IPAddress::IPv4Localhost(), kPort1);
  UDPServerSocket server1(nullptr, NetLogSource());
  server1.AllowAddressReuse();
  int rv = server1.Listen(server1_address);
  ASSERT_THAT(rv, IsOk());

  // Setup the second server to listen.
  IPEndPoint server2_address(IPAddress::IPv4Localhost(), kPort2);
  UDPServerSocket server2(nullptr, NetLogSource());
  server2.AllowAddressReuse();
  rv = server2.Listen(server2_address);
  ASSERT_THAT(rv, IsOk());

  // Setup the client, connected to server 1.
  UDPClientSocket client(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  rv = client.Connect(server1_address);
  EXPECT_THAT(rv, IsOk());

  // Client sends to server1.
  rv = WriteSocket(&client, simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // Server1 waits for message.
  std::string str = RecvFromSocket(&server1);
  EXPECT_EQ(simple_message, str);

  // Get the client's address.
  IPEndPoint client_address;
  rv = client.GetLocalAddress(&client_address);
  EXPECT_THAT(rv, IsOk());

  // Server2 sends reply.
  rv = SendToSocket(&server2, foreign_message,
                    client_address);
  EXPECT_EQ(foreign_message.length(), static_cast<size_t>(rv));

  // Server1 sends reply.
  rv = SendToSocket(&server1, simple_message,
                    client_address);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // Client waits for response.
  str = ReadSocket(&client);
  EXPECT_EQ(simple_message, str);
}

TEST_F(UDPSocketTest, ClientGetLocalPeerAddresses) {
  struct TestData {
    std::string remote_address;
    std::string local_address;
    bool may_fail;
  } tests[] = {
    { "127.0.00.1", "127.0.0.1", false },
    { "::1", "::1", true },
#if !defined(OS_ANDROID) && !defined(OS_IOS)
    // Addresses below are disabled on Android. See crbug.com/161248
    // They are also disabled on iOS. See https://crbug.com/523225
    { "192.168.1.1", "127.0.0.1", false },
    { "2001:db8:0::42", "::1", true },
#endif
  };
  for (size_t i = 0; i < arraysize(tests); i++) {
    SCOPED_TRACE(std::string("Connecting from ") +  tests[i].local_address +
                 std::string(" to ") + tests[i].remote_address);

    IPAddress ip_address;
    EXPECT_TRUE(ip_address.AssignFromIPLiteral(tests[i].remote_address));
    IPEndPoint remote_address(ip_address, 80);
    EXPECT_TRUE(ip_address.AssignFromIPLiteral(tests[i].local_address));
    IPEndPoint local_address(ip_address, 80);

    UDPClientSocket client(DatagramSocket::DEFAULT_BIND, nullptr,
                           NetLogSource());
    int rv = client.Connect(remote_address);
    if (tests[i].may_fail && rv == ERR_ADDRESS_UNREACHABLE) {
      // Connect() may return ERR_ADDRESS_UNREACHABLE for IPv6
      // addresses if IPv6 is not configured.
      continue;
    }

    EXPECT_LE(ERR_IO_PENDING, rv);

    IPEndPoint fetched_local_address;
    rv = client.GetLocalAddress(&fetched_local_address);
    EXPECT_THAT(rv, IsOk());

    // TODO(mbelshe): figure out how to verify the IP and port.
    //                The port is dynamically generated by the udp stack.
    //                The IP is the real IP of the client, not necessarily
    //                loopback.
    //EXPECT_EQ(local_address.address(), fetched_local_address.address());

    IPEndPoint fetched_remote_address;
    rv = client.GetPeerAddress(&fetched_remote_address);
    EXPECT_THAT(rv, IsOk());

    EXPECT_EQ(remote_address, fetched_remote_address);
  }
}

TEST_F(UDPSocketTest, ServerGetLocalAddress) {
  IPEndPoint bind_address(IPAddress::IPv4Localhost(), 0);
  UDPServerSocket server(NULL, NetLogSource());
  int rv = server.Listen(bind_address);
  EXPECT_THAT(rv, IsOk());

  IPEndPoint local_address;
  rv = server.GetLocalAddress(&local_address);
  EXPECT_EQ(rv, 0);

  // Verify that port was allocated.
  EXPECT_GT(local_address.port(), 0);
  EXPECT_EQ(local_address.address(), bind_address.address());
}

TEST_F(UDPSocketTest, ServerGetPeerAddress) {
  IPEndPoint bind_address(IPAddress::IPv4Localhost(), 0);
  UDPServerSocket server(NULL, NetLogSource());
  int rv = server.Listen(bind_address);
  EXPECT_THAT(rv, IsOk());

  IPEndPoint peer_address;
  rv = server.GetPeerAddress(&peer_address);
  EXPECT_EQ(rv, ERR_SOCKET_NOT_CONNECTED);
}

TEST_F(UDPSocketTest, ClientSetDoNotFragment) {
  for (std::string ip : {"127.0.0.1", "::1"}) {
    UDPClientSocket client(DatagramSocket::DEFAULT_BIND, nullptr,
                           NetLogSource());
    IPAddress ip_address;
    EXPECT_TRUE(ip_address.AssignFromIPLiteral(ip));
    IPEndPoint remote_address(ip_address, 80);
    int rv = client.Connect(remote_address);
    // May fail on IPv6 is IPv6 is not configured.
    if (ip_address.IsIPv6() && rv == ERR_ADDRESS_UNREACHABLE)
      return;
    EXPECT_THAT(rv, IsOk());

#if defined(OS_MACOSX)
    EXPECT_EQ(ERR_NOT_IMPLEMENTED, client.SetDoNotFragment());
#else
    rv = client.SetDoNotFragment();
    EXPECT_THAT(rv, IsOk());
#endif
  }
}

TEST_F(UDPSocketTest, ServerSetDoNotFragment) {
  for (std::string ip : {"127.0.0.1", "::1"}) {
    IPEndPoint bind_address;
    ASSERT_TRUE(CreateUDPAddress(ip, 0, &bind_address));
    UDPServerSocket server(nullptr, NetLogSource());
    int rv = server.Listen(bind_address);
    // May fail on IPv6 is IPv6 is not configure
    if (bind_address.address().IsIPv6() &&
        (rv == ERR_ADDRESS_INVALID || rv == ERR_ADDRESS_UNREACHABLE))
      return;
    EXPECT_THAT(rv, IsOk());

#if defined(OS_MACOSX)
    EXPECT_EQ(ERR_NOT_IMPLEMENTED, server.SetDoNotFragment());
#else
    rv = server.SetDoNotFragment();
    EXPECT_THAT(rv, IsOk());
#endif
  }
}

// Close the socket while read is pending.
TEST_F(UDPSocketTest, CloseWithPendingRead) {
  IPEndPoint bind_address(IPAddress::IPv4Localhost(), 0);
  UDPServerSocket server(NULL, NetLogSource());
  int rv = server.Listen(bind_address);
  EXPECT_THAT(rv, IsOk());

  TestCompletionCallback callback;
  IPEndPoint from;
  rv = server.RecvFrom(buffer_.get(), kMaxRead, &from, callback.callback());
  EXPECT_EQ(rv, ERR_IO_PENDING);

  server.Close();

  EXPECT_FALSE(callback.have_result());
}

#if defined(OS_ANDROID)
// Some Android devices do not support multicast socket.
// The ones supporting multicast need WifiManager.MulitcastLock to enable it.
// http://goo.gl/jjAk9
#define MAYBE_JoinMulticastGroup DISABLED_JoinMulticastGroup
#else
#define MAYBE_JoinMulticastGroup JoinMulticastGroup
#endif  // defined(OS_ANDROID)

TEST_F(UDPSocketTest, MAYBE_JoinMulticastGroup) {
  const uint16_t kPort = 9999;
  const char kGroup[] = "237.132.100.17";

  IPEndPoint bind_address;
  ASSERT_TRUE(CreateUDPAddress("0.0.0.0", kPort, &bind_address));
  IPAddress group_ip;
  EXPECT_TRUE(group_ip.AssignFromIPLiteral(kGroup));

  UDPSocket socket(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  EXPECT_THAT(socket.Open(bind_address.GetFamily()), IsOk());

#if defined(OS_FUCHSIA)
  // Fuchsia currently doesn't support automatic interface selection for
  // multicast, so interface index needs to be set explicitly.
  // See https://fuchsia.atlassian.net/browse/NET-195 .
  NetworkInterfaceList interfaces;
  ASSERT_TRUE(GetNetworkList(&interfaces, 0));
  ASSERT_FALSE(interfaces.empty());
  EXPECT_THAT(socket.SetMulticastInterface(interfaces[0].interface_index),
              IsOk());
#endif  // defined(OS_FUCHSIA)

  EXPECT_THAT(socket.Bind(bind_address), IsOk());
  EXPECT_THAT(socket.JoinGroup(group_ip), IsOk());
  // Joining group multiple times.
  EXPECT_NE(OK, socket.JoinGroup(group_ip));
  EXPECT_THAT(socket.LeaveGroup(group_ip), IsOk());
  // Leaving group multiple times.
  EXPECT_NE(OK, socket.LeaveGroup(group_ip));

  socket.Close();
}

TEST_F(UDPSocketTest, MulticastOptions) {
  const uint16_t kPort = 9999;
  IPEndPoint bind_address;
  ASSERT_TRUE(CreateUDPAddress("0.0.0.0", kPort, &bind_address));

  UDPSocket socket(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  // Before binding.
  EXPECT_THAT(socket.SetMulticastLoopbackMode(false), IsOk());
  EXPECT_THAT(socket.SetMulticastLoopbackMode(true), IsOk());
  EXPECT_THAT(socket.SetMulticastTimeToLive(0), IsOk());
  EXPECT_THAT(socket.SetMulticastTimeToLive(3), IsOk());
  EXPECT_NE(OK, socket.SetMulticastTimeToLive(-1));
  EXPECT_THAT(socket.SetMulticastInterface(0), IsOk());

  EXPECT_THAT(socket.Open(bind_address.GetFamily()), IsOk());
  EXPECT_THAT(socket.Bind(bind_address), IsOk());

  EXPECT_NE(OK, socket.SetMulticastLoopbackMode(false));
  EXPECT_NE(OK, socket.SetMulticastTimeToLive(0));
  EXPECT_NE(OK, socket.SetMulticastInterface(0));

  socket.Close();
}

// Checking that DSCP bits are set correctly is difficult,
// but let's check that the code doesn't crash at least.
TEST_F(UDPSocketTest, SetDSCP) {
  // Setup the server to listen.
  IPEndPoint bind_address;
  UDPSocket client(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  // We need a real IP, but we won't actually send anything to it.
  ASSERT_TRUE(CreateUDPAddress("8.8.8.8", 9999, &bind_address));
  int rv = client.Open(bind_address.GetFamily());
  EXPECT_THAT(rv, IsOk());

  rv = client.Connect(bind_address);
  if (rv != OK) {
    // Let's try localhost then.
    bind_address = IPEndPoint(IPAddress::IPv4Localhost(), 9999);
    rv = client.Connect(bind_address);
  }
  EXPECT_THAT(rv, IsOk());

  client.SetDiffServCodePoint(DSCP_NO_CHANGE);
  client.SetDiffServCodePoint(DSCP_AF41);
  client.SetDiffServCodePoint(DSCP_DEFAULT);
  client.SetDiffServCodePoint(DSCP_CS2);
  client.SetDiffServCodePoint(DSCP_NO_CHANGE);
  client.SetDiffServCodePoint(DSCP_DEFAULT);
  client.Close();
}

TEST_F(UDPSocketTest, TestBindToNetwork) {
  UDPSocket socket(DatagramSocket::RANDOM_BIND, nullptr, NetLogSource());
#if defined(OS_ANDROID)
  NetworkChangeNotifierFactoryAndroid ncn_factory;
  NetworkChangeNotifier::DisableForTest ncn_disable_for_test;
  std::unique_ptr<NetworkChangeNotifier> ncn(ncn_factory.CreateInstance());
#endif
  ASSERT_EQ(OK, socket.Open(ADDRESS_FAMILY_IPV4));
  // Test unsuccessful binding, by attempting to bind to a bogus NetworkHandle.
  int rv = socket.BindToNetwork(65536);
#if !defined(OS_ANDROID)
  EXPECT_EQ(ERR_NOT_IMPLEMENTED, rv);
#else
  if (base::android::BuildInfo::GetInstance()->sdk_int() <
      base::android::SDK_VERSION_LOLLIPOP) {
    EXPECT_EQ(ERR_NOT_IMPLEMENTED, rv);
  } else if (base::android::BuildInfo::GetInstance()->sdk_int() >=
             base::android::SDK_VERSION_LOLLIPOP &&
             base::android::BuildInfo::GetInstance()->sdk_int() <
             base::android::SDK_VERSION_MARSHMALLOW) {
    // On Lollipop, we assume if the user has a NetworkHandle that they must
    // have gotten it from a legitimate source, so if binding to the network
    // fails it's assumed to be because the network went away so
    // ERR_NETWORK_CHANGED is returned. In this test the network never existed
    // anyhow.  ConnectivityService.MAX_NET_ID is 65535, so 65536 won't be used.
    EXPECT_EQ(ERR_NETWORK_CHANGED, rv);
  } else if (base::android::BuildInfo::GetInstance()->sdk_int() >=
             base::android::SDK_VERSION_MARSHMALLOW) {
    // On Marshmallow and newer releases, the NetworkHandle is munged by
    // Network.getNetworkHandle() and 65536 isn't munged so it's rejected.
    EXPECT_EQ(ERR_INVALID_ARGUMENT, rv);
  }

  if (base::android::BuildInfo::GetInstance()->sdk_int() >=
      base::android::SDK_VERSION_LOLLIPOP) {
    EXPECT_EQ(
        ERR_INVALID_ARGUMENT,
        socket.BindToNetwork(NetworkChangeNotifier::kInvalidNetworkHandle));

    // Test successful binding, if possible.
    EXPECT_TRUE(NetworkChangeNotifier::AreNetworkHandlesSupported());
    NetworkChangeNotifier::NetworkHandle network_handle =
        NetworkChangeNotifier::GetDefaultNetwork();
    if (network_handle != NetworkChangeNotifier::kInvalidNetworkHandle) {
      EXPECT_EQ(OK, socket.BindToNetwork(network_handle));
    }
  }
#endif
}

}  // namespace

#if defined(OS_WIN)

namespace {

const HANDLE kFakeHandle = (HANDLE)19;
const QOS_FLOWID kFakeFlowId1 = (QOS_FLOWID)27;
const QOS_FLOWID kFakeFlowId2 = (QOS_FLOWID)38;

class TestUDPSocketWin : public UDPSocketWin {
 public:
  TestUDPSocketWin(QwaveAPI& qos,
                   DatagramSocket::BindType bind_type,
                   net::NetLog* net_log,
                   const net::NetLogSource& source)
      : UDPSocketWin(bind_type, net_log, source), qos_(qos) {}

  // Overriding GetQwaveAPI causes the test class to use the injected mock
  // QwaveAPI instance instead of the singleton.  Ensure close is called in the
  // child destructor before our mock CloseHandle is uninstalled.
  ~TestUDPSocketWin() override { UDPSocketWin::Close(); }

  QwaveAPI& GetQwaveAPI() override { return qos_; }

 private:
  QwaveAPI& qos_;

  DISALLOW_COPY_AND_ASSIGN(TestUDPSocketWin);
};

class MockQwaveAPI : public QwaveAPI {
 public:
  MOCK_METHOD2(CreateHandle, BOOL(PQOS_VERSION version, PHANDLE handle));
  MOCK_METHOD1(CloseHandle, BOOL(HANDLE handle));

  MOCK_METHOD6(AddSocketToFlow,
               BOOL(HANDLE handle,
                    SOCKET socket,
                    PSOCKADDR addr,
                    QOS_TRAFFIC_TYPE traffic_type,
                    DWORD flags,
                    PQOS_FLOWID flow_id));

  MOCK_METHOD4(
      RemoveSocketFromFlow,
      BOOL(HANDLE handle, SOCKET socket, QOS_FLOWID flow_id, DWORD reserved));
  MOCK_METHOD7(SetFlow,
               BOOL(HANDLE handle,
                    QOS_FLOWID flow_id,
                    QOS_SET_FLOW op,
                    ULONG size,
                    PVOID data,
                    DWORD reserved,
                    LPOVERLAPPED overlapped));
};

std::unique_ptr<UDPSocket> OpenedDscpTestClient(QwaveAPI& qos,
                                                IPEndPoint bind_address) {
  auto client = std::make_unique<TestUDPSocketWin>(
      qos, DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  int rv = client->Open(bind_address.GetFamily());
  EXPECT_THAT(rv, IsOk());

  return client;
}

std::unique_ptr<UDPSocket> ConnectedDscpTestClient(QwaveAPI& qos) {
  IPEndPoint bind_address;
  // We need a real IP, but we won't actually send anything to it.
  EXPECT_TRUE(CreateUDPAddress("8.8.8.8", 9999, &bind_address));
  auto client = OpenedDscpTestClient(qos, bind_address);
  EXPECT_THAT(client->Connect(bind_address), IsOk());
  return client;
}

std::unique_ptr<UDPSocket> UnconnectedDscpTestClient(QwaveAPI& qos) {
  IPEndPoint bind_address;
  EXPECT_TRUE(CreateUDPAddress("0.0.0.0", 9999, &bind_address));
  auto client = OpenedDscpTestClient(qos, bind_address);
  EXPECT_THAT(client->Bind(bind_address), IsOk());
  return client;
}

}  // namespace

using ::testing::_;
using ::testing::Return;
using ::testing::SetArgPointee;

TEST_F(UDPSocketTest, SetDSCPNoopIfPassedNoChange) {
  MockQwaveAPI qos;
  std::unique_ptr<UDPSocket> client = ConnectedDscpTestClient(qos);
  EXPECT_THAT(client->SetDiffServCodePoint(DSCP_NO_CHANGE), IsOk());
}

TEST_F(UDPSocketTest, SetDSCPFailsIfQOSHandleCanNotBeCreated) {
  MockQwaveAPI qos;
  std::unique_ptr<UDPSocket> client = ConnectedDscpTestClient(qos);

  EXPECT_CALL(qos, CreateHandle(_, _)).WillOnce(Return(false));
  EXPECT_EQ(ERROR_NOT_SUPPORTED, client->SetDiffServCodePoint(DSCP_AF41));
}

MATCHER_P(DscpPointee, dscp, "") {
  return *(DWORD*)arg == (DWORD)dscp;
}

TEST_F(UDPSocketTest, SetDSCPCallsQwaveFunctions) {
  MockQwaveAPI qos;
  std::unique_ptr<UDPSocket> client = ConnectedDscpTestClient(qos);

  EXPECT_CALL(qos, CreateHandle(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(kFakeHandle), Return(true)));
  // AddSocketToFlow also sets flow_id, but we don't use that here
  EXPECT_CALL(qos, AddSocketToFlow(_, _, _, QOSTrafficTypeAudioVideo, _, _))
      .WillOnce(Return(true));
  EXPECT_CALL(qos, SetFlow(_, _, QOSSetOutgoingDSCPValue, _,
                           DscpPointee(DSCP_AF41), _, _));
  EXPECT_THAT(client->SetDiffServCodePoint(DSCP_AF41), IsOk());
  EXPECT_CALL(qos, CloseHandle(kFakeHandle));
}

TEST_F(UDPSocketTest, SecondSetDSCPCallsQwaveFunctions) {
  MockQwaveAPI qos;
  std::unique_ptr<UDPSocket> client = ConnectedDscpTestClient(qos);

  EXPECT_CALL(qos, CreateHandle(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(kFakeHandle), Return(true)));

  EXPECT_CALL(qos, AddSocketToFlow(_, _, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<5>(kFakeFlowId1), Return(true)));
  EXPECT_CALL(qos, SetFlow(_, _, _, _, _, _, _));
  EXPECT_THAT(client->SetDiffServCodePoint(DSCP_AF41), IsOk());

  // New dscp value should reset the flow.
  EXPECT_CALL(qos, RemoveSocketFromFlow(_, _, _, _));
  EXPECT_CALL(qos, AddSocketToFlow(_, _, _, QOSTrafficTypeBestEffort, _, _))
      .WillOnce(DoAll(SetArgPointee<5>(kFakeFlowId2), Return(true)));
  EXPECT_CALL(qos, SetFlow(_, _, QOSSetOutgoingDSCPValue, _,
                           DscpPointee(DSCP_DEFAULT), _, _));
  EXPECT_THAT(client->SetDiffServCodePoint(DSCP_DEFAULT), IsOk());

  // Called from DscpManager destructor.
  EXPECT_CALL(qos, RemoveSocketFromFlow(_, _, _, _));
  EXPECT_CALL(qos, CloseHandle(kFakeHandle));
}

// TODO(zstein): Mocking out DscpManager might be simpler here
// (just verify that DscpManager::Set and DscpManager::PrepareForSend are
// called).
TEST_F(UDPSocketTest, SendToCallsQwaveApis) {
  MockQwaveAPI qos;
  std::unique_ptr<UDPSocket> client = UnconnectedDscpTestClient(qos);

  EXPECT_CALL(qos, CreateHandle(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(kFakeHandle), Return(true)));
  EXPECT_THAT(client->SetDiffServCodePoint(DSCP_AF41), IsOk());

  EXPECT_CALL(qos, AddSocketToFlow(_, _, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<5>(kFakeFlowId1), Return(true)));
  EXPECT_CALL(qos, SetFlow(_, _, _, _, _, _, _));

  std::string simple_message("hello world");
  IPEndPoint server_address(IPAddress::IPv4Localhost(), 9438);
  int rv = SendToSocket(client.get(), simple_message, server_address);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // TODO(zstein): Move to second test case (Qwave APIs called once per address)
  rv = SendToSocket(client.get(), simple_message, server_address);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // TODO(zstein): Move to third test case (Qwave APIs called for each
  // destination address).
  EXPECT_CALL(qos, AddSocketToFlow(_, _, _, _, _, _)).WillOnce(Return(true));
  IPEndPoint server_address2(IPAddress::IPv4Localhost(), 9439);

  rv = SendToSocket(client.get(), simple_message, server_address2);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // Called from DscpManager destructor.
  EXPECT_CALL(qos, RemoveSocketFromFlow(_, _, _, _));
  EXPECT_CALL(qos, CloseHandle(kFakeHandle));
}

class DscpManagerTest : public testing::Test {
 protected:
  DscpManagerTest() : dscp_manager_(qos_, INVALID_SOCKET, (HANDLE)0) {
    CreateUDPAddress("1.2.3.4", 9001, &address1_);
    CreateUDPAddress("1234:5678:90ab:cdef:1234:5678:90ab:cdef", 9002,
                     &address2_);
  }

  MockQwaveAPI qos_;
  DscpManager dscp_manager_;

  IPEndPoint address1_;
  IPEndPoint address2_;
};

TEST_F(DscpManagerTest, PrepareForSendIsNoopIfNoSet) {
  dscp_manager_.PrepareForSend(address1_);
}

TEST_F(DscpManagerTest, PrepareForSendCallsQwaveApisAfterSet) {
  dscp_manager_.Set(DSCP_CS2);

  // AddSocketToFlow should be called for each address.
  EXPECT_CALL(qos_, AddSocketToFlow(_, _, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<5>(kFakeFlowId1), Return(true)))
      .WillOnce(Return(true));
  // SetFlow should only be called when the flow is first created.
  EXPECT_CALL(qos_, SetFlow(_, _, _, _, _, _, _));
  dscp_manager_.PrepareForSend(address1_);
  EXPECT_CALL(qos_, SetFlow(_, _, _, _, _, _, _)).Times(0);
  dscp_manager_.PrepareForSend(address2_);

  // Called from DscpManager destructor.
  EXPECT_CALL(qos_, RemoveSocketFromFlow(_, _, _, _));
}

TEST_F(DscpManagerTest, PrepareForSendCallsQwaveApisOncePerAddress) {
  dscp_manager_.Set(DSCP_CS2);

  EXPECT_CALL(qos_, AddSocketToFlow(_, _, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<5>(kFakeFlowId1), Return(true)));
  EXPECT_CALL(qos_, SetFlow(_, _, _, _, _, _, _));
  dscp_manager_.PrepareForSend(address1_);
  EXPECT_CALL(qos_, AddSocketToFlow(_, _, _, _, _, _)).Times(0);
  EXPECT_CALL(qos_, SetFlow(_, _, _, _, _, _, _)).Times(0);
  dscp_manager_.PrepareForSend(address1_);

  // Called from DscpManager destructor.
  EXPECT_CALL(qos_, RemoveSocketFromFlow(_, _, _, _));
}

TEST_F(DscpManagerTest, SetDestroysExistingFlowAndResetsPrepareState) {
  dscp_manager_.Set(DSCP_CS2);
  EXPECT_CALL(qos_, AddSocketToFlow(_, _, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<5>(kFakeFlowId1), Return(true)));
  EXPECT_CALL(qos_, SetFlow(_, _, _, _, _, _, _));
  dscp_manager_.PrepareForSend(address1_);

  // Calling Set should destroy the existing flow.
  // TODO(zstein): Verify that RemoveSocketFromFlow with no address
  // destroys the flow for all destinations.
  EXPECT_CALL(qos_, RemoveSocketFromFlow(_, NULL, kFakeFlowId1, _));
  dscp_manager_.Set(DSCP_CS5);

  EXPECT_CALL(qos_, AddSocketToFlow(_, _, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<5>(kFakeFlowId2), Return(true)));
  EXPECT_CALL(qos_, SetFlow(_, _, _, _, _, _, _));
  dscp_manager_.PrepareForSend(address1_);

  // Called from DscpManager destructor.
  EXPECT_CALL(qos_, RemoveSocketFromFlow(_, _, kFakeFlowId2, _));
}
#endif

TEST_F(UDPSocketTest, ReadWithSocketOptimization) {
  const uint16_t kPort = 10000;
  std::string simple_message("hello world!");

  // Setup the server to listen.
  IPEndPoint server_address(IPAddress::IPv4Localhost(), kPort);
  UDPServerSocket server(NULL, NetLogSource());
  server.AllowAddressReuse();
  int rv = server.Listen(server_address);
  ASSERT_THAT(rv, IsOk());

  // Setup the client, enable experimental optimization and connected to the
  // server.
  UDPClientSocket client(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  client.EnableRecvOptimization();
  rv = client.Connect(server_address);
  EXPECT_THAT(rv, IsOk());

  // Get the client's address.
  IPEndPoint client_address;
  rv = client.GetLocalAddress(&client_address);
  EXPECT_THAT(rv, IsOk());

  // Server sends the message to the client.
  rv = SendToSocket(&server, simple_message, client_address);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));

  // Client receives the message.
  std::string str = ReadSocket(&client);
  EXPECT_EQ(simple_message, str);

  server.Close();
  client.Close();
}

// Tests that read from a socket correctly returns
// |ERR_MSG_TOO_BIG| when the buffer is too small and
// returns the actual message when it fits the buffer.
// For the optimized path, the buffer size should be at least
// 1 byte greater than the message.
TEST_F(UDPSocketTest, ReadWithSocketOptimizationTruncation) {
  const uint16_t kPort = 10000;
  std::string too_long_message(kMaxRead + 1, 'A');
  std::string right_length_message(kMaxRead - 1, 'B');
  std::string exact_length_message(kMaxRead, 'C');

  // Setup the server to listen.
  IPEndPoint server_address(IPAddress::IPv4Localhost(), kPort);
  UDPServerSocket server(NULL, NetLogSource());
  server.AllowAddressReuse();
  int rv = server.Listen(server_address);
  ASSERT_THAT(rv, IsOk());

  // Setup the client, enable experimental optimization and connected to the
  // server.
  UDPClientSocket client(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  client.EnableRecvOptimization();
  rv = client.Connect(server_address);
  EXPECT_THAT(rv, IsOk());

  // Get the client's address.
  IPEndPoint client_address;
  rv = client.GetLocalAddress(&client_address);
  EXPECT_THAT(rv, IsOk());

  // Send messages to the client.
  rv = SendToSocket(&server, too_long_message, client_address);
  EXPECT_EQ(too_long_message.length(), static_cast<size_t>(rv));
  rv = SendToSocket(&server, right_length_message, client_address);
  EXPECT_EQ(right_length_message.length(), static_cast<size_t>(rv));
  rv = SendToSocket(&server, exact_length_message, client_address);
  EXPECT_EQ(exact_length_message.length(), static_cast<size_t>(rv));

  // Client receives the messages.

  // 1. The first message is |too_long_message|. Its size exceeds the buffer.
  // In that case, the client is expected to get |ERR_MSG_TOO_BIG| when the
  // data is read.
  TestCompletionCallback callback;
  rv = client.Read(buffer_.get(), kMaxRead, callback.callback());
  rv = callback.GetResult(rv);
  EXPECT_EQ(ERR_MSG_TOO_BIG, rv);

  // 2. The second message is |right_length_message|. Its size is
  // one byte smaller than the size of the buffer. In that case, the client
  // is expected to read the whole message successfully.
  rv = client.Read(buffer_.get(), kMaxRead, callback.callback());
  rv = callback.GetResult(rv);
  EXPECT_EQ(static_cast<int>(right_length_message.length()), rv);
  EXPECT_EQ(right_length_message, std::string(buffer_->data(), rv));

  // 3. The third message is |exact_length_message|. Its size is equal to
  // the read buffer size. In that case, the client expects to get
  // |ERR_MSG_TOO_BIG| when the socket is read. Internally, the optimized
  // path uses read() system call that requires one extra byte to detect
  // truncated messages; therefore, messages that fill the buffer exactly
  // are considered truncated.
  // The optimization is only enabled on POSIX platforms. On Windows,
  // the optimization is turned off; therefore, the client
  // should be able to read the whole message without encountering
  // |ERR_MSG_TOO_BIG|.
  rv = client.Read(buffer_.get(), kMaxRead, callback.callback());
  rv = callback.GetResult(rv);
#if defined(OS_POSIX)
  EXPECT_EQ(ERR_MSG_TOO_BIG, rv);
#else
  EXPECT_EQ(static_cast<int>(exact_length_message.length()), rv);
  EXPECT_EQ(exact_length_message, std::string(buffer_->data(), rv));
#endif
  server.Close();
  client.Close();
}

// On Android, where socket tagging is supported, verify that UDPSocket::Tag
// works as expected.
#if defined(OS_ANDROID)
TEST_F(UDPSocketTest, Tag) {
  UDPServerSocket server(nullptr, NetLogSource());
  ASSERT_THAT(server.Listen(IPEndPoint(IPAddress::IPv4Localhost(), 0)), IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server.GetLocalAddress(&server_address), IsOk());

  UDPClientSocket client(DatagramSocket::DEFAULT_BIND, nullptr, NetLogSource());
  ASSERT_THAT(client.Connect(server_address), IsOk());

  // Verify UDP packets are tagged and counted properly.
  int32_t tag_val1 = 0x12345678;
  uint64_t old_traffic = GetTaggedBytes(tag_val1);
  SocketTag tag1(SocketTag::UNSET_UID, tag_val1);
  client.ApplySocketTag(tag1);
  // Client sends to the server.
  std::string simple_message("hello world!");
  int rv = WriteSocket(&client, simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));
  // Server waits for message.
  std::string str = RecvFromSocket(&server);
  EXPECT_EQ(simple_message, str);
  // Server echoes reply.
  rv = SendToSocket(&server, simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));
  // Client waits for response.
  str = ReadSocket(&client);
  EXPECT_EQ(simple_message, str);
  EXPECT_GT(GetTaggedBytes(tag_val1), old_traffic);

  // Verify socket can be retagged with a new value and the current process's
  // UID.
  int32_t tag_val2 = 0x87654321;
  old_traffic = GetTaggedBytes(tag_val2);
  SocketTag tag2(getuid(), tag_val2);
  client.ApplySocketTag(tag2);
  // Client sends to the server.
  rv = WriteSocket(&client, simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));
  // Server waits for message.
  str = RecvFromSocket(&server);
  EXPECT_EQ(simple_message, str);
  // Server echoes reply.
  rv = SendToSocket(&server, simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));
  // Client waits for response.
  str = ReadSocket(&client);
  EXPECT_EQ(simple_message, str);
  EXPECT_GT(GetTaggedBytes(tag_val2), old_traffic);

  // Verify socket can be retagged with a new value and the current process's
  // UID.
  old_traffic = GetTaggedBytes(tag_val1);
  client.ApplySocketTag(tag1);
  // Client sends to the server.
  rv = WriteSocket(&client, simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));
  // Server waits for message.
  str = RecvFromSocket(&server);
  EXPECT_EQ(simple_message, str);
  // Server echoes reply.
  rv = SendToSocket(&server, simple_message);
  EXPECT_EQ(simple_message.length(), static_cast<size_t>(rv));
  // Client waits for response.
  str = ReadSocket(&client);
  EXPECT_EQ(simple_message, str);
  EXPECT_GT(GetTaggedBytes(tag_val1), old_traffic);
}
#endif

}  // namespace net
