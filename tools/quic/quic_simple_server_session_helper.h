// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_TOOLS_QUIC_QUIC_SIMPLE_SERVER_SESSION_HELPER_H_
#define NET_TOOLS_QUIC_QUIC_SIMPLE_SERVER_SESSION_HELPER_H_

#include "net/third_party/quic/core/crypto/quic_random.h"
#include "net/third_party/quic/core/http/quic_server_session_base.h"

namespace net {

// Simple helper for server sessions which generates a new random
// connection ID for stateless rejects.
class QuicSimpleServerSessionHelper
    : public quic::QuicCryptoServerStream::Helper {
 public:
  explicit QuicSimpleServerSessionHelper(quic::QuicRandom* random);

  ~QuicSimpleServerSessionHelper() override;

  quic::QuicConnectionId GenerateConnectionIdForReject(
      quic::QuicTransportVersion /*version*/,
      quic::QuicConnectionId /*connection_id*/) const override;

  bool CanAcceptClientHello(const quic::CryptoHandshakeMessage& message,
                            const quic::QuicSocketAddress& client_address,
                            const quic::QuicSocketAddress& peer_address,
                            const quic::QuicSocketAddress& self_address,
                            std::string* error_details) const override;

 private:
  quic::QuicRandom* random_;  // Unowned.
};

}  // namespace net

#endif  // NET_TOOLS_QUIC_QUIC_SIMPLE_SERVER_SESSION_HELPER_H_
