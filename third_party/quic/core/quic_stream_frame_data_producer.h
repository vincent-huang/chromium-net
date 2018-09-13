// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_QUIC_CORE_QUIC_STREAM_FRAME_DATA_PRODUCER_H_
#define NET_THIRD_PARTY_QUIC_CORE_QUIC_STREAM_FRAME_DATA_PRODUCER_H_

#include "net/third_party/quic/core/quic_types.h"

namespace quic {

class QuicDataWriter;

// Pure virtual class to retrieve stream data.
class QUIC_EXPORT_PRIVATE QuicStreamFrameDataProducer {
 public:
  virtual ~QuicStreamFrameDataProducer() {}

  // Let |writer| write |data_length| data with |offset| of stream |id|. The
  // write fails when either stream is closed or corresponding data is failed to
  // be retrieved. This method allows writing a single stream frame from data
  // that spans multiple buffers.
  virtual WriteStreamDataResult WriteStreamData(QuicStreamId id,
                                                QuicStreamOffset offset,
                                                QuicByteCount data_length,
                                                QuicDataWriter* writer) = 0;
};

}  // namespace quic

#endif  // NET_THIRD_PARTY_QUIC_CORE_QUIC_STREAM_FRAME_DATA_PRODUCER_H_
