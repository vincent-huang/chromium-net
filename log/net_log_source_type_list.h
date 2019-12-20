// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// NOTE: No header guards are used, since this file is intended to be expanded
// directly within a block where the SOURCE_TYPE macro is defined.
// The following line silences a presubmit warning that would otherwise be
// triggered by this:
// no-include-guard-because-multiply-included
// NOLINT(build/header_guard)

// Used for global events which don't correspond to a particular entity.
SOURCE_TYPE(NONE)

SOURCE_TYPE(URL_REQUEST)
SOURCE_TYPE(PAC_FILE_DECIDER)
SOURCE_TYPE(HTTP_PROXY_CONNECT_JOB)
SOURCE_TYPE(SOCKS_CONNECT_JOB)
SOURCE_TYPE(SSL_CONNECT_JOB)
SOURCE_TYPE(TRANSPORT_CONNECT_JOB)
SOURCE_TYPE(WEB_SOCKET_TRANSPORT_CONNECT_JOB)
SOURCE_TYPE(SOCKET)
SOURCE_TYPE(HTTP2_SESSION)
SOURCE_TYPE(QUIC_SESSION)
SOURCE_TYPE(QUIC_CONNECTION_MIGRATION)
SOURCE_TYPE(QUIC_PORT_MIGRATION)
SOURCE_TYPE(HOST_RESOLVER_IMPL_JOB)
SOURCE_TYPE(DISK_CACHE_ENTRY)
SOURCE_TYPE(MEMORY_CACHE_ENTRY)
SOURCE_TYPE(HTTP_STREAM_JOB)
SOURCE_TYPE(EXPONENTIAL_BACKOFF_THROTTLING)
SOURCE_TYPE(UDP_SOCKET)
SOURCE_TYPE(CERT_VERIFIER_JOB)
SOURCE_TYPE(PROXY_CLIENT_SOCKET)
SOURCE_TYPE(BIDIRECTIONAL_STREAM)
SOURCE_TYPE(NETWORK_QUALITY_ESTIMATOR)
SOURCE_TYPE(HTTP_STREAM_JOB_CONTROLLER)
SOURCE_TYPE(CT_TREE_STATE_TRACKER)
SOURCE_TYPE(SERVER_PUSH_LOOKUP_TRANSACTION)
SOURCE_TYPE(QUIC_STREAM_FACTORY_JOB)
SOURCE_TYPE(HTTP_SERVER_PROPERTIES)
SOURCE_TYPE(HOST_CACHE_PERSISTENCE_MANAGER)
SOURCE_TYPE(TRIAL_CERT_VERIFIER_JOB)
SOURCE_TYPE(COOKIE_STORE)
SOURCE_TYPE(HTTP_AUTH_CONTROLLER)
SOURCE_TYPE(HTTP3_SESSION)
SOURCE_TYPE(QUIC_TRANSPORT_CLIENT)
