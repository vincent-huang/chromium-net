// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_QUIC_CORE_QPACK_QPACK_PROGRESSIVE_DECODER_H_
#define NET_THIRD_PARTY_QUIC_CORE_QPACK_QPACK_PROGRESSIVE_DECODER_H_

#include <cstdint>
#include <memory>

#include "net/third_party/quic/core/qpack/qpack_decoder_stream_sender.h"
#include "net/third_party/quic/core/qpack/qpack_encoder_stream_receiver.h"
#include "net/third_party/quic/core/qpack/qpack_instruction_decoder.h"
#include "net/third_party/quic/core/quic_types.h"
#include "net/third_party/quic/platform/api/quic_export.h"
#include "net/third_party/quic/platform/api/quic_string.h"
#include "net/third_party/quic/platform/api/quic_string_piece.h"

namespace quic {

class QpackHeaderTable;

// Class to decode a single header block.
class QUIC_EXPORT_PRIVATE QpackProgressiveDecoder
    : public QpackInstructionDecoder::Delegate {
 public:
  // Interface for receiving decoded header block from the decoder.
  class QUIC_EXPORT_PRIVATE HeadersHandlerInterface {
   public:
    virtual ~HeadersHandlerInterface() {}

    // Called when a new header name-value pair is decoded.  Multiple values for
    // a given name will be emitted as multiple calls to OnHeader.
    virtual void OnHeaderDecoded(QuicStringPiece name,
                                 QuicStringPiece value) = 0;

    // Called when the header block is completely decoded.
    // Indicates the total number of bytes in this block.
    // The decoder will not access the handler after this call.
    // Note that this method might not be called synchronously when the header
    // block is received on the wire, in case decoding is blocked on receiving
    // entries on the encoder stream.  TODO(bnc): Implement blocked decoding.
    virtual void OnDecodingCompleted() = 0;

    // Called when a decoding error has occurred.  No other methods will be
    // called afterwards.
    virtual void OnDecodingErrorDetected(QuicStringPiece error_message) = 0;
  };

  QpackProgressiveDecoder() = delete;
  QpackProgressiveDecoder(QuicStreamId stream_id,
                          QpackHeaderTable* header_table,
                          QpackDecoderStreamSender* decoder_stream_sender,
                          HeadersHandlerInterface* handler);
  QpackProgressiveDecoder(const QpackProgressiveDecoder&) = delete;
  QpackProgressiveDecoder& operator=(const QpackProgressiveDecoder&) = delete;
  ~QpackProgressiveDecoder() override = default;

  // Calculate actual Largest Reference from largest reference value sent on
  // wire, MaxEntries, and total number of dynamic table insertions according to
  // https://quicwg.org/base-drafts/draft-ietf-quic-qpack.html#largest-reference
  // Returns true on success, false on invalid input or overflow/underflow.
  static bool DecodeLargestReference(uint64_t wire_largest_reference,
                                     uint64_t max_entries,
                                     uint64_t total_number_of_inserts,
                                     uint64_t* largest_reference);

  // Provide a data fragment to decode.
  void Decode(QuicStringPiece data);

  // Signal that the entire header block has been received and passed in
  // through Decode().  No methods must be called afterwards.
  void EndHeaderBlock();

  // QpackInstructionDecoder::Delegate implementation.
  bool OnInstructionDecoded(const QpackInstruction* instruction) override;
  void OnError(QuicStringPiece error_message) override;

 private:
  bool DoIndexedHeaderFieldInstruction();
  bool DoIndexedHeaderFieldPostBaseInstruction();
  bool DoLiteralHeaderFieldNameReferenceInstruction();
  bool DoLiteralHeaderFieldPostBaseInstruction();
  bool DoLiteralHeaderFieldInstruction();
  bool DoPrefixInstruction();

  // Calculates Base Index from |largest_reference_|, which must be set before
  // calling this method, and sign bit and Delta Base Index in the Header Data
  // Prefix, which are passed in as arguments.  Returns true on success, false
  // on failure due to overflow/underflow.
  bool DeltaBaseIndexToBaseIndex(bool sign,
                                 uint64_t delta_base_index,
                                 uint64_t* base_index);

  // The request stream can use relative index (but different from the kind of
  // relative index used on the encoder stream), and post-base index.
  // These methods convert relative index and post-base index to absolute index
  // (one based).  They return true on success, or false if conversion fails due
  // to overflow/underflow.
  bool RequestStreamRelativeIndexToAbsoluteIndex(
      uint64_t relative_index,
      uint64_t* absolute_index) const;
  bool PostBaseIndexToAbsoluteIndex(uint64_t post_base_index,
                                    uint64_t* absolute_index) const;

  const QuicStreamId stream_id_;

  // |prefix_decoder_| only decodes a handful of bytes then it can be
  // destroyed to conserve memory.  |instruction_decoder_|, on the other hand,
  // is used until the entire header block is decoded.
  std::unique_ptr<QpackInstructionDecoder> prefix_decoder_;
  QpackInstructionDecoder instruction_decoder_;

  const QpackHeaderTable* const header_table_;
  QpackDecoderStreamSender* const decoder_stream_sender_;
  HeadersHandlerInterface* const handler_;

  // Largest Reference and Base Index are parsed from the Header Data Prefix.
  // They are both absolute indices, that is, one based.
  uint64_t largest_reference_;
  uint64_t base_index_;

  // Keep track of largest reference seen in this header block.
  // After decoding is completed, this can be compared to |largest_reference_|.
  uint64_t largest_reference_seen_;

  // False until prefix is fully read and decoded.
  bool prefix_decoded_;

  // True until EndHeaderBlock() is called.
  bool decoding_;

  // True if a decoding error has been detected.
  bool error_detected_;
};

}  // namespace quic

#endif  // NET_THIRD_PARTY_QUIC_CORE_QPACK_QPACK_PROGRESSIVE_DECODER_H_
