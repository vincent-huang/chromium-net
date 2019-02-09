// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/ssl_connect_job.h"

#include <cstdlib>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/trace_event/trace_event.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_errors.h"
#include "net/base/trace_constants.h"
#include "net/base/url_util.h"
#include "net/http/http_proxy_client_socket.h"
#include "net/http/http_proxy_client_socket_pool.h"
#include "net/log/net_log_source_type.h"
#include "net/log/net_log_with_source.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/socks_connect_job.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/transport_connect_job.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_info.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net {

SSLSocketParams::SSLSocketParams(
    const scoped_refptr<TransportSocketParams>& direct_params,
    const scoped_refptr<SOCKSSocketParams>& socks_proxy_params,
    const scoped_refptr<HttpProxySocketParams>& http_proxy_params,
    const HostPortPair& host_and_port,
    const SSLConfig& ssl_config,
    PrivacyMode privacy_mode)
    : direct_params_(direct_params),
      socks_proxy_params_(socks_proxy_params),
      http_proxy_params_(http_proxy_params),
      host_and_port_(host_and_port),
      ssl_config_(ssl_config),
      privacy_mode_(privacy_mode) {
  // Only one set of lower level pool params should be non-NULL.
  DCHECK((direct_params_ && !socks_proxy_params_ && !http_proxy_params_) ||
         (!direct_params_ && socks_proxy_params_ && !http_proxy_params_) ||
         (!direct_params_ && !socks_proxy_params_ && http_proxy_params_));
}

SSLSocketParams::~SSLSocketParams() = default;

SSLSocketParams::ConnectionType SSLSocketParams::GetConnectionType() const {
  if (direct_params_.get()) {
    DCHECK(!socks_proxy_params_.get());
    DCHECK(!http_proxy_params_.get());
    return DIRECT;
  }

  if (socks_proxy_params_.get()) {
    DCHECK(!http_proxy_params_.get());
    return SOCKS_PROXY;
  }

  DCHECK(http_proxy_params_.get());
  return HTTP_PROXY;
}

const scoped_refptr<TransportSocketParams>&
SSLSocketParams::GetDirectConnectionParams() const {
  DCHECK_EQ(GetConnectionType(), DIRECT);
  return direct_params_;
}

const scoped_refptr<SOCKSSocketParams>&
SSLSocketParams::GetSocksProxyConnectionParams() const {
  DCHECK_EQ(GetConnectionType(), SOCKS_PROXY);
  return socks_proxy_params_;
}

const scoped_refptr<HttpProxySocketParams>&
SSLSocketParams::GetHttpProxyConnectionParams() const {
  DCHECK_EQ(GetConnectionType(), HTTP_PROXY);
  return http_proxy_params_;
}

// Timeout for the SSL handshake portion of the connect.
static const int kSSLHandshakeTimeoutInSeconds = 30;

SSLConnectJob::SSLConnectJob(
    RequestPriority priority,
    const CommonConnectJobParams& common_connect_job_params,
    const scoped_refptr<SSLSocketParams>& params,
    HttpProxyClientSocketPool* http_proxy_pool,
    ConnectJob::Delegate* delegate)
    : ConnectJob(priority,
                 ConnectionTimeout(
                     *params,
                     common_connect_job_params.network_quality_estimator),
                 common_connect_job_params,
                 delegate,
                 NetLogWithSource::Make(common_connect_job_params.net_log,
                                        NetLogSourceType::SSL_CONNECT_JOB)),
      params_(params),
      http_proxy_pool_(http_proxy_pool),
      callback_(base::BindRepeating(&SSLConnectJob::OnIOComplete,
                                    base::Unretained(this))) {}

SSLConnectJob::~SSLConnectJob() = default;

LoadState SSLConnectJob::GetLoadState() const {
  switch (next_state_) {
    case STATE_TRANSPORT_CONNECT:
    case STATE_SOCKS_CONNECT:
    case STATE_TUNNEL_CONNECT:
      return LOAD_STATE_IDLE;
    case STATE_TRANSPORT_CONNECT_COMPLETE:
    case STATE_SOCKS_CONNECT_COMPLETE:
      return nested_connect_job_->GetLoadState();
    case STATE_TUNNEL_CONNECT_COMPLETE:
      if (transport_socket_handle_->socket())
        return LOAD_STATE_ESTABLISHING_PROXY_TUNNEL;
      return transport_socket_handle_->GetLoadState();
    case STATE_SSL_CONNECT:
    case STATE_SSL_CONNECT_COMPLETE:
      return LOAD_STATE_SSL_HANDSHAKE;
    default:
      NOTREACHED();
      return LOAD_STATE_IDLE;
  }
}

bool SSLConnectJob::HasEstablishedConnection() const {
  // Return true to prevent creating any backup jobs when this is used on top of
  // another socket pool type.
  if (http_proxy_pool_)
    return true;

  // If waiting on a nested ConnectJob, defer to that ConnectJob's state.
  if (nested_connect_job_)
    return nested_connect_job_->HasEstablishedConnection();
  // Otherwise, return true if a socket has been created.
  return nested_socket_ || ssl_socket_;
}

void SSLConnectJob::OnConnectJobComplete(int result, ConnectJob* job) {
  DCHECK_EQ(job, nested_connect_job_.get());
  OnIOComplete(result);
}

void SSLConnectJob::GetAdditionalErrorState(ClientSocketHandle* handle) {
  // Headers in |error_response_info_| indicate a proxy tunnel setup
  // problem. See DoTunnelConnectComplete.
  if (error_response_info_.headers.get()) {
    handle->set_pending_http_proxy_connection(
        std::move(transport_socket_handle_));
  }
  handle->set_ssl_error_response_info(error_response_info_);
  if (!connect_timing_.ssl_start.is_null())
    handle->set_is_ssl_error(true);

  handle->set_connection_attempts(connection_attempts_);
}

base::TimeDelta SSLConnectJob::ConnectionTimeout(
    const SSLSocketParams& params,
    const NetworkQualityEstimator* network_quality_estimator) {
  SSLSocketParams::ConnectionType connection_type = params.GetConnectionType();

  base::TimeDelta nested_job_timeout;
  switch (connection_type) {
    case SSLSocketParams::DIRECT:
      nested_job_timeout = TransportConnectJob::ConnectionTimeout();
      break;
    case SSLSocketParams::SOCKS_PROXY:
      nested_job_timeout = SOCKSConnectJob::ConnectionTimeout();
      break;
    case SSLSocketParams::HTTP_PROXY:
      nested_job_timeout = HttpProxyConnectJob::ConnectionTimeout(
          *params.GetHttpProxyConnectionParams(), network_quality_estimator);
      break;
  }
  return nested_job_timeout +
         base::TimeDelta::FromSeconds(kSSLHandshakeTimeoutInSeconds);
}

void SSLConnectJob::OnIOComplete(int result) {
  int rv = DoLoop(result);
  if (rv != ERR_IO_PENDING)
    NotifyDelegateOfCompletion(rv);  // Deletes |this|.
}

int SSLConnectJob::DoLoop(int result) {
  TRACE_EVENT0(NetTracingCategory(), "SSLConnectJob::DoLoop");
  DCHECK_NE(next_state_, STATE_NONE);

  int rv = result;
  do {
    State state = next_state_;
    next_state_ = STATE_NONE;
    switch (state) {
      case STATE_TRANSPORT_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoTransportConnect();
        break;
      case STATE_TRANSPORT_CONNECT_COMPLETE:
        rv = DoTransportConnectComplete(rv);
        break;
      case STATE_SOCKS_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoSOCKSConnect();
        break;
      case STATE_SOCKS_CONNECT_COMPLETE:
        rv = DoSOCKSConnectComplete(rv);
        break;
      case STATE_TUNNEL_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoTunnelConnect();
        break;
      case STATE_TUNNEL_CONNECT_COMPLETE:
        rv = DoTunnelConnectComplete(rv);
        break;
      case STATE_SSL_CONNECT:
        DCHECK_EQ(OK, rv);
        rv = DoSSLConnect();
        break;
      case STATE_SSL_CONNECT_COMPLETE:
        rv = DoSSLConnectComplete(rv);
        break;
      default:
        NOTREACHED() << "bad state";
        rv = ERR_FAILED;
        break;
    }
  } while (rv != ERR_IO_PENDING && next_state_ != STATE_NONE);

  return rv;
}

int SSLConnectJob::DoTransportConnect() {
  DCHECK(!nested_connect_job_);
  DCHECK(params_->GetDirectConnectionParams());

  next_state_ = STATE_TRANSPORT_CONNECT_COMPLETE;
  nested_connect_job_ = TransportConnectJob::CreateTransportConnectJob(
      params_->GetDirectConnectionParams(), priority(),
      common_connect_job_params(), this);
  return nested_connect_job_->Connect();
}

int SSLConnectJob::DoTransportConnectComplete(int result) {
  // TODO(mmenke): Implement a better API to get this information.
  ClientSocketHandle bogus_handle;
  nested_connect_job_->GetAdditionalErrorState(&bogus_handle);
  connection_attempts_.insert(connection_attempts_.end(),
                              bogus_handle.connection_attempts().begin(),
                              bogus_handle.connection_attempts().end());
  if (result == OK) {
    next_state_ = STATE_SSL_CONNECT;
    nested_socket_ = nested_connect_job_->PassSocket();
    nested_socket_->GetPeerAddress(&server_address_);
  }

  return result;
}

int SSLConnectJob::DoSOCKSConnect() {
  DCHECK(!nested_connect_job_);
  DCHECK(params_->GetSocksProxyConnectionParams());

  next_state_ = STATE_SOCKS_CONNECT_COMPLETE;
  nested_connect_job_ = std::make_unique<SOCKSConnectJob>(
      priority(), common_connect_job_params(),
      params_->GetSocksProxyConnectionParams(), this);
  return nested_connect_job_->Connect();
}

int SSLConnectJob::DoSOCKSConnectComplete(int result) {
  if (result == OK) {
    next_state_ = STATE_SSL_CONNECT;
    nested_socket_ = nested_connect_job_->PassSocket();
  }

  return result;
}

int SSLConnectJob::DoTunnelConnect() {
  DCHECK(http_proxy_pool_);
  next_state_ = STATE_TUNNEL_CONNECT_COMPLETE;

  transport_socket_handle_.reset(new ClientSocketHandle());
  scoped_refptr<HttpProxySocketParams> http_proxy_params =
      params_->GetHttpProxyConnectionParams();
  return transport_socket_handle_->Init(
      group_name(), http_proxy_params, priority(), socket_tag(),
      respect_limits(), callback_, http_proxy_pool_, net_log());
}

int SSLConnectJob::DoTunnelConnectComplete(int result) {
  // Extract the information needed to prompt for appropriate proxy
  // authentication so that when ClientSocketPoolBaseHelper calls
  // |GetAdditionalErrorState|, we can easily set the state.
  if (result == ERR_SSL_CLIENT_AUTH_CERT_NEEDED) {
    error_response_info_ = transport_socket_handle_->ssl_error_response_info();
  } else if (result == ERR_PROXY_AUTH_REQUESTED ||
             result == ERR_HTTPS_PROXY_TUNNEL_RESPONSE) {
    StreamSocket* socket = transport_socket_handle_->socket();
    ProxyClientSocket* tunnel_socket = static_cast<ProxyClientSocket*>(socket);
    error_response_info_ = *tunnel_socket->GetConnectResponseInfo();
  }
  if (result < 0)
    return result;

  next_state_ = STATE_SSL_CONNECT;
  return result;
}

int SSLConnectJob::DoSSLConnect() {
  TRACE_EVENT0(NetTracingCategory(), "SSLConnectJob::DoSSLConnect");
  next_state_ = STATE_SSL_CONNECT_COMPLETE;

  // Reset the timeout to just the time allowed for the SSL handshake.
  ResetTimer(base::TimeDelta::FromSeconds(kSSLHandshakeTimeoutInSeconds));

  // If the handle has a fresh socket, get its connect start and DNS times.
  // This should always be the case in the nested socket pool path, and
  // *will* always be the case in the non-nested socket pool path.
  const LoadTimingInfo::ConnectTiming* socket_connect_timing = nullptr;
  if (nested_connect_job_) {
    socket_connect_timing = &nested_connect_job_->connect_timing();
  } else if (!transport_socket_handle_->is_reused() &&
             !transport_socket_handle_->connect_timing()
                  .connect_start.is_null()) {
    socket_connect_timing = &transport_socket_handle_->connect_timing();
  }

  if (socket_connect_timing) {
    // Overwriting |connect_start| serves two purposes - it adjusts timing so
    // |connect_start| doesn't include dns times, and it adjusts the time so
    // as not to include time spent waiting for an idle socket.
    connect_timing_.connect_start = socket_connect_timing->connect_start;
    connect_timing_.dns_start = socket_connect_timing->dns_start;
    connect_timing_.dns_end = socket_connect_timing->dns_end;
  }

  connect_timing_.ssl_start = base::TimeTicks::Now();

  // If privacy mode is enabled and the session shard is non-empty, prefix the
  // SSL session shard with "pm/". Otherwise, prefix with "nopm/".
  // TODO(mmenke): Consider moving this up to the socket pool layer, after
  // giving socket pools knowledge of privacy mode.
  SSLClientSocketContext context_with_privacy_mode(
      ssl_client_socket_context().cert_verifier,
      ssl_client_socket_context().channel_id_service,
      ssl_client_socket_context().transport_security_state,
      ssl_client_socket_context().cert_transparency_verifier,
      ssl_client_socket_context().ct_policy_enforcer,
      ssl_client_socket_context().ssl_client_session_cache,
      (params_->privacy_mode() == PRIVACY_MODE_ENABLED ? "pm/" : "nopm/") +
          ssl_client_socket_context().ssl_session_cache_shard);
  if (nested_socket_.get()) {
    DCHECK(!transport_socket_handle_);
    ssl_socket_ = client_socket_factory()->CreateSSLClientSocket(
        std::move(nested_socket_), params_->host_and_port(),
        params_->ssl_config(), context_with_privacy_mode);
    nested_connect_job_.reset();
  } else {
    ssl_socket_ = client_socket_factory()->CreateSSLClientSocket(
        std::move(transport_socket_handle_), params_->host_and_port(),
        params_->ssl_config(), context_with_privacy_mode);
  }
  return ssl_socket_->Connect(callback_);
}

int SSLConnectJob::DoSSLConnectComplete(int result) {
  connect_timing_.ssl_end = base::TimeTicks::Now();

  if (result != OK && !server_address_.address().empty()) {
    connection_attempts_.push_back(ConnectionAttempt(server_address_, result));
    server_address_ = IPEndPoint();
  }

  const std::string& host = params_->host_and_port().host();
  bool tls13_supported = IsTLS13ExperimentHost(host);

  if (result == OK) {
    DCHECK(!connect_timing_.ssl_start.is_null());
    base::TimeDelta connect_duration =
        connect_timing_.ssl_end - connect_timing_.ssl_start;
    UMA_HISTOGRAM_CUSTOM_TIMES("Net.SSL_Connection_Latency_2", connect_duration,
                               base::TimeDelta::FromMilliseconds(1),
                               base::TimeDelta::FromMinutes(1), 100);

    SSLInfo ssl_info;
    bool has_ssl_info = ssl_socket_->GetSSLInfo(&ssl_info);
    DCHECK(has_ssl_info);

    UMA_HISTOGRAM_ENUMERATION(
        "Net.SSLVersion",
        SSLConnectionStatusToVersion(ssl_info.connection_status),
        SSL_CONNECTION_VERSION_MAX);

    uint16_t cipher_suite =
        SSLConnectionStatusToCipherSuite(ssl_info.connection_status);
    base::UmaHistogramSparse("Net.SSL_CipherSuite", cipher_suite);

    if (ssl_info.key_exchange_group != 0) {
      base::UmaHistogramSparse("Net.SSL_KeyExchange.ECDHE",
                               ssl_info.key_exchange_group);
    }

    if (ssl_info.handshake_type == SSLInfo::HANDSHAKE_RESUME) {
      UMA_HISTOGRAM_CUSTOM_TIMES("Net.SSL_Connection_Latency_Resume_Handshake",
                                 connect_duration,
                                 base::TimeDelta::FromMilliseconds(1),
                                 base::TimeDelta::FromMinutes(1), 100);
    } else if (ssl_info.handshake_type == SSLInfo::HANDSHAKE_FULL) {
      UMA_HISTOGRAM_CUSTOM_TIMES("Net.SSL_Connection_Latency_Full_Handshake",
                                 connect_duration,
                                 base::TimeDelta::FromMilliseconds(1),
                                 base::TimeDelta::FromMinutes(1), 100);
    }

    if (tls13_supported) {
      UMA_HISTOGRAM_CUSTOM_TIMES("Net.SSL_Connection_Latency_TLS13Experiment",
                                 connect_duration,
                                 base::TimeDelta::FromMilliseconds(1),
                                 base::TimeDelta::FromMinutes(1), 100);
    }
  }

  // Don't double-count the version interference probes.
  if (!params_->ssl_config().version_interference_probe) {
    base::UmaHistogramSparse("Net.SSL_Connection_Error", std::abs(result));

    if (tls13_supported) {
      base::UmaHistogramSparse("Net.SSL_Connection_Error_TLS13Experiment",
                               std::abs(result));
    }
  }

  if (result == OK || IsCertificateError(result)) {
    SetSocket(std::move(ssl_socket_));
  } else if (result == ERR_SSL_CLIENT_AUTH_CERT_NEEDED) {
    error_response_info_.cert_request_info = new SSLCertRequestInfo;
    ssl_socket_->GetSSLCertRequestInfo(
        error_response_info_.cert_request_info.get());
  }

  return result;
}

SSLConnectJob::State SSLConnectJob::GetInitialState(
    SSLSocketParams::ConnectionType connection_type) {
  switch (connection_type) {
    case SSLSocketParams::DIRECT:
      return STATE_TRANSPORT_CONNECT;
    case SSLSocketParams::HTTP_PROXY:
      return STATE_TUNNEL_CONNECT;
    case SSLSocketParams::SOCKS_PROXY:
      return STATE_SOCKS_CONNECT;
  }
  NOTREACHED();
  return STATE_NONE;
}

int SSLConnectJob::ConnectInternal() {
  next_state_ = GetInitialState(params_->GetConnectionType());
  return DoLoop(OK);
}

void SSLConnectJob::ChangePriorityInternal(RequestPriority priority) {
  if (transport_socket_handle_)
    transport_socket_handle_->SetPriority(priority);
  if (nested_connect_job_)
    nested_connect_job_->ChangePriority(priority);
}

}  // namespace net
