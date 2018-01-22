// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/test_proxy_delegate.h"

#include "net/proxy_resolution/proxy_info.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

TestProxyDelegate::TestProxyDelegate() = default;

TestProxyDelegate::~TestProxyDelegate() = default;

void TestProxyDelegate::OnResolveProxy(
    const GURL& url,
    const std::string& method,
    const ProxyRetryInfoMap& proxy_retry_info,
    ProxyInfo* result) {
  // Only set |alternative_proxy_server_| as the alternative proxy if the
  // ProxyService has not marked it as bad.
  ProxyInfo alternative_proxy_info;
  alternative_proxy_info.UseProxyServer(alternative_proxy_server_);
  alternative_proxy_info.DeprioritizeBadProxies(proxy_retry_info);
  if (!alternative_proxy_info.is_empty())
    result->SetAlternativeProxy(alternative_proxy_info.proxy_server());
}

void TestProxyDelegate::OnFallback(const ProxyServer& bad_proxy,
                                   int net_error) {}

bool TestProxyDelegate::IsTrustedSpdyProxy(const ProxyServer& proxy_server) {
  return proxy_server.is_valid() && trusted_spdy_proxy_ == proxy_server;
}

}  // namespace net
