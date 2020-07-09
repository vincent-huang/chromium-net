// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_PRIVACY_MODE_H_
#define NET_BASE_PRIVACY_MODE_H_

namespace net {

// Privacy Mode is enabled if cookies to particular site are blocked, so
// Channel ID is disabled on that connection (https, spdy or quic).
enum PrivacyMode {
  PRIVACY_MODE_DISABLED = 0,
  PRIVACY_MODE_ENABLED = 1,

  // Due to http://crbug.com/775438, PRIVACY_MODE_ENABLED still sends client
  // certs. This mode ensures that the request is sent without client certs.
  PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS = 2,
};

}  // namespace net

#endif  // NET_BASE_PRIVACY_MODE_H_
