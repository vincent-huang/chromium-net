// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/ssl_client_socket.h"

#include <string>

#include "base/logging.h"
#include "net/socket/ssl_client_socket_impl.h"
#include "net/socket/stream_socket.h"
#include "net/ssl/ssl_client_session_cache.h"
#include "net/ssl/ssl_key_logger.h"

namespace net {

SSLClientSocket::SSLClientSocket()
    : signed_cert_timestamps_received_(false),
      stapled_ocsp_response_received_(false) {}

// static
void SSLClientSocket::SetSSLKeyLogger(std::unique_ptr<SSLKeyLogger> logger) {
  SSLClientSocketImpl::SetSSLKeyLogger(std::move(logger));
}

// static
std::vector<uint8_t> SSLClientSocket::SerializeNextProtos(
    const NextProtoVector& next_protos) {
  std::vector<uint8_t> wire_protos;
  for (const NextProto next_proto : next_protos) {
    const std::string proto = NextProtoToString(next_proto);
    if (proto.size() > 255) {
      LOG(WARNING) << "Ignoring overlong ALPN protocol: " << proto;
      continue;
    }
    if (proto.size() == 0) {
      LOG(WARNING) << "Ignoring empty ALPN protocol";
      continue;
    }
    wire_protos.push_back(proto.size());
    for (const char ch : proto) {
      wire_protos.push_back(static_cast<uint8_t>(ch));
    }
  }

  return wire_protos;
}

SSLClientContext::SSLClientContext(
    SSLConfigService* ssl_config_service,
    CertVerifier* cert_verifier,
    TransportSecurityState* transport_security_state,
    CTVerifier* cert_transparency_verifier,
    CTPolicyEnforcer* ct_policy_enforcer,
    SSLClientSessionCache* ssl_client_session_cache)
    : ssl_config_service_(ssl_config_service),
      cert_verifier_(cert_verifier),
      transport_security_state_(transport_security_state),
      cert_transparency_verifier_(cert_transparency_verifier),
      ct_policy_enforcer_(ct_policy_enforcer),
      ssl_client_session_cache_(ssl_client_session_cache) {
  CHECK(cert_verifier_);
  CHECK(transport_security_state_);
  CHECK(cert_transparency_verifier_);
  CHECK(ct_policy_enforcer_);

  if (ssl_config_service_) {
    config_ = ssl_config_service_->GetSSLContextConfig();
    ssl_config_service_->AddObserver(this);
  }
  CertDatabase::GetInstance()->AddObserver(this);
}

SSLClientContext::~SSLClientContext() {
  if (ssl_config_service_) {
    ssl_config_service_->RemoveObserver(this);
  }
  CertDatabase::GetInstance()->RemoveObserver(this);
}

std::unique_ptr<SSLClientSocket> SSLClientContext::CreateSSLClientSocket(
    std::unique_ptr<StreamSocket> stream_socket,
    const HostPortPair& host_and_port,
    const SSLConfig& ssl_config) {
  return std::make_unique<SSLClientSocketImpl>(this, std::move(stream_socket),
                                               host_and_port, ssl_config);
}

void SSLClientContext::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SSLClientContext::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SSLClientContext::OnSSLContextConfigChanged() {
  // TODO(davidben): Should we flush |ssl_client_session_cache_| here? We flush
  // the socket pools, but not the session cache. While BoringSSL-based servers
  // never change version or cipher negotiation based on client-offered
  // sessions, other servers do.
  config_ = ssl_config_service_->GetSSLContextConfig();
  NotifySSLConfigChanged(false /* not a cert database change */);
}

void SSLClientContext::OnCertDBChanged() {
  if (ssl_client_session_cache_) {
    ssl_client_session_cache_->Flush();
  }
  NotifySSLConfigChanged(true /* cert database change */);
}

void SSLClientContext::NotifySSLConfigChanged(bool is_cert_database_change) {
  for (Observer& observer : observers_) {
    observer.OnSSLConfigChanged(is_cert_database_change);
  }
}

}  // namespace net
