// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_QUIC_TEST_TOOLS_MOCK_ENCRYPTER_H_
#define NET_THIRD_PARTY_QUIC_TEST_TOOLS_MOCK_ENCRYPTER_H_

#include <cstddef>

#include "base/compiler_specific.h"
#include "base/macros.h"
#include "net/third_party/quic/core/crypto/quic_encrypter.h"
#include "net/third_party/quic/core/quic_types.h"
#include "net/third_party/quic/platform/api/quic_export.h"
#include "net/third_party/quic/platform/api/quic_string_piece.h"

namespace quic {

// A MockEncrypter is a QuicEncrypter that returns a plaintext
// unmodified. No encryption or MAC is applied. This is used
// to allow fuzzing to mutate plaintext packets.
class MockEncrypter : public QuicEncrypter {
 public:
  explicit MockEncrypter(Perspective perspective);
  ~MockEncrypter() override {}

  // QuicEncrypter implementation
  bool SetKey(QuicStringPiece key) override;
  bool SetNoncePrefix(QuicStringPiece nonce_prefix) override;
  bool SetIV(QuicStringPiece iv) override;
  bool EncryptPacket(QuicTransportVersion version,
                     QuicPacketNumber packet_number,
                     QuicStringPiece associated_data,
                     QuicStringPiece plaintext,
                     char* output,
                     size_t* output_length,
                     size_t max_output_length) override;
  size_t GetKeySize() const override;
  size_t GetNoncePrefixSize() const override;
  size_t GetIVSize() const override;
  size_t GetMaxPlaintextSize(size_t ciphertext_size) const override;
  size_t GetCiphertextSize(size_t plaintext_size) const override;
  QuicStringPiece GetKey() const override;
  QuicStringPiece GetNoncePrefix() const override;

 private:
  DISALLOW_COPY_AND_ASSIGN(MockEncrypter);
};

}  // namespace quic

#endif  // NET_THIRD_PARTY_QUIC_TEST_TOOLS_MOCK_ENCRYPTER_H_
