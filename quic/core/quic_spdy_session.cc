// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/core/quic_spdy_session.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "net/quic/core/quic_flags.h"
#include "net/quic/core/quic_headers_stream.h"
#include "net/quic/platform/api/quic_bug_tracker.h"
#include "net/quic/platform/api/quic_logging.h"
#include "net/quic/platform/api/quic_str_cat.h"

using base::StringPiece;
using std::string;

namespace net {

namespace {

class HeaderTableDebugVisitor
    : public HpackHeaderTable::DebugVisitorInterface {
 public:
  HeaderTableDebugVisitor(const QuicClock* clock,
                          std::unique_ptr<QuicHpackDebugVisitor> visitor)
      : clock_(clock), headers_stream_hpack_visitor_(std::move(visitor)) {}

  int64_t OnNewEntry(const HpackEntry& entry) override {
    QUIC_DVLOG(1) << entry.GetDebugString();
    return (clock_->ApproximateNow() - QuicTime::Zero()).ToMicroseconds();
  }

  void OnUseEntry(const HpackEntry& entry) override {
    const QuicTime::Delta elapsed(
        clock_->ApproximateNow() -
        QuicTime::Delta::FromMicroseconds(entry.time_added()) -
        QuicTime::Zero());
    QUIC_DVLOG(1) << entry.GetDebugString() << " " << elapsed.ToMilliseconds()
                  << " ms";
    headers_stream_hpack_visitor_->OnUseEntry(elapsed);
  }

 private:
  const QuicClock* clock_;
  std::unique_ptr<QuicHpackDebugVisitor> headers_stream_hpack_visitor_;

  DISALLOW_COPY_AND_ASSIGN(HeaderTableDebugVisitor);
};

// When forced HOL blocking is enabled, extra bytes in the form of
// HTTP/2 DATA frame headers are inserted on the way down to the
// session layer.  |ForceAckListener| filters the |OnPacketAcked()|
// notifications generated by the session layer to not count the extra
// bytes.  Otherwise, code that is using ack listener on streams might
// consider it an error if more bytes are acked than were written to
// the stream, it is the case with some internal stats gathering code.
class ForceHolAckListener : public QuicAckListenerInterface {
 public:
  // |extra_bytes| should be initialized to the size of the HTTP/2
  // DATA frame header inserted when forced HOL blocking is enabled.
  ForceHolAckListener(
      QuicReferenceCountedPointer<QuicAckListenerInterface> stream_ack_listener,
      int extra_bytes)
      : stream_ack_listener_(std::move(stream_ack_listener)),
        extra_bytes_(extra_bytes) {
    DCHECK_GE(extra_bytes, 0);
  }

  void OnPacketAcked(int acked_bytes, QuicTime::Delta ack_delay_time) override {
    if (extra_bytes_ > 0) {
      // Don't count the added HTTP/2 DATA frame header bytes
      int delta = std::min(extra_bytes_, acked_bytes);
      extra_bytes_ -= delta;
      acked_bytes -= delta;
    }
    stream_ack_listener_->OnPacketAcked(acked_bytes, ack_delay_time);
  }

  void OnPacketRetransmitted(int retransmitted_bytes) override {
    stream_ack_listener_->OnPacketRetransmitted(retransmitted_bytes);
  }

 protected:
  ~ForceHolAckListener() override {}

 private:
  QuicReferenceCountedPointer<QuicAckListenerInterface> stream_ack_listener_;
  int extra_bytes_;

  DISALLOW_COPY_AND_ASSIGN(ForceHolAckListener);
};

}  // namespace

// A SpdyFramerVisitor that passes HEADERS frames to the QuicSpdyStream, and
// closes the connection if any unexpected frames are received.
class QuicSpdySession::SpdyFramerVisitor
    : public SpdyFramerVisitorInterface,
      public SpdyFramerDebugVisitorInterface {
 public:
  explicit SpdyFramerVisitor(QuicSpdySession* session) : session_(session) {}

  SpdyHeadersHandlerInterface* OnHeaderFrameStart(
      SpdyStreamId /* stream_id */) override {
    return &header_list_;
  }

  void OnHeaderFrameEnd(SpdyStreamId /* stream_id */,
                        bool end_headers) override {
    if (end_headers) {
      if (session_->IsConnected()) {
        session_->OnHeaderList(header_list_);
      }
      header_list_.Clear();
    }
  }

  void OnStreamFrameData(SpdyStreamId stream_id,
                         const char* data,
                         size_t len) override {
    if (session_->OnStreamFrameData(stream_id, data, len)) {
      return;
    }
    CloseConnection("SPDY DATA frame received.");
  }

  void OnStreamEnd(SpdyStreamId stream_id) override {
    // The framer invokes OnStreamEnd after processing a frame that had the fin
    // bit set.
  }

  void OnStreamPadding(SpdyStreamId stream_id, size_t len) override {
    CloseConnection("SPDY frame padding received.");
  }

  void OnError(SpdyFramer* framer) override {
    CloseConnection(QuicStrCat(
        "SPDY framing error: ",
        SpdyFramer::SpdyFramerErrorToString(framer->spdy_framer_error())));
  }

  void OnDataFrameHeader(SpdyStreamId stream_id,
                         size_t length,
                         bool fin) override {
    if (session_->OnDataFrameHeader(stream_id, length, fin)) {
      return;
    }
    CloseConnection("SPDY DATA frame received.");
  }

  void OnRstStream(SpdyStreamId stream_id,
                   SpdyRstStreamStatus status) override {
    CloseConnection("SPDY RST_STREAM frame received.");
  }

  void OnSetting(SpdySettingsIds id, uint32_t value) override {
    if (!FLAGS_quic_reloadable_flag_quic_respect_http2_settings_frame) {
      CloseConnection("SPDY SETTINGS frame received.");
      return;
    }
    switch (id) {
      case SETTINGS_HEADER_TABLE_SIZE:
        session_->UpdateHeaderEncoderTableSize(value);
        break;
      case SETTINGS_ENABLE_PUSH:
        if (FLAGS_quic_reloadable_flag_quic_enable_server_push_by_default &&
            session_->perspective() == Perspective::IS_SERVER) {
          // See rfc7540, Section 6.5.2.
          if (value > 1) {
            CloseConnection(
                QuicStrCat("Invalid value for SETTINGS_ENABLE_PUSH: ", value));
            return;
          }
          session_->UpdateEnableServerPush(value > 0);
          break;
        } else {
          CloseConnection(
              QuicStrCat("Unsupported field of HTTP/2 SETTINGS frame: ", id));
        }
        break;
      // TODO(fayang): Need to support SETTINGS_MAX_HEADER_LIST_SIZE when
      // clients are actually sending it.
      case SETTINGS_MAX_HEADER_LIST_SIZE:
        if (FLAGS_quic_reloadable_flag_quic_send_max_header_list_size) {
          break;
        }
      default:
        CloseConnection(
            QuicStrCat("Unsupported field of HTTP/2 SETTINGS frame: ", id));
    }
  }

  void OnSettingsAck() override {
    if (!FLAGS_quic_reloadable_flag_quic_respect_http2_settings_frame) {
      CloseConnection("SPDY SETTINGS frame received.");
    }
  }

  void OnSettingsEnd() override {
    if (!FLAGS_quic_reloadable_flag_quic_respect_http2_settings_frame) {
      CloseConnection("SPDY SETTINGS frame received.");
    }
  }

  void OnPing(SpdyPingId unique_id, bool is_ack) override {
    CloseConnection("SPDY PING frame received.");
  }

  void OnGoAway(SpdyStreamId last_accepted_stream_id,
                SpdyGoAwayStatus status) override {
    CloseConnection("SPDY GOAWAY frame received.");
  }

  void OnHeaders(SpdyStreamId stream_id,
                 bool has_priority,
                 int weight,
                 SpdyStreamId /*parent_stream_id*/,
                 bool /*exclusive*/,
                 bool fin,
                 bool end) override {
    if (!session_->IsConnected()) {
      return;
    }

    // TODO(mpw): avoid down-conversion and plumb SpdyStreamPrecedence through
    // QuicHeadersStream.
    SpdyPriority priority =
        has_priority ? Http2WeightToSpdy3Priority(weight) : 0;
    session_->OnHeaders(stream_id, has_priority, priority, fin);
  }

  void OnWindowUpdate(SpdyStreamId stream_id, int delta_window_size) override {
    CloseConnection("SPDY WINDOW_UPDATE frame received.");
  }

  void OnPushPromise(SpdyStreamId stream_id,
                     SpdyStreamId promised_stream_id,
                     bool end) override {
    if (!session_->supports_push_promise()) {
      CloseConnection("PUSH_PROMISE not supported.");
      return;
    }
    if (!session_->IsConnected()) {
      return;
    }
    session_->OnPushPromise(stream_id, promised_stream_id, end);
  }

  void OnContinuation(SpdyStreamId stream_id, bool end) override {}

  void OnPriority(SpdyStreamId stream_id,
                  SpdyStreamId parent_id,
                  int weight,
                  bool exclusive) override {
    CloseConnection("SPDY PRIORITY frame received.");
  }

  bool OnUnknownFrame(SpdyStreamId stream_id, uint8_t frame_type) override {
    CloseConnection("Unknown frame type received.");
    return false;
  }

  // SpdyFramerDebugVisitorInterface implementation
  void OnSendCompressedFrame(SpdyStreamId stream_id,
                             SpdyFrameType type,
                             size_t payload_len,
                             size_t frame_len) override {
    if (payload_len == 0) {
      QUIC_BUG << "Zero payload length.";
      return;
    }
    int compression_pct = 100 - (100 * frame_len) / payload_len;
    QUIC_DVLOG(1) << "Net.QuicHpackCompressionPercentage: " << compression_pct;
  }

  void OnReceiveCompressedFrame(SpdyStreamId stream_id,
                                SpdyFrameType type,
                                size_t frame_len) override {
    if (session_->IsConnected()) {
      session_->OnCompressedFrameSize(frame_len);
    }
  }

  void set_max_uncompressed_header_bytes(
      size_t set_max_uncompressed_header_bytes) {
    header_list_.set_max_uncompressed_header_bytes(
        set_max_uncompressed_header_bytes);
  }

 private:
  void CloseConnection(const string& details) {
    if (session_->IsConnected()) {
      session_->CloseConnectionWithDetails(QUIC_INVALID_HEADERS_STREAM_DATA,
                                           details);
    }
  }

 private:
  QuicSpdySession* session_;
  QuicHeaderList header_list_;

  DISALLOW_COPY_AND_ASSIGN(SpdyFramerVisitor);
};

QuicHpackDebugVisitor::QuicHpackDebugVisitor() {}

QuicHpackDebugVisitor::~QuicHpackDebugVisitor() {}

QuicSpdySession::QuicSpdySession(QuicConnection* connection,
                                 QuicSession::Visitor* visitor,
                                 const QuicConfig& config)
    : QuicSession(connection, visitor, config),
      force_hol_blocking_(false),
      server_push_enabled_(false),
      stream_id_(kInvalidStreamId),
      promised_stream_id_(kInvalidStreamId),
      fin_(false),
      frame_len_(0),
      uncompressed_frame_len_(0),
      supports_push_promise_(perspective() == Perspective::IS_CLIENT),
      cur_max_timestamp_(QuicTime::Zero()),
      prev_max_timestamp_(QuicTime::Zero()),
      spdy_framer_(SpdyFramer::ENABLE_COMPRESSION),
      spdy_framer_visitor_(new SpdyFramerVisitor(this)) {
  spdy_framer_.set_visitor(spdy_framer_visitor_.get());
  spdy_framer_.set_debug_visitor(spdy_framer_visitor_.get());
}

QuicSpdySession::~QuicSpdySession() {
  // Set the streams' session pointers in closed and dynamic stream lists
  // to null to avoid subsequent use of this session.
  for (auto& stream : *closed_streams()) {
    static_cast<QuicSpdyStream*>(stream.get())->ClearSession();
  }
  for (auto const& kv : dynamic_streams()) {
    static_cast<QuicSpdyStream*>(kv.second.get())->ClearSession();
  }
}

void QuicSpdySession::Initialize() {
  QuicSession::Initialize();

  if (perspective() == Perspective::IS_SERVER) {
    set_largest_peer_created_stream_id(kHeadersStreamId);
  } else {
    QuicStreamId headers_stream_id = GetNextOutgoingStreamId();
    DCHECK_EQ(headers_stream_id, kHeadersStreamId);
  }

  headers_stream_.reset(new QuicHeadersStream(this));
  DCHECK_EQ(kHeadersStreamId, headers_stream_->id());
  static_streams()[kHeadersStreamId] = headers_stream_.get();
}

void QuicSpdySession::OnStreamHeadersPriority(QuicStreamId stream_id,
                                              SpdyPriority priority) {
  QuicSpdyStream* stream = GetSpdyDataStream(stream_id);
  if (!stream) {
    // It's quite possible to receive headers after a stream has been reset.
    return;
  }
  stream->OnStreamHeadersPriority(priority);
}

void QuicSpdySession::OnStreamHeaderList(QuicStreamId stream_id,
                                         bool fin,
                                         size_t frame_len,
                                         const QuicHeaderList& header_list) {
  QuicSpdyStream* stream = GetSpdyDataStream(stream_id);
  if (!stream) {
    // It's quite possible to receive headers after a stream has been reset.
    return;
  }
  stream->OnStreamHeaderList(fin, frame_len, header_list);
}

size_t QuicSpdySession::ProcessHeaderData(const struct iovec& iov,
                                          QuicTime timestamp) {
  DCHECK(timestamp.IsInitialized());
  UpdateCurMaxTimeStamp(timestamp);
  return spdy_framer_.ProcessInput(static_cast<char*>(iov.iov_base),
                                   iov.iov_len);
}

size_t QuicSpdySession::WriteHeaders(
    QuicStreamId id,
    SpdyHeaderBlock headers,
    bool fin,
    SpdyPriority priority,
    QuicReferenceCountedPointer<QuicAckListenerInterface>
        ack_notifier_delegate) {
  return WriteHeadersImpl(id, std::move(headers), fin, priority,
                          std::move(ack_notifier_delegate));
}

size_t QuicSpdySession::WriteHeadersImpl(
    QuicStreamId id,
    SpdyHeaderBlock headers,
    bool fin,
    SpdyPriority priority,
    QuicReferenceCountedPointer<QuicAckListenerInterface>
        ack_notifier_delegate) {
  SpdyHeadersIR headers_frame(id, std::move(headers));
  headers_frame.set_fin(fin);
  if (perspective() == Perspective::IS_CLIENT) {
    headers_frame.set_has_priority(true);
    headers_frame.set_weight(Spdy3PriorityToHttp2Weight(priority));
  }
  SpdySerializedFrame frame(spdy_framer_.SerializeFrame(headers_frame));
  headers_stream_->WriteOrBufferData(StringPiece(frame.data(), frame.size()),
                                     false, std::move(ack_notifier_delegate));
  return frame.size();
}

size_t QuicSpdySession::WritePushPromise(QuicStreamId original_stream_id,
                                         QuicStreamId promised_stream_id,
                                         SpdyHeaderBlock headers) {
  if (perspective() == Perspective::IS_CLIENT) {
    QUIC_BUG << "Client shouldn't send PUSH_PROMISE";
    return 0;
  }

  SpdyPushPromiseIR push_promise(original_stream_id, promised_stream_id,
                                 std::move(headers));
  // PUSH_PROMISE must not be the last frame sent out, at least followed by
  // response headers.
  push_promise.set_fin(false);

  SpdySerializedFrame frame(spdy_framer_.SerializeFrame(push_promise));
  headers_stream_->WriteOrBufferData(StringPiece(frame.data(), frame.size()),
                                     false, nullptr);
  return frame.size();
}

void QuicSpdySession::WriteDataFrame(
    QuicStreamId id,
    StringPiece data,
    bool fin,
    QuicReferenceCountedPointer<QuicAckListenerInterface> ack_listener) {
  // Note that certain SpdyDataIR constructors perform a deep copy of |data|
  // which should be avoided here.
  SpdyDataIR spdy_data(id);
  spdy_data.SetDataShallow(data);
  spdy_data.set_fin(fin);
  SpdySerializedFrame frame(spdy_framer_.SerializeFrame(spdy_data));
  QuicReferenceCountedPointer<ForceHolAckListener> force_hol_ack_listener;
  if (ack_listener != nullptr) {
    force_hol_ack_listener = new ForceHolAckListener(
        std::move(ack_listener), frame.size() - data.length());
  }
  // Use buffered writes so that coherence of framing is preserved
  // between streams.
  headers_stream_->WriteOrBufferData(StringPiece(frame.data(), frame.size()),
                                     false, std::move(force_hol_ack_listener));
}

QuicConsumedData QuicSpdySession::WritevStreamData(
    QuicStreamId id,
    QuicIOVector iov,
    QuicStreamOffset offset,
    bool fin,
    QuicReferenceCountedPointer<QuicAckListenerInterface> ack_listener) {
  const size_t max_len =
      kSpdyInitialFrameSizeLimit - kDataFrameMinimumSize;

  QuicConsumedData result(0, false);
  size_t total_length = iov.total_length;

  if (total_length == 0 && fin) {
    WriteDataFrame(id, StringPiece(), true, std::move(ack_listener));
    result.fin_consumed = true;
    return result;
  }

  // Encapsulate the data into HTTP/2 DATA frames.  The outer loop
  // handles each element of the source iov, the inner loop handles
  // the possibility of fragmenting each of those into multiple DATA
  // frames, as the DATA frames have a max size of 16KB.
  for (int i = 0; i < iov.iov_count; i++) {
    size_t src_iov_offset = 0;
    const struct iovec* src_iov = &iov.iov[i];
    do {
      if (headers_stream_->queued_data_bytes() > 0) {
        // Limit the amount of buffering to the minimum needed to
        // preserve framing.
        return result;
      }
      size_t len = std::min(
          std::min(src_iov->iov_len - src_iov_offset, max_len), total_length);
      char* data = static_cast<char*>(src_iov->iov_base) + src_iov_offset;
      src_iov_offset += len;
      offset += len;
      // fin handling, only set it for the final HTTP/2 DATA frame.
      bool last_iov = i == iov.iov_count - 1;
      bool last_fragment_within_iov = src_iov_offset >= src_iov->iov_len;
      bool frame_fin = (last_iov && last_fragment_within_iov) ? fin : false;
      WriteDataFrame(id, StringPiece(data, len), frame_fin, ack_listener);
      result.bytes_consumed += len;
      if (frame_fin) {
        result.fin_consumed = true;
      }
      DCHECK_GE(total_length, len);
      total_length -= len;
      if (total_length <= 0) {
        return result;
      }
    } while (src_iov_offset < src_iov->iov_len);
  }

  return result;
}

size_t QuicSpdySession::SendMaxHeaderListSize(size_t value) {
  SpdySettingsIR settings_frame;
  settings_frame.AddSetting(SETTINGS_MAX_HEADER_LIST_SIZE, value);

  SpdySerializedFrame frame(spdy_framer_.SerializeFrame(settings_frame));
  headers_stream_->WriteOrBufferData(StringPiece(frame.data(), frame.size()),
                                     false, nullptr);
  return frame.size();
}

void QuicSpdySession::OnHeadersHeadOfLineBlocking(QuicTime::Delta delta) {
  // Implemented in Chromium for stats tracking.
}

void QuicSpdySession::RegisterStreamPriority(QuicStreamId id,
                                             SpdyPriority priority) {
  write_blocked_streams()->RegisterStream(id, priority);
}

void QuicSpdySession::UnregisterStreamPriority(QuicStreamId id) {
  write_blocked_streams()->UnregisterStream(id);
}

void QuicSpdySession::UpdateStreamPriority(QuicStreamId id,
                                           SpdyPriority new_priority) {
  write_blocked_streams()->UpdateStreamPriority(id, new_priority);
}

QuicSpdyStream* QuicSpdySession::GetSpdyDataStream(
    const QuicStreamId stream_id) {
  return static_cast<QuicSpdyStream*>(GetOrCreateDynamicStream(stream_id));
}

void QuicSpdySession::OnCryptoHandshakeEvent(CryptoHandshakeEvent event) {
  QuicSession::OnCryptoHandshakeEvent(event);
  if (FLAGS_quic_reloadable_flag_quic_send_max_header_list_size &&
      event == HANDSHAKE_CONFIRMED && config()->SupportMaxHeaderListSize()) {
    SendMaxHeaderListSize(kDefaultMaxUncompressedHeaderSize);
  }
}

void QuicSpdySession::OnPromiseHeaderList(QuicStreamId stream_id,
                                          QuicStreamId promised_stream_id,
                                          size_t frame_len,
                                          const QuicHeaderList& header_list) {
  string error = "OnPromiseHeaderList should be overriden in client code.";
  QUIC_BUG << error;
  connection()->CloseConnection(QUIC_INTERNAL_ERROR, error,
                                ConnectionCloseBehavior::SILENT_CLOSE);
}

void QuicSpdySession::OnConfigNegotiated() {
  QuicSession::OnConfigNegotiated();
  if (config()->HasClientSentConnectionOption(kDHDT, perspective())) {
    DisableHpackDynamicTable();
  }
  const QuicVersion version = connection()->version();
  if (FLAGS_quic_reloadable_flag_quic_enable_force_hol_blocking &&
      version > QUIC_VERSION_35 && config()->ForceHolBlocking(perspective())) {
    force_hol_blocking_ = true;
    // Since all streams are tunneled through the headers stream, it
    // is important that headers stream never flow control blocks.
    // Otherwise, busy-loop behaviour can ensue where data streams
    // data try repeatedly to write data not realizing that the
    // tunnel through the headers stream is blocked.
    headers_stream_->flow_controller()->UpdateReceiveWindowSize(
        kStreamReceiveWindowLimit);
    headers_stream_->flow_controller()->UpdateSendWindowOffset(
        kStreamReceiveWindowLimit);
  }

  if (version > QUIC_VERSION_34) {
    server_push_enabled_ =
        FLAGS_quic_reloadable_flag_quic_enable_server_push_by_default;
  }
}

void QuicSpdySession::OnStreamFrameData(QuicStreamId stream_id,
                                        const char* data,
                                        size_t len,
                                        bool fin) {
  QuicSpdyStream* stream = GetSpdyDataStream(stream_id);
  if (stream == nullptr) {
    return;
  }
  const QuicStreamOffset offset =
      stream->flow_controller()->highest_received_byte_offset();
  const QuicStreamFrame frame(stream_id, fin, offset, StringPiece(data, len));
  QUIC_DVLOG(1) << "De-encapsulating DATA frame for stream " << stream_id
                << " offset " << offset << " len " << len << " fin " << fin;
  OnStreamFrame(frame);
}

bool QuicSpdySession::ShouldReleaseHeadersStreamSequencerBuffer() {
  return false;
}

void QuicSpdySession::OnHeaders(SpdyStreamId stream_id,
                                bool has_priority,
                                SpdyPriority priority,
                                bool fin) {
  if (has_priority) {
    if (perspective() == Perspective::IS_CLIENT) {
      CloseConnectionWithDetails(QUIC_INVALID_HEADERS_STREAM_DATA,
                                 "Server must not send priorities.");
      return;
    }
    OnStreamHeadersPriority(stream_id, priority);
  } else {
    if (perspective() == Perspective::IS_SERVER) {
      CloseConnectionWithDetails(QUIC_INVALID_HEADERS_STREAM_DATA,
                                 "Client must send priorities.");
      return;
    }
  }
  DCHECK_EQ(kInvalidStreamId, stream_id_);
  DCHECK_EQ(kInvalidStreamId, promised_stream_id_);
  stream_id_ = stream_id;
  fin_ = fin;
}

void QuicSpdySession::OnPushPromise(SpdyStreamId stream_id,
                                    SpdyStreamId promised_stream_id,
                                    bool end) {
  DCHECK_EQ(kInvalidStreamId, stream_id_);
  DCHECK_EQ(kInvalidStreamId, promised_stream_id_);
  stream_id_ = stream_id;
  promised_stream_id_ = promised_stream_id;
}

void QuicSpdySession::OnHeaderList(const QuicHeaderList& header_list) {
  QUIC_DVLOG(1) << "Received header list for stream " << stream_id_ << ": "
                << header_list.DebugString();
  if (prev_max_timestamp_ > cur_max_timestamp_) {
    // prev_max_timestamp_ > cur_max_timestamp_ implies that
    // headers from lower numbered streams actually came off the
    // wire after headers for the current stream, hence there was
    // HOL blocking.
    QuicTime::Delta delta = prev_max_timestamp_ - cur_max_timestamp_;
    QUIC_DLOG(INFO) << "stream " << stream_id_
                    << ": Net.QuicSession.HeadersHOLBlockedTime "
                    << delta.ToMilliseconds();
    OnHeadersHeadOfLineBlocking(delta);
  }
  prev_max_timestamp_ = std::max(prev_max_timestamp_, cur_max_timestamp_);
  cur_max_timestamp_ = QuicTime::Zero();
  if (promised_stream_id_ == kInvalidStreamId) {
    OnStreamHeaderList(stream_id_, fin_, frame_len_, header_list);
  } else {
    OnPromiseHeaderList(stream_id_, promised_stream_id_, frame_len_,
                        header_list);
  }
  // Reset state for the next frame.
  promised_stream_id_ = kInvalidStreamId;
  stream_id_ = kInvalidStreamId;
  fin_ = false;
  frame_len_ = 0;
  uncompressed_frame_len_ = 0;
}

void QuicSpdySession::OnCompressedFrameSize(size_t frame_len) {
  frame_len_ += frame_len;
}

void QuicSpdySession::DisableHpackDynamicTable() {
  spdy_framer_.UpdateHeaderEncoderTableSize(0);
}

void QuicSpdySession::SetHpackEncoderDebugVisitor(
    std::unique_ptr<QuicHpackDebugVisitor> visitor) {
  spdy_framer_.SetEncoderHeaderTableDebugVisitor(
      std::unique_ptr<HeaderTableDebugVisitor>(new HeaderTableDebugVisitor(
          connection()->helper()->GetClock(), std::move(visitor))));
}

void QuicSpdySession::SetHpackDecoderDebugVisitor(
    std::unique_ptr<QuicHpackDebugVisitor> visitor) {
  spdy_framer_.SetDecoderHeaderTableDebugVisitor(
      std::unique_ptr<HeaderTableDebugVisitor>(new HeaderTableDebugVisitor(
          connection()->helper()->GetClock(), std::move(visitor))));
}

void QuicSpdySession::UpdateHeaderEncoderTableSize(uint32_t value) {
  spdy_framer_.UpdateHeaderEncoderTableSize(value);
}

void QuicSpdySession::UpdateEnableServerPush(bool value) {
  set_server_push_enabled(value);
}

bool QuicSpdySession::OnDataFrameHeader(QuicStreamId stream_id,
                                        size_t length,
                                        bool fin) {
  if (!force_hol_blocking()) {
    return false;
  }
  if (!IsConnected()) {
    return true;
  }
  QUIC_DVLOG(1) << "DATA frame header for stream " << stream_id << " length "
                << length << " fin " << fin;
  fin_ = fin;
  frame_len_ = length;
  if (fin && length == 0) {
    OnStreamFrameData(stream_id, "", 0);
  }
  return true;
}

bool QuicSpdySession::OnStreamFrameData(QuicStreamId stream_id,
                                        const char* data,
                                        size_t len) {
  if (!force_hol_blocking()) {
    return false;
  }
  if (!IsConnected()) {
    return true;
  }
  frame_len_ -= len;
  // Ignore fin_ while there is more data coming, if frame_len_ > 0.
  OnStreamFrameData(stream_id, data, len, frame_len_ > 0 ? false : fin_);
  return true;
}

void QuicSpdySession::set_max_uncompressed_header_bytes(
    size_t set_max_uncompressed_header_bytes) {
  spdy_framer_visitor_->set_max_uncompressed_header_bytes(
      set_max_uncompressed_header_bytes);
}

void QuicSpdySession::CloseConnectionWithDetails(QuicErrorCode error,
                                                 const string& details) {
  connection()->CloseConnection(
      error, details, ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
}

}  // namespace net
