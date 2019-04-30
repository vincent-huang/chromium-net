// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cookies/cookie_monster_netlog_params.h"

#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_store.h"

namespace net {

base::Value NetLogCookieMonsterConstructorCallback(
    bool persistent_store,
    NetLogCaptureMode /* capture_mode */) {
  base::DictionaryValue dict;
  dict.SetKey("persistent_store", base::Value(persistent_store));
  return std::move(dict);
}

base::Value NetLogCookieMonsterCookieAdded(const CanonicalCookie* cookie,
                                           bool sync_requested,
                                           NetLogCaptureMode capture_mode) {
  if (!capture_mode.include_cookies_and_credentials())
    return base::Value();

  base::DictionaryValue dict;
  dict.SetKey("name", base::Value(cookie->Name()));
  dict.SetKey("value", base::Value(cookie->Value()));
  dict.SetKey("domain", base::Value(cookie->Domain()));
  dict.SetKey("path", base::Value(cookie->Path()));
  dict.SetKey("httponly", base::Value(cookie->IsHttpOnly()));
  dict.SetKey("secure", base::Value(cookie->IsSecure()));
  dict.SetKey("priority",
              base::Value(CookiePriorityToString(cookie->Priority())));
  dict.SetKey("same_site",
              base::Value(CookieSameSiteToString(cookie->SameSite())));
  dict.SetKey("is_persistent", base::Value(cookie->IsPersistent()));
  dict.SetKey("sync_requested", base::Value(sync_requested));
  return std::move(dict);
}

base::Value NetLogCookieMonsterCookieDeleted(const CanonicalCookie* cookie,
                                             CookieChangeCause cause,
                                             bool sync_requested,
                                             NetLogCaptureMode capture_mode) {
  if (!capture_mode.include_cookies_and_credentials())
    return base::Value();

  base::DictionaryValue dict;
  dict.SetKey("name", base::Value(cookie->Name()));
  dict.SetKey("value", base::Value(cookie->Value()));
  dict.SetKey("domain", base::Value(cookie->Domain()));
  dict.SetKey("path", base::Value(cookie->Path()));
  dict.SetKey("is_persistent", base::Value(cookie->IsPersistent()));
  dict.SetKey("deletion_cause", base::Value(CookieChangeCauseToString(cause)));
  dict.SetKey("sync_requested", base::Value(sync_requested));
  return std::move(dict);
}

base::Value NetLogCookieMonsterCookieRejectedSecure(
    const CanonicalCookie* old_cookie,
    const CanonicalCookie* new_cookie,
    NetLogCaptureMode capture_mode) {
  if (!capture_mode.include_cookies_and_credentials())
    return base::Value();
  base::DictionaryValue dict;
  dict.SetKey("name", base::Value(old_cookie->Name()));
  dict.SetKey("domain", base::Value(old_cookie->Domain()));
  dict.SetKey("oldpath", base::Value(old_cookie->Path()));
  dict.SetKey("newpath", base::Value(new_cookie->Path()));
  dict.SetKey("oldvalue", base::Value(old_cookie->Value()));
  dict.SetKey("newvalue", base::Value(new_cookie->Value()));
  return std::move(dict);
}

base::Value NetLogCookieMonsterCookieRejectedHttponly(
    const CanonicalCookie* old_cookie,
    const CanonicalCookie* new_cookie,
    NetLogCaptureMode capture_mode) {
  if (!capture_mode.include_cookies_and_credentials())
    return base::Value();
  base::DictionaryValue dict;
  dict.SetKey("name", base::Value(old_cookie->Name()));
  dict.SetKey("domain", base::Value(old_cookie->Domain()));
  dict.SetKey("path", base::Value(old_cookie->Path()));
  dict.SetKey("oldvalue", base::Value(old_cookie->Value()));
  dict.SetKey("newvalue", base::Value(new_cookie->Value()));
  return std::move(dict);
}

base::Value NetLogCookieMonsterCookiePreservedSkippedSecure(
    const CanonicalCookie* skipped_secure,
    const CanonicalCookie* preserved,
    const CanonicalCookie* new_cookie,
    NetLogCaptureMode capture_mode) {
  if (!capture_mode.include_cookies_and_credentials())
    return base::Value();
  base::DictionaryValue dict;
  dict.SetKey("name", base::Value(preserved->Name()));
  dict.SetKey("domain", base::Value(preserved->Domain()));
  dict.SetKey("path", base::Value(preserved->Path()));
  dict.SetKey("securecookiedomain", base::Value(skipped_secure->Domain()));
  dict.SetKey("securecookiepath", base::Value(skipped_secure->Path()));
  dict.SetKey("preservedvalue", base::Value(preserved->Value()));
  dict.SetKey("discardedvalue", base::Value(new_cookie->Value()));
  return std::move(dict);
}

}  // namespace net
