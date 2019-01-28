// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_SSL_CONNECT_JOB_H_
#define NET_SOCKET_SSL_CONNECT_JOB_H_

#include <memory>
#include <string>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "net/base/completion_once_callback.h"
#include "net/base/completion_repeating_callback.h"
#include "net/base/net_export.h"
#include "net/base/privacy_mode.h"
#include "net/http/http_response_info.h"
#include "net/socket/connect_job.h"
#include "net/socket/connection_attempts.h"
#include "net/socket/ssl_client_socket.h"
#include "net/ssl/ssl_config_service.h"

namespace net {

class HostPortPair;
class HttpProxyClientSocketPool;
class HttpProxySocketParams;
class SOCKSSocketParams;
class TransportClientSocketPool;
class TransportSocketParams;

class NET_EXPORT_PRIVATE SSLSocketParams
    : public base::RefCounted<SSLSocketParams> {
 public:
  enum ConnectionType { DIRECT, SOCKS_PROXY, HTTP_PROXY };

  // Exactly one of |direct_params|, |socks_proxy_params|, and
  // |http_proxy_params| must be non-NULL.
  SSLSocketParams(const scoped_refptr<TransportSocketParams>& direct_params,
                  const scoped_refptr<SOCKSSocketParams>& socks_proxy_params,
                  const scoped_refptr<HttpProxySocketParams>& http_proxy_params,
                  const HostPortPair& host_and_port,
                  const SSLConfig& ssl_config,
                  PrivacyMode privacy_mode);

  // Returns the type of the underlying connection.
  ConnectionType GetConnectionType() const;

  // Must be called only when GetConnectionType() returns DIRECT.
  const scoped_refptr<TransportSocketParams>& GetDirectConnectionParams() const;

  // Must be called only when GetConnectionType() returns SOCKS_PROXY.
  const scoped_refptr<SOCKSSocketParams>& GetSocksProxyConnectionParams() const;

  // Must be called only when GetConnectionType() returns HTTP_PROXY.
  const scoped_refptr<HttpProxySocketParams>& GetHttpProxyConnectionParams()
      const;

  const HostPortPair& host_and_port() const { return host_and_port_; }
  const SSLConfig& ssl_config() const { return ssl_config_; }
  PrivacyMode privacy_mode() const { return privacy_mode_; }

 private:
  friend class base::RefCounted<SSLSocketParams>;
  ~SSLSocketParams();

  const scoped_refptr<TransportSocketParams> direct_params_;
  const scoped_refptr<SOCKSSocketParams> socks_proxy_params_;
  const scoped_refptr<HttpProxySocketParams> http_proxy_params_;
  const HostPortPair host_and_port_;
  const SSLConfig ssl_config_;
  const PrivacyMode privacy_mode_;

  DISALLOW_COPY_AND_ASSIGN(SSLSocketParams);
};

// SSLConnectJob establishes a connection, through a proxy if needed, and then
// handles the SSL handshake. It returns an SSLClientSocket on success.
class NET_EXPORT_PRIVATE SSLConnectJob : public ConnectJob {
 public:
  // Note: the SSLConnectJob does not own |messenger| so it must outlive the
  // job.
  SSLConnectJob(RequestPriority priority,
                const CommonConnectJobParams& common_connect_job_params,
                const scoped_refptr<SSLSocketParams>& params,
                TransportClientSocketPool* transport_pool,
                TransportClientSocketPool* socks_pool,
                HttpProxyClientSocketPool* http_proxy_pool,
                Delegate* delegate);
  ~SSLConnectJob() override;

  // ConnectJob methods.
  LoadState GetLoadState() const override;

  void GetAdditionalErrorState(ClientSocketHandle* handle) override;

  // Returns the connection timeout that will be used by a HttpProxyConnectJob
  // created with the specified parameters, given current network conditions.
  static base::TimeDelta ConnectionTimeout(
      const SSLSocketParams& params,
      const NetworkQualityEstimator* network_quality_estimator);

 private:
  enum State {
    STATE_TRANSPORT_CONNECT,
    STATE_TRANSPORT_CONNECT_COMPLETE,
    STATE_SOCKS_CONNECT,
    STATE_SOCKS_CONNECT_COMPLETE,
    STATE_TUNNEL_CONNECT,
    STATE_TUNNEL_CONNECT_COMPLETE,
    STATE_SSL_CONNECT,
    STATE_SSL_CONNECT_COMPLETE,
    STATE_NONE,
  };

  void OnIOComplete(int result);

  // Runs the state transition loop.
  int DoLoop(int result);

  int DoTransportConnect();
  int DoTransportConnectComplete(int result);
  int DoSOCKSConnect();
  int DoSOCKSConnectComplete(int result);
  int DoTunnelConnect();
  int DoTunnelConnectComplete(int result);
  int DoSSLConnect();
  int DoSSLConnectComplete(int result);

  // Returns the initial state for the state machine based on the
  // |connection_type|.
  static State GetInitialState(SSLSocketParams::ConnectionType connection_type);

  // Starts the SSL connection process.  Returns OK on success and
  // ERR_IO_PENDING if it cannot immediately service the request.
  // Otherwise, it returns a net error code.
  int ConnectInternal() override;

  void ChangePriorityInternal(RequestPriority priority) override;

  scoped_refptr<SSLSocketParams> params_;
  TransportClientSocketPool* const transport_pool_;
  TransportClientSocketPool* const socks_pool_;
  HttpProxyClientSocketPool* const http_proxy_pool_;

  State next_state_;
  CompletionRepeatingCallback callback_;
  std::unique_ptr<ClientSocketHandle> transport_socket_handle_;
  std::unique_ptr<SSLClientSocket> ssl_socket_;

  HttpResponseInfo error_response_info_;

  ConnectionAttempts connection_attempts_;
  // The address of the server the connect job is connected to. Populated if
  // and only if the connect job is connected *directly* to the server (not
  // through an HTTPS CONNECT request or a SOCKS proxy).
  IPEndPoint server_address_;

  DISALLOW_COPY_AND_ASSIGN(SSLConnectJob);
};

}  // namespace net

#endif  // NET_SOCKET_SSL_CONNECT_JOB_H_
