// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "base/logging.h"

#include "net/base/address_list.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/log/test_net_log.h"
#include "net/socket/fuzzed_socket.h"
#include "net/socket/socks5_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "third_party/libFuzzer/src/utils/FuzzedDataProvider.h"

// Fuzzer for Socks5ClientSocket.  Only covers the SOCKS5 greeet and
// handshake.
//
// |data| is used to create a FuzzedSocket to fuzz reads and writes, see that
// class for details.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Use a test NetLog, to exercise logging code.
  net::TestNetLog test_net_log;

  FuzzedDataProvider data_provider(data, size);

  net::TestCompletionCallback callback;
  std::unique_ptr<net::FuzzedSocket> fuzzed_socket(
      new net::FuzzedSocket(&data_provider, &test_net_log));
  CHECK_EQ(net::OK, fuzzed_socket->Connect(callback.callback()));

  net::SOCKS5ClientSocket socket(std::move(fuzzed_socket),
                                 net::HostPortPair("foo", 80),
                                 TRAFFIC_ANNOTATION_FOR_TESTS);
  int result = socket.Connect(callback.callback());
  callback.GetResult(result);
  return 0;
}
