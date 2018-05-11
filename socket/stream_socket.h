// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_STREAM_SOCKET_H_
#define NET_SOCKET_STREAM_SOCKET_H_

#include <stdint.h>

#include "base/macros.h"
#include "net/base/net_errors.h"
#include "net/base/net_export.h"
#include "net/socket/connection_attempts.h"
#include "net/socket/next_proto.h"
#include "net/socket/socket.h"
#include "net/ssl/token_binding.h"

namespace crypto {
class ECPrivateKey;
}

namespace net {

class ChannelIDService;
class IPEndPoint;
class NetLogWithSource;
class SSLCertRequestInfo;
class SSLInfo;
class SocketTag;

class NET_EXPORT StreamSocket : public Socket {
 public:
  // This is used in DumpMemoryStats() to track the estimate of memory usage of
  // a socket.
  struct NET_EXPORT_PRIVATE SocketMemoryStats {
   public:
    SocketMemoryStats();
    ~SocketMemoryStats();
    // Estimated total memory usage of this socket in bytes.
    size_t total_size;
    // Size of all buffers used by this socket in bytes.
    size_t buffer_size;
    // Number of certs used by this socket.
    size_t cert_count;
    // Total size of certs used by this socket in bytes.
    size_t cert_size;

   private:
    DISALLOW_COPY_AND_ASSIGN(SocketMemoryStats);
  };

  ~StreamSocket() override {}

  // Called to establish a connection.  Returns OK if the connection could be
  // established synchronously.  Otherwise, ERR_IO_PENDING is returned and the
  // given callback will run asynchronously when the connection is established
  // or when an error occurs.  The result is some other error code if the
  // connection could not be established.
  //
  // The socket's Read and Write methods may not be called until Connect
  // succeeds.
  //
  // It is valid to call Connect on an already connected socket, in which case
  // OK is simply returned.
  //
  // Connect may also be called again after a call to the Disconnect method.
  //
  virtual int Connect(CompletionOnceCallback callback) = 0;

  // Called to disconnect a socket.  Does nothing if the socket is already
  // disconnected.  After calling Disconnect it is possible to call Connect
  // again to establish a new connection.
  //
  // If IO (Connect, Read, or Write) is pending when the socket is
  // disconnected, the pending IO is cancelled, and the completion callback
  // will not be called.
  virtual void Disconnect() = 0;

  // Called to test if the connection is still alive.  Returns false if a
  // connection wasn't established or the connection is dead.  True is returned
  // if the connection was terminated, but there is unread data in the incoming
  // buffer.
  virtual bool IsConnected() const = 0;

  // Called to test if the connection is still alive and idle.  Returns false
  // if a connection wasn't established, the connection is dead, or there is
  // unread data in the incoming buffer.
  virtual bool IsConnectedAndIdle() const = 0;

  // Copies the peer address to |address| and returns a network error code.
  // ERR_SOCKET_NOT_CONNECTED will be returned if the socket is not connected.
  virtual int GetPeerAddress(IPEndPoint* address) const = 0;

  // Copies the local address to |address| and returns a network error code.
  // ERR_SOCKET_NOT_CONNECTED will be returned if the socket is not bound.
  virtual int GetLocalAddress(IPEndPoint* address) const = 0;

  // Gets the NetLog for this socket.
  virtual const NetLogWithSource& NetLog() const = 0;

  // Set the annotation to indicate this socket was created for speculative
  // reasons.  This call is generally forwarded to a basic TCPClientSocket*.
  //
  // These methods are deprecated and are slated for removal.
  virtual void SetSubresourceSpeculation() = 0;
  virtual void SetOmniboxSpeculation() = 0;

  // Returns true if the socket ever had any reads or writes.  StreamSockets
  // layered on top of transport sockets should return if their own Read() or
  // Write() methods had been called, not the underlying transport's.
  virtual bool WasEverUsed() const = 0;

  // TODO(jri): Clean up -- rename to a more general EnableAutoConnectOnWrite.
  // Enables use of TCP FastOpen for the underlying transport socket.
  virtual void EnableTCPFastOpenIfSupported() {}

  // Returns true if ALPN was negotiated during the connection of this socket.
  virtual bool WasAlpnNegotiated() const = 0;

  // Returns the protocol negotiated via ALPN for this socket, or
  // kProtoUnknown will be returned if ALPN is not applicable.
  virtual NextProto GetNegotiatedProtocol() const = 0;

  // Gets the SSL connection information of the socket.  Returns false if
  // SSL was not used by this socket.
  virtual bool GetSSLInfo(SSLInfo* ssl_info) = 0;

  // Gets the SSL CertificateRequest info of the socket after Connect failed
  // with ERR_SSL_CLIENT_AUTH_CERT_NEEDED.  Must not be called on a socket that
  // does not support SSL.
  virtual void GetSSLCertRequestInfo(
      SSLCertRequestInfo* cert_request_info) const;

  // Returns the ChannelIDService used by this socket, or NULL if
  // channel ids are not supported.  Must not be called on a socket that does
  // not support SSL.
  virtual ChannelIDService* GetChannelIDService() const;

  // Generates the signature used in Token Binding using key |*key| and for a
  // Token Binding of type |tb_type|, putting the signature in |*out|. Returns a
  // net error code.  Must not be called on a socket that does not support SSL.
  virtual Error GetTokenBindingSignature(crypto::ECPrivateKey* key,
                                         TokenBindingType tb_type,
                                         std::vector<uint8_t>* out);

  // This method is only for debugging https://crbug.com/548423 and will be
  // removed when that bug is closed. This returns the channel ID key that was
  // used when establishing the connection (or NULL if no channel ID was used).
  // Must not be called on a socket that does not support SSL.
  virtual crypto::ECPrivateKey* GetChannelIDKey() const;

  // Overwrites |out| with the connection attempts made in the process of
  // connecting this socket.
  virtual void GetConnectionAttempts(ConnectionAttempts* out) const = 0;

  // Clears the socket's list of connection attempts.
  virtual void ClearConnectionAttempts() = 0;

  // Adds |attempts| to the socket's list of connection attempts.
  virtual void AddConnectionAttempts(const ConnectionAttempts& attempts) = 0;

  // Returns the total number of number bytes read by the socket. This only
  // counts the payload bytes. Transport headers are not counted. Returns
  // 0 if the socket does not implement the function. The count is reset when
  // Disconnect() is called.
  virtual int64_t GetTotalReceivedBytes() const = 0;

  // Dumps memory allocation stats into |stats|. |stats| can be assumed as being
  // default initialized upon entry. Implementations should override fields in
  // |stats|. Default implementation does nothing.
  virtual void DumpMemoryStats(SocketMemoryStats* stats) const {}

  // Apply |tag| to this socket. If socket isn't yet connected, tag will be
  // applied when socket is later connected. If Connect() fails or socket
  // is closed, tag is cleared. If this socket is layered upon or wraps an
  // underlying socket, |tag| will be applied to the underlying socket in the
  // same manner as if ApplySocketTag() was called on the underlying socket.
  // The tag can be applied at any time, in other words active sockets can be
  // retagged with a different tag. Sockets wrapping multiplexed sockets
  // (e.g. sockets who proxy through a QUIC or Spdy stream) cannot be tagged as
  // the tag would inadvertently affect other streams; calling ApplySocketTag()
  // in this case will result in CHECK(false).
  virtual void ApplySocketTag(const SocketTag& tag) = 0;
};

}  // namespace net

#endif  // NET_SOCKET_STREAM_SOCKET_H_
