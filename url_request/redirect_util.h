// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_URL_REQUEST_REDIRECT_UTIL_H_
#define NET_URL_REQUEST_REDIRECT_UTIL_H_

#include <string>

#include "base/optional.h"
#include "net/base/net_export.h"

class GURL;

namespace net {

struct RedirectInfo;
class HttpRequestHeaders;

class RedirectUtil {
 public:
  // Updates HTTP headers in |request_headers| for a redirect.
  // |removed_headers| and |modified_headers| are specified by
  // clients to add or override existing headers for the redirect.
  // |should_clear_upload| is set to true when the request body should be
  // cleared during the redirect.
  NET_EXPORT static void UpdateHttpRequest(
      const GURL& original_url,
      const std::string& original_method,
      const RedirectInfo& redirect_info,
      const base::Optional<std::vector<std::string>>& removed_headers,
      const base::Optional<net::HttpRequestHeaders>& modified_headers,
      HttpRequestHeaders* request_headers,
      bool* should_clear_upload);
};

}  // namespace net

#endif  // NET_URL_REQUEST_REDIRECT_UTIL_H_
