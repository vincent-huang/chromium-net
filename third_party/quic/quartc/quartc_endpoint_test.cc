// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/quic/quartc/quartc_endpoint.h"

#include "net/third_party/quic/core/quic_versions.h"
#include "net/third_party/quic/platform/api/quic_test.h"
#include "net/third_party/quic/quartc/quartc_crypto_helpers.h"
#include "net/third_party/quic/quartc/quartc_fakes.h"
#include "net/third_party/quic/quartc/simulated_packet_transport.h"
#include "net/third_party/quic/test_tools/simulator/link.h"
#include "net/third_party/quic/test_tools/simulator/simulator.h"

namespace quic {
namespace {

class QuartcEndpointTest : public QuicTest {
 protected:
  QuartcEndpointTest()
      : client_transport_(&simulator_,
                          "client_transport",
                          "server_transport",
                          10 * kDefaultMaxPacketSize),
        server_transport_(&simulator_,
                          "server_transport",
                          "client_transport",
                          10 * kDefaultMaxPacketSize),
        client_server_link_(&client_transport_,
                            &server_transport_,
                            QuicBandwidth::FromKBitsPerSecond(10000),
                            QuicTime::Delta::FromMilliseconds(1)),
        server_session_delegate_(&server_stream_delegate_,
                                 simulator_.GetClock()),
        server_endpoint_delegate_(&server_session_delegate_),
        server_endpoint_(
            QuicMakeUnique<QuartcServerEndpoint>(simulator_.GetAlarmFactory(),
                                                 simulator_.GetClock(),
                                                 &server_endpoint_delegate_,
                                                 QuartcSessionConfig())),
        client_session_delegate_(&client_stream_delegate_,
                                 simulator_.GetClock()),
        client_endpoint_delegate_(&client_session_delegate_),
        client_endpoint_(QuicMakeUnique<QuartcClientEndpoint>(
            simulator_.GetAlarmFactory(),
            simulator_.GetClock(),
            &client_endpoint_delegate_,
            QuartcSessionConfig(),
            /*serialized_server_config=*/"")) {}

  simulator::Simulator simulator_;

  simulator::SimulatedQuartcPacketTransport client_transport_;
  simulator::SimulatedQuartcPacketTransport server_transport_;
  simulator::SymmetricLink client_server_link_;

  FakeQuartcStreamDelegate server_stream_delegate_;
  FakeQuartcSessionDelegate server_session_delegate_;
  FakeQuartcEndpointDelegate server_endpoint_delegate_;

  std::unique_ptr<QuartcServerEndpoint> server_endpoint_;

  FakeQuartcStreamDelegate client_stream_delegate_;
  FakeQuartcSessionDelegate client_session_delegate_;
  FakeQuartcEndpointDelegate client_endpoint_delegate_;

  std::unique_ptr<QuartcClientEndpoint> client_endpoint_;
};

// After calling Connect, the client endpoint must wait for an async callback.
// The callback occurs after a finite amount of time and produces a session.
TEST_F(QuartcEndpointTest, ClientCreatesSessionAsynchronously) {
  client_endpoint_->Connect(&client_transport_);

  EXPECT_EQ(client_endpoint_delegate_.session(), nullptr);

  EXPECT_TRUE(simulator_.RunUntil(
      [this] { return client_endpoint_delegate_.session() != nullptr; }));
}

// Tests that the server can negotiate for an older QUIC version if the client
// attempts to connect using a newer version.
TEST_F(QuartcEndpointTest,
       QUIC_TEST_DISABLED_IN_CHROME(ServerNegotiatesForOldVersion)) {
  // Note: for this test, we need support for two versions.  Which two shouldn't
  // matter, but they must be enabled so that the version manager doesn't filter
  // them out.
  SetQuicReloadableFlag(quic_enable_version_46, true);
  SetQuicReloadableFlag(quic_enable_version_43, true);

  // Reset the client endpoint to prefer version 46 but also be capable of
  // speaking version 43.
  ParsedQuicVersionVector client_versions;
  client_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_46});
  client_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_43});
  client_endpoint_ = QuicMakeUnique<QuartcClientEndpoint>(
      simulator_.GetAlarmFactory(), simulator_.GetClock(),
      &client_endpoint_delegate_, QuartcSessionConfig(),
      /*serialized_server_config=*/"",
      QuicMakeUnique<QuicVersionManager>(client_versions));

  // Reset the server endpoint to only speak version 43.
  ParsedQuicVersionVector server_versions;
  server_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_43});
  server_endpoint_ = QuicMakeUnique<QuartcServerEndpoint>(
      simulator_.GetAlarmFactory(), simulator_.GetClock(),
      &server_endpoint_delegate_, QuartcSessionConfig(),
      QuicMakeUnique<QuicVersionManager>(server_versions));

  // The endpoints should be able to establish a connection using version 46.
  server_endpoint_->Connect(&server_transport_);
  client_endpoint_->Connect(&client_transport_);

  ASSERT_TRUE(simulator_.RunUntil([this] {
    return client_endpoint_delegate_.session() != nullptr &&
           client_endpoint_delegate_.session()->IsEncryptionEstablished() &&
           server_endpoint_delegate_.session() != nullptr &&
           server_endpoint_delegate_.session()->IsEncryptionEstablished();
  }));
  EXPECT_EQ(client_endpoint_delegate_.session()->connection()->version(),
            server_versions[0]);
  EXPECT_EQ(server_endpoint_delegate_.session()->connection()->version(),
            server_versions[0]);
}

// Tests that the server can accept connections from clients that use older
// QUIC versions.
TEST_F(QuartcEndpointTest,
       QUIC_TEST_DISABLED_IN_CHROME(ServerAcceptsOldVersion)) {
  // Note: for this test, we need support for two versions.  Which two shouldn't
  // matter, but they must be enabled so that the version manager doesn't filter
  // them out.
  SetQuicReloadableFlag(quic_enable_version_46, true);
  SetQuicReloadableFlag(quic_enable_version_43, true);

  // Reset the client endpoint to only speak version 43.
  ParsedQuicVersionVector client_versions;
  client_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_43});
  client_endpoint_ = QuicMakeUnique<QuartcClientEndpoint>(
      simulator_.GetAlarmFactory(), simulator_.GetClock(),
      &client_endpoint_delegate_, QuartcSessionConfig(),
      /*serialized_server_config=*/"",
      QuicMakeUnique<QuicVersionManager>(client_versions));

  // Reset the server endpoint to prefer version 46 but also be capable of
  // speaking version 43.
  ParsedQuicVersionVector server_versions;
  server_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_46});
  server_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_43});
  server_endpoint_ = QuicMakeUnique<QuartcServerEndpoint>(
      simulator_.GetAlarmFactory(), simulator_.GetClock(),
      &server_endpoint_delegate_, QuartcSessionConfig(),
      QuicMakeUnique<QuicVersionManager>(server_versions));

  // The endpoints should be able to establish a connection using version 46.
  server_endpoint_->Connect(&server_transport_);
  client_endpoint_->Connect(&client_transport_);

  ASSERT_TRUE(simulator_.RunUntil([this] {
    return client_endpoint_delegate_.session() != nullptr &&
           client_endpoint_delegate_.session()->IsEncryptionEstablished() &&
           server_endpoint_delegate_.session() != nullptr &&
           server_endpoint_delegate_.session()->IsEncryptionEstablished();
  }));
  EXPECT_EQ(client_endpoint_delegate_.session()->connection()->version(),
            client_versions[0]);
  EXPECT_EQ(server_endpoint_delegate_.session()->connection()->version(),
            client_versions[0]);
}

// Tests that version negotiation fails when the client and server support
// completely disjoint sets of versions.
TEST_F(QuartcEndpointTest, VersionNegotiationWithDisjointVersions) {
  // Note: for this test, we need support for two versions.  Which two shouldn't
  // matter, but they must be enabled so that the version manager doesn't filter
  // them out.
  SetQuicReloadableFlag(quic_enable_version_46, true);
  SetQuicReloadableFlag(quic_enable_version_43, true);

  // Reset the client endpoint to only speak version 43.
  ParsedQuicVersionVector client_versions;
  client_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_43});
  client_endpoint_ = QuicMakeUnique<QuartcClientEndpoint>(
      simulator_.GetAlarmFactory(), simulator_.GetClock(),
      &client_endpoint_delegate_, QuartcSessionConfig(),
      /*serialized_server_config=*/"",
      QuicMakeUnique<QuicVersionManager>(client_versions));

  // Reset the server endpoint to only speak version 46.
  ParsedQuicVersionVector server_versions;
  server_versions.push_back({PROTOCOL_QUIC_CRYPTO, QUIC_VERSION_46});
  server_endpoint_ = QuicMakeUnique<QuartcServerEndpoint>(
      simulator_.GetAlarmFactory(), simulator_.GetClock(),
      &server_endpoint_delegate_, QuartcSessionConfig(),
      QuicMakeUnique<QuicVersionManager>(server_versions));

  // The endpoints should be unable to establish a connection.
  server_endpoint_->Connect(&server_transport_);
  client_endpoint_->Connect(&client_transport_);

  // Note that the error is reported from the client and *not* the server.  The
  // server sees an invalid version, sends a version negotiation packet, and
  // never gets a response, because the client stops sending when it can't find
  // a mutually supported versions.
  ASSERT_TRUE(simulator_.RunUntil([this] {
    return client_endpoint_delegate_.session() != nullptr &&
           client_endpoint_delegate_.session()->error() != QUIC_NO_ERROR;
  }));
  EXPECT_EQ(client_endpoint_delegate_.session()->error(), QUIC_INVALID_VERSION);
}

}  // namespace
}  // namespace quic
