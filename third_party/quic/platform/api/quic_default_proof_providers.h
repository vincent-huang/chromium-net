// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_QUIC_PLATFORM_API_QUIC_DEFAULT_PROOF_PROVIDERS_H_
#define NET_THIRD_PARTY_QUIC_PLATFORM_API_QUIC_DEFAULT_PROOF_PROVIDERS_H_

#include <memory>

#include "net/third_party/quic/core/crypto/proof_source.h"
#include "net/third_party/quic/core/crypto/proof_verifier.h"
#include "net/third_party/quic/platform/impl/quic_default_proof_providers_impl.h"

namespace quic {

// Provides a default proof verifier.  The verifier has to do a good faith
// attempt at verifying the certificate against a reasonable root store, and not
// just always return success.
inline std::unique_ptr<ProofVerifier> CreateDefaultProofVerifier() {
  return CreateDefaultProofVerifierImpl();
}

// Provides a default proof source for CLI-based tools.  The actual certificates
// used in the proof source should be confifgurable via command-line flags.
inline std::unique_ptr<ProofSource> CreateDefaultProofSource() {
  return CreateDefaultProofSourceImpl();
}

}  // namespace quic

#endif  // NET_THIRD_PARTY_QUIC_PLATFORM_API_QUIC_DEFAULT_PROOF_PROVIDERS_H_
