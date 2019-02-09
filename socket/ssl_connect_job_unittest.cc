// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/ssl_connect_job.h"

#include <memory>
#include <string>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_task_environment.h"
#include "base/time/time.h"
#include "net/base/auth.h"
#include "net/base/load_timing_info.h"
#include "net/base/net_errors.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/cert/multi_log_ct_verifier.h"
#include "net/dns/mock_host_resolver.h"
#include "net/http/http_auth_handler_factory.h"
#include "net/http/http_network_session.h"
#include "net/http/http_proxy_client_socket_pool.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_server_properties_impl.h"
#include "net/http/transport_security_state.h"
#include "net/log/net_log_source.h"
#include "net/log/net_log_with_source.h"
#include "net/proxy_resolution/proxy_resolution_service.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/connect_job_test_util.h"
#include "net/socket/next_proto.h"
#include "net/socket/socket_tag.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/socks_connect_job.h"
#include "net/socket/transport_client_socket_pool.h"
#include "net/socket/transport_connect_job.h"
#include "net/ssl/ssl_config_service_defaults.h"
#include "net/test/gtest_util.h"
#include "net/test/test_certificate_data.h"
#include "net/test/test_with_scoped_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

const int kMaxSockets = 32;
const int kMaxSocketsPerGroup = 6;
const char kGroupName[] = "a";

// Just check that all connect times are set to base::TimeTicks::Now(), for
// tests that don't update the mocked out time.
void CheckConnectTimesSet(const LoadTimingInfo::ConnectTiming& connect_timing) {
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.dns_start);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.dns_end);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.connect_start);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.ssl_start);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.ssl_end);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.connect_end);
}

// Just check that all connect times are set to base::TimeTicks::Now(), except
// for DNS times, for tests that don't update the mocked out time and use a
// proxy.
void CheckConnectTimesExceptDnsSet(
    const LoadTimingInfo::ConnectTiming& connect_timing) {
  EXPECT_TRUE(connect_timing.dns_start.is_null());
  EXPECT_TRUE(connect_timing.dns_end.is_null());
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.connect_start);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.ssl_start);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.ssl_end);
  EXPECT_EQ(base::TimeTicks::Now(), connect_timing.connect_end);
}

class SSLConnectJobTest : public WithScopedTaskEnvironment,
                          public testing::Test {
 public:
  SSLConnectJobTest()
      : WithScopedTaskEnvironment(
            base::test::ScopedTaskEnvironment::MainThreadType::MOCK_TIME,
            base::test::ScopedTaskEnvironment::NowSource::
                MAIN_THREAD_MOCK_TIME),
        proxy_resolution_service_(ProxyResolutionService::CreateDirect()),
        ssl_config_service_(new SSLConfigServiceDefaults),
        http_auth_handler_factory_(
            HttpAuthHandlerFactory::CreateDefault(&host_resolver_)),
        session_(CreateNetworkSession()),
        ssl_client_socket_context_(&cert_verifier_,
                                   nullptr /* channel_id_service_arg */,
                                   &transport_security_state_,
                                   &ct_verifier_,
                                   &ct_policy_enforcer_,
                                   nullptr /* ssl_client_session_cache */,
                                   std::string() /* ssl_session_cache_shard */),
        direct_transport_socket_params_(
            new TransportSocketParams(HostPortPair("host", 443),
                                      false,
                                      OnHostResolutionCallback())),
        transport_socket_pool_(kMaxSockets,
                               kMaxSocketsPerGroup,
                               &socket_factory_,
                               &host_resolver_,
                               nullptr /* cert_verifier */,
                               nullptr /* channel_id_server */,
                               nullptr /* transport_security_state */,
                               nullptr /* cert_transparency_verifier */,
                               nullptr /* ct_policy_enforcer */,
                               nullptr /* ssl_client_session_cache */,
                               std::string() /* ssl_session_cache_shard */,
                               nullptr /* ssl_config_service */,
                               nullptr /* socket_performance_watcher_factory */,
                               nullptr /* network_quality_estimator */,
                               nullptr /* net_log */),
        proxy_transport_socket_params_(
            new TransportSocketParams(HostPortPair("proxy", 443),
                                      false,
                                      OnHostResolutionCallback())),
        socks_socket_params_(
            new SOCKSSocketParams(proxy_transport_socket_params_,
                                  true,
                                  HostPortPair("sockshost", 443),
                                  TRAFFIC_ANNOTATION_FOR_TESTS)),
        http_proxy_socket_params_(
            new HttpProxySocketParams(proxy_transport_socket_params_,
                                      nullptr /* ssl_params */,
                                      quic::QUIC_VERSION_UNSUPPORTED,
                                      std::string(),
                                      HostPortPair("host", 80),
                                      session_->http_auth_cache(),
                                      session_->http_auth_handler_factory(),
                                      session_->spdy_session_pool(),
                                      session_->quic_stream_factory(),
                                      /*is_trusted_proxy=*/false,
                                      /*tunnel=*/true,
                                      TRAFFIC_ANNOTATION_FOR_TESTS)),
        http_proxy_socket_pool_(kMaxSockets,
                                kMaxSocketsPerGroup,
                                &transport_socket_pool_,
                                nullptr /* ssl_pool */,
                                nullptr /* proxy_delegate */,
                                nullptr /* network_quality_estimator */,
                                nullptr /* net_log */) {
    ssl_config_service_->GetSSLConfig(&ssl_config_);

    // Set an initial delay to ensure that the first call to TimeTicks::Now()
    // before incrementing the counter does not return a null value.
    FastForwardBy(base::TimeDelta::FromSeconds(1));
  }

  ~SSLConnectJobTest() override = default;

  std::unique_ptr<ConnectJob> CreateConnectJob(
      TestConnectJobDelegate* test_delegate,
      ProxyServer::Scheme proxy_scheme = ProxyServer::SCHEME_DIRECT,
      RequestPriority priority = DEFAULT_PRIORITY) {
    return std::make_unique<SSLConnectJob>(
        priority,
        CommonConnectJobParams(
            kGroupName, SocketTag(), true /* respect_limits */,
            &socket_factory_, &host_resolver_, ssl_client_socket_context_,
            nullptr /* socket_performance_watcher */,
            nullptr /* network_quality_estimator */, nullptr /* net_log */,
            nullptr /* websocket_lock_endpoint_manager */),
        SSLParams(proxy_scheme),
        proxy_scheme == ProxyServer::SCHEME_HTTP ? &http_proxy_socket_pool_
                                                 : nullptr,
        test_delegate);
  }

  scoped_refptr<SSLSocketParams> SSLParams(ProxyServer::Scheme proxy) {
    return base::MakeRefCounted<SSLSocketParams>(
        proxy == ProxyServer::SCHEME_DIRECT ? direct_transport_socket_params_
                                            : nullptr,
        proxy == ProxyServer::SCHEME_SOCKS5 ? socks_socket_params_ : nullptr,
        proxy == ProxyServer::SCHEME_HTTP ? http_proxy_socket_params_ : nullptr,
        HostPortPair("host", 443), ssl_config_, PRIVACY_MODE_DISABLED);
  }

  void AddAuthToCache() {
    const base::string16 kFoo(base::ASCIIToUTF16("foo"));
    const base::string16 kBar(base::ASCIIToUTF16("bar"));
    session_->http_auth_cache()->Add(
        GURL("http://proxy:443/"), "MyRealm1", HttpAuth::AUTH_SCHEME_BASIC,
        "Basic realm=MyRealm1", AuthCredentials(kFoo, kBar), "/");
  }

  HttpNetworkSession* CreateNetworkSession() {
    HttpNetworkSession::Context session_context;
    session_context.host_resolver = &host_resolver_;
    session_context.cert_verifier = &cert_verifier_;
    session_context.transport_security_state = &transport_security_state_;
    session_context.cert_transparency_verifier = &ct_verifier_;
    session_context.ct_policy_enforcer = &ct_policy_enforcer_;
    session_context.proxy_resolution_service = proxy_resolution_service_.get();
    session_context.client_socket_factory = &socket_factory_;
    session_context.ssl_config_service = ssl_config_service_.get();
    session_context.http_auth_handler_factory =
        http_auth_handler_factory_.get();
    session_context.http_server_properties = &http_server_properties_;
    return new HttpNetworkSession(HttpNetworkSession::Params(),
                                  session_context);
  }

 protected:
  MockClientSocketFactory socket_factory_;
  MockCachingHostResolver host_resolver_;
  MockCertVerifier cert_verifier_;
  TransportSecurityState transport_security_state_;
  MultiLogCTVerifier ct_verifier_;
  DefaultCTPolicyEnforcer ct_policy_enforcer_;
  const std::unique_ptr<ProxyResolutionService> proxy_resolution_service_;
  const std::unique_ptr<SSLConfigService> ssl_config_service_;
  const std::unique_ptr<HttpAuthHandlerFactory> http_auth_handler_factory_;
  HttpServerPropertiesImpl http_server_properties_;
  const std::unique_ptr<HttpNetworkSession> session_;
  SSLClientSocketContext ssl_client_socket_context_;

  scoped_refptr<TransportSocketParams> direct_transport_socket_params_;
  TransportClientSocketPool transport_socket_pool_;

  scoped_refptr<TransportSocketParams> proxy_transport_socket_params_;
  scoped_refptr<SOCKSSocketParams> socks_socket_params_;
  scoped_refptr<HttpProxySocketParams> http_proxy_socket_params_;

  HttpProxyClientSocketPool http_proxy_socket_pool_;

  SSLConfig ssl_config_;
};

TEST_F(SSLConnectJobTest, TCPFail) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    host_resolver_.set_synchronous_mode(io_mode == SYNCHRONOUS);
    StaticSocketDataProvider data;
    data.set_connect_data(MockConnect(io_mode, ERR_CONNECTION_FAILED));
    socket_factory_.AddSocketDataProvider(&data);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> ssl_connect_job =
        CreateConnectJob(&test_delegate);
    test_delegate.StartJobExpectingResult(
        ssl_connect_job.get(), ERR_CONNECTION_FAILED, io_mode == SYNCHRONOUS);
    EXPECT_FALSE(test_delegate.socket());
    ClientSocketHandle handle;
    ssl_connect_job->GetAdditionalErrorState(&handle);
    EXPECT_FALSE(handle.is_ssl_error());
    ASSERT_EQ(1u, handle.connection_attempts().size());
    EXPECT_THAT(handle.connection_attempts()[0].result,
                test::IsError(ERR_CONNECTION_FAILED));
  }
}

TEST_F(SSLConnectJobTest, BasicDirectSync) {
  host_resolver_.set_synchronous_mode(true);
  StaticSocketDataProvider data;
  data.set_connect_data(MockConnect(SYNCHRONOUS, OK));
  socket_factory_.AddSocketDataProvider(&data);
  SSLSocketDataProvider ssl(SYNCHRONOUS, OK);
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate, ProxyServer::SCHEME_DIRECT, MEDIUM);

  test_delegate.StartJobExpectingResult(ssl_connect_job.get(), OK,
                                        true /* expect_sync_result */);
  EXPECT_EQ(MEDIUM, host_resolver_.last_request_priority());

  ClientSocketHandle handle;
  ssl_connect_job->GetAdditionalErrorState(&handle);
  EXPECT_EQ(0u, handle.connection_attempts().size());
  CheckConnectTimesSet(ssl_connect_job->connect_timing());
}

TEST_F(SSLConnectJobTest, BasicDirectAsync) {
  host_resolver_.set_ondemand_mode(true);
  base::TimeTicks start_time = base::TimeTicks::Now();
  StaticSocketDataProvider data;
  data.set_connect_data(MockConnect(ASYNC, OK));
  socket_factory_.AddSocketDataProvider(&data);
  SSLSocketDataProvider ssl(ASYNC, OK);
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate, ProxyServer::SCHEME_DIRECT, MEDIUM);
  EXPECT_THAT(ssl_connect_job->Connect(), test::IsError(ERR_IO_PENDING));
  EXPECT_TRUE(host_resolver_.has_pending_requests());
  EXPECT_EQ(MEDIUM, host_resolver_.last_request_priority());
  FastForwardBy(base::TimeDelta::FromSeconds(5));

  base::TimeTicks resolve_complete_time = base::TimeTicks::Now();
  host_resolver_.ResolveAllPending();
  EXPECT_THAT(test_delegate.WaitForResult(), test::IsOk());

  ClientSocketHandle handle;
  ssl_connect_job->GetAdditionalErrorState(&handle);
  EXPECT_EQ(0u, handle.connection_attempts().size());

  // Check times. Since time is mocked out, all times will be the same, except
  // |dns_start|, which is the only one recorded before the FastForwardBy()
  // call. The test classes don't allow any other phases to be triggered on
  // demand, or delayed by a set interval.
  EXPECT_EQ(start_time, ssl_connect_job->connect_timing().dns_start);
  EXPECT_EQ(resolve_complete_time, ssl_connect_job->connect_timing().dns_end);
  EXPECT_EQ(resolve_complete_time,
            ssl_connect_job->connect_timing().connect_start);
  EXPECT_EQ(resolve_complete_time, ssl_connect_job->connect_timing().ssl_start);
  EXPECT_EQ(resolve_complete_time, ssl_connect_job->connect_timing().ssl_end);
  EXPECT_EQ(resolve_complete_time,
            ssl_connect_job->connect_timing().connect_end);
}

TEST_F(SSLConnectJobTest, DirectHasEstablishedConnection) {
  host_resolver_.set_ondemand_mode(true);
  StaticSocketDataProvider data;
  data.set_connect_data(MockConnect(ASYNC, OK));
  socket_factory_.AddSocketDataProvider(&data);

  // SSL negotiation hangs. Value returned after SSL negotiation is complete
  // doesn't matter, as HasEstablishedConnection() may only be used between job
  // start and job complete.
  SSLSocketDataProvider ssl(SYNCHRONOUS, ERR_IO_PENDING);
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate, ProxyServer::SCHEME_DIRECT, MEDIUM);
  EXPECT_THAT(ssl_connect_job->Connect(), test::IsError(ERR_IO_PENDING));
  EXPECT_TRUE(host_resolver_.has_pending_requests());
  EXPECT_EQ(LOAD_STATE_RESOLVING_HOST, ssl_connect_job->GetLoadState());
  EXPECT_FALSE(ssl_connect_job->HasEstablishedConnection());

  // DNS resolution completes, and then the ConnectJob tries to connect the
  // socket, which should succeed asynchronously.
  host_resolver_.ResolveNow(1);
  EXPECT_EQ(LOAD_STATE_CONNECTING, ssl_connect_job->GetLoadState());
  EXPECT_FALSE(ssl_connect_job->HasEstablishedConnection());

  // Spinning the message loop causes the socket to finish connecting. The SSL
  // handshake should start and hang.
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_EQ(LOAD_STATE_SSL_HANDSHAKE, ssl_connect_job->GetLoadState());
  EXPECT_TRUE(ssl_connect_job->HasEstablishedConnection());
}

TEST_F(SSLConnectJobTest, RequestPriority) {
  host_resolver_.set_ondemand_mode(true);
  // Make resolution eventually fail, so old jobs can easily be removed from the
  // socket pool.
  host_resolver_.rules()->AddSimulatedFailure(
      direct_transport_socket_params_->destination().hostname());
  for (int initial_priority = MINIMUM_PRIORITY;
       initial_priority <= MAXIMUM_PRIORITY; ++initial_priority) {
    SCOPED_TRACE(initial_priority);
    for (int new_priority = MINIMUM_PRIORITY; new_priority <= MAXIMUM_PRIORITY;
         ++new_priority) {
      SCOPED_TRACE(new_priority);
      if (initial_priority == new_priority)
        continue;
      TestConnectJobDelegate test_delegate;
      std::unique_ptr<ConnectJob> ssl_connect_job =
          CreateConnectJob(&test_delegate, ProxyServer::SCHEME_DIRECT,
                           static_cast<RequestPriority>(initial_priority));
      EXPECT_THAT(ssl_connect_job->Connect(), test::IsError(ERR_IO_PENDING));
      EXPECT_TRUE(host_resolver_.has_pending_requests());
      int request_id = host_resolver_.num_resolve();
      EXPECT_EQ(initial_priority, host_resolver_.request_priority(request_id));

      ssl_connect_job->ChangePriority(
          static_cast<RequestPriority>(new_priority));
      EXPECT_EQ(new_priority, host_resolver_.request_priority(request_id));

      ssl_connect_job->ChangePriority(
          static_cast<RequestPriority>(initial_priority));
      EXPECT_EQ(initial_priority, host_resolver_.request_priority(request_id));

      // Complete the resolution, which should result in destroying the
      // connecting socket.
      host_resolver_.ResolveAllPending();
      ASSERT_THAT(test_delegate.WaitForResult(),
                  test::IsError(ERR_NAME_NOT_RESOLVED));
    }
  }
}

TEST_F(SSLConnectJobTest, DirectCertError) {
  StaticSocketDataProvider data;
  socket_factory_.AddSocketDataProvider(&data);
  SSLSocketDataProvider ssl(ASYNC, ERR_CERT_COMMON_NAME_INVALID);
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate(
      TestConnectJobDelegate::SocketExpected::ALWAYS);
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate);

  test_delegate.StartJobExpectingResult(ssl_connect_job.get(),
                                        ERR_CERT_COMMON_NAME_INVALID,
                                        false /* expect_sync_result */);
  ClientSocketHandle handle;
  ssl_connect_job->GetAdditionalErrorState(&handle);
  EXPECT_TRUE(handle.is_ssl_error());
  ASSERT_EQ(1u, handle.connection_attempts().size());
  EXPECT_THAT(handle.connection_attempts()[0].result,
              test::IsError(ERR_CERT_COMMON_NAME_INVALID));
  CheckConnectTimesSet(ssl_connect_job->connect_timing());
}

TEST_F(SSLConnectJobTest, DirectSSLError) {
  StaticSocketDataProvider data;
  socket_factory_.AddSocketDataProvider(&data);
  SSLSocketDataProvider ssl(ASYNC, ERR_SSL_PROTOCOL_ERROR);
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate);

  test_delegate.StartJobExpectingResult(ssl_connect_job.get(),
                                        ERR_SSL_PROTOCOL_ERROR,
                                        false /* expect_sync_result */);
  ClientSocketHandle handle;
  ssl_connect_job->GetAdditionalErrorState(&handle);
  EXPECT_TRUE(handle.is_ssl_error());
  ASSERT_EQ(1u, handle.connection_attempts().size());
  EXPECT_THAT(handle.connection_attempts()[0].result,
              test::IsError(ERR_SSL_PROTOCOL_ERROR));
}

TEST_F(SSLConnectJobTest, DirectWithNPN) {
  StaticSocketDataProvider data;
  socket_factory_.AddSocketDataProvider(&data);
  SSLSocketDataProvider ssl(ASYNC, OK);
  ssl.next_proto = kProtoHTTP11;
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate);

  test_delegate.StartJobExpectingResult(ssl_connect_job.get(), OK,
                                        false /* expect_sync_result */);
  EXPECT_TRUE(test_delegate.socket()->WasAlpnNegotiated());
  CheckConnectTimesSet(ssl_connect_job->connect_timing());
}

TEST_F(SSLConnectJobTest, DirectGotHTTP2) {
  StaticSocketDataProvider data;
  socket_factory_.AddSocketDataProvider(&data);
  SSLSocketDataProvider ssl(ASYNC, OK);
  ssl.next_proto = kProtoHTTP2;
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate);

  test_delegate.StartJobExpectingResult(ssl_connect_job.get(), OK,
                                        false /* expect_sync_result */);
  EXPECT_TRUE(test_delegate.socket()->WasAlpnNegotiated());
  EXPECT_EQ(kProtoHTTP2, test_delegate.socket()->GetNegotiatedProtocol());
  CheckConnectTimesSet(ssl_connect_job->connect_timing());
}

TEST_F(SSLConnectJobTest, SOCKSFail) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    host_resolver_.set_synchronous_mode(io_mode == SYNCHRONOUS);
    StaticSocketDataProvider data;
    data.set_connect_data(MockConnect(io_mode, ERR_CONNECTION_FAILED));
    socket_factory_.AddSocketDataProvider(&data);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> ssl_connect_job =
        CreateConnectJob(&test_delegate, ProxyServer::SCHEME_SOCKS5);
    test_delegate.StartJobExpectingResult(ssl_connect_job.get(),
                                          ERR_PROXY_CONNECTION_FAILED,
                                          io_mode == SYNCHRONOUS);

    ClientSocketHandle handle;
    ssl_connect_job->GetAdditionalErrorState(&handle);
    EXPECT_FALSE(handle.is_ssl_error());
    EXPECT_EQ(0u, handle.connection_attempts().size());
  }
}

TEST_F(SSLConnectJobTest, SOCKSBasic) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    const char kSOCKS5Request[] = {0x05, 0x01, 0x00, 0x03, 0x09, 's',
                                   'o',  'c',  'k',  's',  'h',  'o',
                                   's',  't',  0x01, 0xBB};

    MockWrite writes[] = {
        MockWrite(io_mode, kSOCKS5GreetRequest, kSOCKS5GreetRequestLength),
        MockWrite(io_mode, kSOCKS5Request, base::size(kSOCKS5Request)),
    };

    MockRead reads[] = {
        MockRead(io_mode, kSOCKS5GreetResponse, kSOCKS5GreetResponseLength),
        MockRead(io_mode, kSOCKS5OkResponse, kSOCKS5OkResponseLength),
    };

    host_resolver_.set_synchronous_mode(io_mode == SYNCHRONOUS);
    StaticSocketDataProvider data(reads, writes);
    data.set_connect_data(MockConnect(io_mode, OK));
    socket_factory_.AddSocketDataProvider(&data);
    SSLSocketDataProvider ssl(io_mode, OK);
    socket_factory_.AddSSLSocketDataProvider(&ssl);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> ssl_connect_job =
        CreateConnectJob(&test_delegate, ProxyServer::SCHEME_SOCKS5);
    test_delegate.StartJobExpectingResult(ssl_connect_job.get(), OK,
                                          io_mode == SYNCHRONOUS);
    CheckConnectTimesExceptDnsSet(ssl_connect_job->connect_timing());
  }
}

TEST_F(SSLConnectJobTest, SOCKSHasEstablishedConnection) {
  const char kSOCKS5Request[] = {0x05, 0x01, 0x00, 0x03, 0x09, 's', 'o',  'c',
                                 'k',  's',  'h',  'o',  's',  't', 0x01, 0xBB};

  MockWrite writes[] = {
      MockWrite(SYNCHRONOUS, kSOCKS5GreetRequest, kSOCKS5GreetRequestLength, 0),
      MockWrite(SYNCHRONOUS, kSOCKS5Request, base::size(kSOCKS5Request), 3),
  };

  MockRead reads[] = {
      // Pause so can probe current state.
      MockRead(ASYNC, ERR_IO_PENDING, 1),
      MockRead(ASYNC, kSOCKS5GreetResponse, kSOCKS5GreetResponseLength, 2),
      MockRead(SYNCHRONOUS, kSOCKS5OkResponse, kSOCKS5OkResponseLength, 4),
  };

  host_resolver_.set_ondemand_mode(true);
  SequencedSocketData data(reads, writes);
  data.set_connect_data(MockConnect(ASYNC, OK));
  socket_factory_.AddSocketDataProvider(&data);

  // SSL negotiation hangs. Value returned after SSL negotiation is complete
  // doesn't matter, as HasEstablishedConnection() may only be used between job
  // start and job complete.
  SSLSocketDataProvider ssl(SYNCHRONOUS, ERR_IO_PENDING);
  socket_factory_.AddSSLSocketDataProvider(&ssl);

  TestConnectJobDelegate test_delegate;
  std::unique_ptr<ConnectJob> ssl_connect_job =
      CreateConnectJob(&test_delegate, ProxyServer::SCHEME_SOCKS5);
  EXPECT_THAT(ssl_connect_job->Connect(), test::IsError(ERR_IO_PENDING));
  EXPECT_TRUE(host_resolver_.has_pending_requests());
  EXPECT_EQ(LOAD_STATE_RESOLVING_HOST, ssl_connect_job->GetLoadState());
  EXPECT_FALSE(ssl_connect_job->HasEstablishedConnection());

  // DNS resolution completes, and then the ConnectJob tries to connect the
  // socket, which should succeed asynchronously.
  host_resolver_.ResolveNow(1);
  EXPECT_EQ(LOAD_STATE_CONNECTING, ssl_connect_job->GetLoadState());
  EXPECT_FALSE(ssl_connect_job->HasEstablishedConnection());

  // Spin the message loop until the first read of the handshake.
  // HasEstablishedConnection() should return true, as a TCP connection has been
  // successfully established by this point.
  data.RunUntilPaused();
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_EQ(LOAD_STATE_CONNECTING, ssl_connect_job->GetLoadState());
  EXPECT_TRUE(ssl_connect_job->HasEstablishedConnection());

  // Finish up the handshake, and spin the message loop until the SSL handshake
  // starts and hang.
  data.Resume();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(test_delegate.has_result());
  EXPECT_EQ(LOAD_STATE_SSL_HANDSHAKE, ssl_connect_job->GetLoadState());
  EXPECT_TRUE(ssl_connect_job->HasEstablishedConnection());
}

TEST_F(SSLConnectJobTest, SOCKSRequestPriority) {
  host_resolver_.set_ondemand_mode(true);
  // Make resolution eventually fail, so old jobs can easily be removed from the
  // socket pool.
  host_resolver_.rules()->AddSimulatedFailure(
      socks_socket_params_->transport_params()->destination().hostname());
  for (int initial_priority = MINIMUM_PRIORITY;
       initial_priority <= MAXIMUM_PRIORITY; ++initial_priority) {
    SCOPED_TRACE(initial_priority);
    for (int new_priority = MINIMUM_PRIORITY; new_priority <= MAXIMUM_PRIORITY;
         ++new_priority) {
      SCOPED_TRACE(new_priority);
      if (initial_priority == new_priority)
        continue;
      TestConnectJobDelegate test_delegate;
      std::unique_ptr<ConnectJob> ssl_connect_job =
          CreateConnectJob(&test_delegate, ProxyServer::SCHEME_SOCKS5,
                           static_cast<RequestPriority>(initial_priority));
      EXPECT_THAT(ssl_connect_job->Connect(), test::IsError(ERR_IO_PENDING));
      EXPECT_TRUE(host_resolver_.has_pending_requests());
      int request_id = host_resolver_.num_resolve();
      EXPECT_EQ(initial_priority, host_resolver_.request_priority(request_id));

      ssl_connect_job->ChangePriority(
          static_cast<RequestPriority>(new_priority));
      EXPECT_EQ(new_priority, host_resolver_.request_priority(request_id));

      ssl_connect_job->ChangePriority(
          static_cast<RequestPriority>(initial_priority));
      EXPECT_EQ(initial_priority, host_resolver_.request_priority(request_id));

      // Complete the resolution, which should result in destroying the
      // connecting socket.
      host_resolver_.ResolveAllPending();
      ASSERT_THAT(test_delegate.WaitForResult(),
                  test::IsError(ERR_PROXY_CONNECTION_FAILED));
    }
  }
}

TEST_F(SSLConnectJobTest, HttpProxyFail) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    host_resolver_.set_synchronous_mode(io_mode == SYNCHRONOUS);
    StaticSocketDataProvider data;
    data.set_connect_data(MockConnect(io_mode, ERR_CONNECTION_FAILED));
    socket_factory_.AddSocketDataProvider(&data);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> ssl_connect_job =
        CreateConnectJob(&test_delegate, ProxyServer::SCHEME_HTTP);
    test_delegate.StartJobExpectingResult(ssl_connect_job.get(),
                                          ERR_PROXY_CONNECTION_FAILED,
                                          io_mode == SYNCHRONOUS);

    ClientSocketHandle handle;
    ssl_connect_job->GetAdditionalErrorState(&handle);
    EXPECT_FALSE(handle.is_ssl_error());
    EXPECT_EQ(0u, handle.connection_attempts().size());
  }
}

TEST_F(SSLConnectJobTest, HttpProxyBasic) {
  for (IoMode io_mode : {SYNCHRONOUS, ASYNC}) {
    SCOPED_TRACE(io_mode);
    host_resolver_.set_synchronous_mode(io_mode == SYNCHRONOUS);
    MockWrite writes[] = {
        MockWrite(io_mode,
                  "CONNECT host:80 HTTP/1.1\r\n"
                  "Host: host:80\r\n"
                  "Proxy-Connection: keep-alive\r\n"
                  "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };
    MockRead reads[] = {
        MockRead(io_mode, "HTTP/1.1 200 Connection Established\r\n\r\n"),
    };
    StaticSocketDataProvider data(reads, writes);
    data.set_connect_data(MockConnect(io_mode, OK));
    socket_factory_.AddSocketDataProvider(&data);
    AddAuthToCache();
    SSLSocketDataProvider ssl(io_mode, OK);
    socket_factory_.AddSSLSocketDataProvider(&ssl);

    TestConnectJobDelegate test_delegate;
    std::unique_ptr<ConnectJob> ssl_connect_job =
        CreateConnectJob(&test_delegate, ProxyServer::SCHEME_HTTP);
    test_delegate.StartJobExpectingResult(ssl_connect_job.get(), OK,
                                          io_mode == SYNCHRONOUS);
    CheckConnectTimesExceptDnsSet(ssl_connect_job->connect_timing());
  }
}

TEST_F(SSLConnectJobTest, HttpProxyRequestPriority) {
  host_resolver_.set_ondemand_mode(true);
  // Make resolution eventually fail, so old jobs can easily be removed from the
  // socket pool.
  host_resolver_.rules()->AddSimulatedFailure(
      socks_socket_params_->transport_params()->destination().hostname());
  for (int initial_priority = MINIMUM_PRIORITY;
       initial_priority <= MAXIMUM_PRIORITY; ++initial_priority) {
    SCOPED_TRACE(initial_priority);
    for (int new_priority = MINIMUM_PRIORITY; new_priority <= MAXIMUM_PRIORITY;
         ++new_priority) {
      SCOPED_TRACE(new_priority);
      if (initial_priority == new_priority)
        continue;
      TestConnectJobDelegate test_delegate;
      std::unique_ptr<ConnectJob> ssl_connect_job =
          CreateConnectJob(&test_delegate, ProxyServer::SCHEME_HTTP,
                           static_cast<RequestPriority>(initial_priority));
      EXPECT_THAT(ssl_connect_job->Connect(), test::IsError(ERR_IO_PENDING));
      EXPECT_TRUE(host_resolver_.has_pending_requests());
      int request_id = host_resolver_.num_resolve();
      EXPECT_EQ(initial_priority, host_resolver_.request_priority(request_id));

      ssl_connect_job->ChangePriority(
          static_cast<RequestPriority>(new_priority));
      EXPECT_EQ(new_priority, host_resolver_.request_priority(request_id));

      ssl_connect_job->ChangePriority(
          static_cast<RequestPriority>(initial_priority));
      EXPECT_EQ(initial_priority, host_resolver_.request_priority(request_id));

      // Complete the resolution, which should result in destroying the
      // connecting socket.
      host_resolver_.ResolveAllPending();
      ASSERT_THAT(test_delegate.WaitForResult(),
                  test::IsError(ERR_PROXY_CONNECTION_FAILED));
    }
  }
}

}  // namespace
}  // namespace net
