// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/proxy/proxy_list.h"

#include "net/base/net_log.h"
#include "net/proxy/proxy_retry_info.h"
#include "net/proxy/proxy_server.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

// Test parsing from a PAC string.
TEST(ProxyListTest, SetFromPacString) {
  const struct {
    const char* pac_input;
    const char* pac_output;
  } tests[] = {
    // Valid inputs:
    {  "PROXY foopy:10",
       "PROXY foopy:10",
    },
    {  " DIRECT",  // leading space.
       "DIRECT",
    },
    {  "PROXY foopy1 ; proxy foopy2;\t DIRECT",
       "PROXY foopy1:80;PROXY foopy2:80;DIRECT",
    },
    {  "proxy foopy1 ; SOCKS foopy2",
       "PROXY foopy1:80;SOCKS foopy2:1080",
    },
    // Try putting DIRECT first.
    {  "DIRECT ; proxy foopy1 ; DIRECT ; SOCKS5 foopy2;DIRECT ",
       "DIRECT;PROXY foopy1:80;DIRECT;SOCKS5 foopy2:1080;DIRECT",
    },
    // Try putting DIRECT consecutively.
    {  "DIRECT ; proxy foopy1:80; DIRECT ; DIRECT",
       "DIRECT;PROXY foopy1:80;DIRECT;DIRECT",
    },

    // Invalid inputs (parts which aren't understood get
    // silently discarded):
    //
    // If the proxy list string parsed to empty, automatically fall-back to
    // DIRECT.
    {  "PROXY-foopy:10",
       "DIRECT",
    },
    {  "PROXY",
       "DIRECT",
    },
    {  "PROXY foopy1 ; JUNK ; JUNK ; SOCKS5 foopy2 ; ;",
       "PROXY foopy1:80;SOCKS5 foopy2:1080",
    },
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    ProxyList list;
    list.SetFromPacString(tests[i].pac_input);
    EXPECT_EQ(tests[i].pac_output, list.ToPacString());
    EXPECT_FALSE(list.IsEmpty());
  }
}

TEST(ProxyListTest, RemoveProxiesWithoutScheme) {
  const struct {
    const char* pac_input;
    int filter;
    const char* filtered_pac_output;
  } tests[] = {
    {  "PROXY foopy:10 ; SOCKS5 foopy2 ; SOCKS foopy11 ; PROXY foopy3 ; DIRECT",
       // Remove anything that isn't HTTP or DIRECT.
       ProxyServer::SCHEME_DIRECT | ProxyServer::SCHEME_HTTP,
       "PROXY foopy:10;PROXY foopy3:80;DIRECT",
    },
    {  "PROXY foopy:10 ; SOCKS5 foopy2",
       // Remove anything that isn't HTTP or SOCKS5.
       ProxyServer::SCHEME_DIRECT | ProxyServer::SCHEME_SOCKS4,
       "",
    },
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    ProxyList list;
    list.SetFromPacString(tests[i].pac_input);
    list.RemoveProxiesWithoutScheme(tests[i].filter);
    EXPECT_EQ(tests[i].filtered_pac_output, list.ToPacString());
  }
}

TEST(ProxyListTest, HasUntriedProxies) {
  // As in DeprioritizeBadProxies, we use a lengthy timeout to avoid depending
  // on the current time.
  ProxyRetryInfo proxy_retry_info;
  proxy_retry_info.bad_until =
      base::TimeTicks::Now() + base::TimeDelta::FromDays(1);

  // An empty list has nothing to try.
  {
    ProxyList list;
    ProxyRetryInfoMap proxy_retry_info;
    EXPECT_FALSE(list.HasUntriedProxies(proxy_retry_info));
  }

  // A list with one bad proxy has something to try. With two bad proxies,
  // there's nothing to try.
  {
    ProxyList list;
    list.SetFromPacString("PROXY bad1:80; PROXY bad2:80");
    ProxyRetryInfoMap retry_info_map;
    retry_info_map["bad1:80"] = proxy_retry_info;
    EXPECT_TRUE(list.HasUntriedProxies(retry_info_map));
    retry_info_map["bad2:80"] = proxy_retry_info;
    EXPECT_FALSE(list.HasUntriedProxies(retry_info_map));
  }

  // A list with one bad proxy and a DIRECT entry has something to try.
  {
    ProxyList list;
    list.SetFromPacString("PROXY bad1:80; DIRECT");
    ProxyRetryInfoMap retry_info_map;
    retry_info_map["bad1:80"] = proxy_retry_info;
    EXPECT_TRUE(list.HasUntriedProxies(retry_info_map));
  }
}

TEST(ProxyListTest, DeprioritizeBadProxies) {
  // Retry info that marks a proxy as being bad for a *very* long time (to avoid
  // the test depending on the current time.)
  ProxyRetryInfo proxy_retry_info;
  proxy_retry_info.bad_until =
      base::TimeTicks::Now() + base::TimeDelta::FromDays(1);

  // Call DeprioritizeBadProxies with an empty map -- should have no effect.
  {
    ProxyList list;
    list.SetFromPacString("PROXY foopy1:80;PROXY foopy2:80;PROXY foopy3:80");

    ProxyRetryInfoMap retry_info_map;
    list.DeprioritizeBadProxies(retry_info_map);
    EXPECT_EQ("PROXY foopy1:80;PROXY foopy2:80;PROXY foopy3:80",
              list.ToPacString());
  }

  // Call DeprioritizeBadProxies with 2 of the three proxies marked as bad.
  // These proxies should be retried last.
  {
    ProxyList list;
    list.SetFromPacString("PROXY foopy1:80;PROXY foopy2:80;PROXY foopy3:80");

    ProxyRetryInfoMap retry_info_map;
    retry_info_map["foopy1:80"] = proxy_retry_info;
    retry_info_map["foopy3:80"] = proxy_retry_info;
    retry_info_map["socks5://localhost:1080"] = proxy_retry_info;

    list.DeprioritizeBadProxies(retry_info_map);

    EXPECT_EQ("PROXY foopy2:80;PROXY foopy1:80;PROXY foopy3:80",
              list.ToPacString());
  }

  // Call DeprioritizeBadProxies where ALL of the proxies are marked as bad.
  // This should have no effect on the order.
  {
    ProxyList list;
    list.SetFromPacString("PROXY foopy1:80;PROXY foopy2:80;PROXY foopy3:80");

    ProxyRetryInfoMap retry_info_map;
    retry_info_map["foopy1:80"] = proxy_retry_info;
    retry_info_map["foopy2:80"] = proxy_retry_info;
    retry_info_map["foopy3:80"] = proxy_retry_info;

    list.DeprioritizeBadProxies(retry_info_map);

    EXPECT_EQ("PROXY foopy1:80;PROXY foopy2:80;PROXY foopy3:80",
              list.ToPacString());
  }
}

TEST(ProxyListTest, UpdateRetryInfoOnFallback) {
  ProxyRetryInfo proxy_retry_info;
  // Retrying should put the first proxy on the retry list.
  {
    ProxyList list;
    ProxyRetryInfoMap retry_info_map;
    BoundNetLog net_log;
    list.SetFromPacString("PROXY foopy1:80;PROXY foopy2:80;PROXY foopy3:80");
    list.UpdateRetryInfoOnFallback(&retry_info_map,
                                   base::TimeDelta::FromSeconds(60),
                                   ProxyServer(),
                                   net_log);
    EXPECT_TRUE(retry_info_map.end() != retry_info_map.find("foopy1:80"));
    EXPECT_TRUE(retry_info_map.end() == retry_info_map.find("foopy2:80"));
    EXPECT_TRUE(retry_info_map.end() == retry_info_map.find("foopy3:80"));
  }
  // Including another bad proxy should put both the first and the specified
  // proxy on the retry list.
  {
    ProxyList list;
    ProxyRetryInfoMap retry_info_map;
    BoundNetLog net_log;
    ProxyServer proxy_server = ProxyServer::FromURI("foopy3:80",
                                                    ProxyServer::SCHEME_HTTP);
    list.SetFromPacString("PROXY foopy1:80;PROXY foopy2:80;PROXY foopy3:80");
    list.UpdateRetryInfoOnFallback(&retry_info_map,
                                   base::TimeDelta::FromSeconds(60),
                                   proxy_server,
                                   net_log);
    EXPECT_TRUE(retry_info_map.end() != retry_info_map.find("foopy1:80"));
    EXPECT_TRUE(retry_info_map.end() == retry_info_map.find("foopy2:80"));
    EXPECT_TRUE(retry_info_map.end() != retry_info_map.find("foopy3:80"));
  }
  // If the first proxy is DIRECT, nothing is added to the retry list, even
  // if another bad proxy is specified.
  {
    ProxyList list;
    ProxyRetryInfoMap retry_info_map;
    BoundNetLog net_log;
    ProxyServer proxy_server = ProxyServer::FromURI("foopy2:80",
                                                    ProxyServer::SCHEME_HTTP);
    list.SetFromPacString("DIRECT;PROXY foopy2:80;PROXY foopy3:80");
    list.UpdateRetryInfoOnFallback(&retry_info_map,
                                   base::TimeDelta::FromSeconds(60),
                                   proxy_server,
                                   net_log);
    EXPECT_TRUE(retry_info_map.end() == retry_info_map.find("foopy2:80"));
    EXPECT_TRUE(retry_info_map.end() == retry_info_map.find("foopy3:80"));
  }
}

}  // namesapce

}  // namespace net
