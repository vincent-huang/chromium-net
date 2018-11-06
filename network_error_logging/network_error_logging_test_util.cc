// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/network_error_logging/network_error_logging_test_util.h"

#include <algorithm>

namespace net {

TestNetworkErrorLoggingService::TestNetworkErrorLoggingService() = default;
TestNetworkErrorLoggingService::~TestNetworkErrorLoggingService() = default;

void TestNetworkErrorLoggingService::OnHeader(
    const url::Origin& origin,
    const IPAddress& received_ip_address,
    const std::string& value) {
  VLOG(1) << "Received NEL policy for " << origin;
  Header header;
  header.origin = origin;
  header.received_ip_address = received_ip_address;
  header.value = value;
  headers_.push_back(header);
}

void TestNetworkErrorLoggingService::OnRequest(RequestDetails details) {
  errors_.push_back(std::move(details));
}

void TestNetworkErrorLoggingService::RemoveBrowsingData(
    const base::RepeatingCallback<bool(const GURL&)>& origin_filter) {}

void TestNetworkErrorLoggingService::RemoveAllBrowsingData() {}

bool TestNetworkErrorLoggingService::Header::MatchesAddressList(
    const AddressList& address_list) const {
  return std::any_of(address_list.begin(), address_list.end(),
                     [this](const IPEndPoint& endpoint) {
                       return endpoint.address() == received_ip_address;
                     });
}

}  // namespace net
