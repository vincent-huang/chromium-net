// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_HTTP_HTTP_NEGOTIATE_AUTH_SYSTEM_H_
#define NET_HTTP_HTTP_NEGOTIATE_AUTH_SYSTEM_H_

#include "net/base/completion_once_callback.h"
#include "net/base/net_export.h"
#include "net/http/http_auth.h"

namespace net {

class AuthCredentials;
class HttpAuthChallengeTokenizer;

class NET_EXPORT_PRIVATE HttpNegotiateAuthSystem {
 public:
  virtual ~HttpNegotiateAuthSystem() = default;

  virtual bool Init() = 0;

  // True if authentication needs the identity of the user from Chrome.
  virtual bool NeedsIdentity() const = 0;

  // True authentication can use explicit credentials included in the URL.
  virtual bool AllowsExplicitCredentials() const = 0;

  // Parse a received Negotiate challenge.
  virtual HttpAuth::AuthorizationResult ParseChallenge(
      HttpAuthChallengeTokenizer* tok) = 0;

  // Generates an authentication token.
  //
  // The return value is an error code. The authentication token will be
  // returned in |*auth_token|. If the result code is not |OK|, the value of
  // |*auth_token| is unspecified.
  //
  // If the operation cannot be completed synchronously, |ERR_IO_PENDING| will
  // be returned and the real result code will be passed to the completion
  // callback.  Otherwise the result code is returned immediately from this
  // call.
  //
  // If the AndroidAuthNegotiate object is deleted before completion then the
  // callback will not be called.
  //
  // If no immediate result is returned then |auth_token| must remain valid
  // until the callback has been called.
  //
  // |spn| is the Service Principal Name of the server that the token is
  // being generated for.
  //
  // If this is the first round of a multiple round scheme, credentials are
  // obtained using |*credentials|. If |credentials| is NULL, the default
  // credentials are used instead.
  virtual int GenerateAuthToken(const AuthCredentials* credentials,
                                const std::string& spn,
                                const std::string& channel_bindings,
                                std::string* auth_token,
                                CompletionOnceCallback callback) = 0;

  // Delegation is allowed on the Kerberos ticket. This allows certain servers
  // to act as the user, such as an IIS server retrieving data from a
  // Kerberized MSSQL server.
  virtual void Delegate() = 0;
};

}  // namespace net

#endif  // NET_HTTP_HTTP_NEGOTIATE_AUTH_SYSTEM_H_
