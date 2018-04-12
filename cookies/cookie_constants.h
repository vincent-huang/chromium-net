// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_COOKIES_COOKIE_CONSTANTS_H_
#define NET_COOKIES_COOKIE_CONSTANTS_H_

#include <string>

#include "net/base/net_export.h"

namespace net {

enum CookiePriority {
  COOKIE_PRIORITY_LOW     = 0,
  COOKIE_PRIORITY_MEDIUM  = 1,
  COOKIE_PRIORITY_HIGH    = 2,
  COOKIE_PRIORITY_DEFAULT = COOKIE_PRIORITY_MEDIUM
};

// See https://tools.ietf.org/html/draft-ietf-httpbis-cookie-same-site-00
// and https://tools.ietf.org/html/draft-ietf-httpbis-rfc6265bis for
// information about same site cookie restrictions.
enum class CookieSameSite {
  NO_RESTRICTION = 0,
  LAX_MODE = 1,
  STRICT_MODE = 2,
  DEFAULT_MODE = NO_RESTRICTION
};

// Returns the Set-Cookie header priority token corresponding to |priority|.
//
// TODO(mkwst): Remove this once its callsites are refactored.
NET_EXPORT std::string CookiePriorityToString(CookiePriority priority);

// Converts the Set-Cookie header priority token |priority| to a CookiePriority.
// Defaults to COOKIE_PRIORITY_DEFAULT for empty or unrecognized strings.
NET_EXPORT CookiePriority StringToCookiePriority(const std::string& priority);

// Converts the Set-Cookie header SameSite token |same_site| to a
// CookieSameSite. Defaults to CookieSameSite::DEFAULT_MODE for empty or
// unrecognized strings.
NET_EXPORT CookieSameSite StringToCookieSameSite(const std::string& same_site);

}  // namespace net

#endif  // NET_COOKIES_COOKIE_CONSTANTS_H_
