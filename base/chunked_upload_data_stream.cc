// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/chunked_upload_data_stream.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"

namespace net {

ChunkedUploadDataStream::ChunkedUploadDataStream(int64_t identifier)
    : UploadDataStream(true, identifier),
      read_index_(0),
      read_offset_(0),
      all_data_appended_(false),
      read_buffer_len_(0) {
}

ChunkedUploadDataStream::~ChunkedUploadDataStream() {
}

void ChunkedUploadDataStream::AppendData(
    const char* data, int data_len, bool is_done) {
  DCHECK(!all_data_appended_);
  DCHECK(data_len > 0 || is_done);
  if (data_len > 0) {
    DCHECK(data);
    upload_data_.push_back(
        make_scoped_ptr(new std::vector<char>(data, data + data_len)));
  }
  all_data_appended_ = is_done;

  if (!read_buffer_.get())
    return;

  int result = ReadChunk(read_buffer_.get(), read_buffer_len_);
  // Shouldn't get an error or ERR_IO_PENDING.
  DCHECK_GE(result, 0);
  read_buffer_ = NULL;
  read_buffer_len_ = 0;
  OnReadCompleted(result);
}

int ChunkedUploadDataStream::InitInternal() {
  // ResetInternal should already have been called.
  DCHECK(!read_buffer_.get());
  DCHECK_EQ(0u, read_index_);
  DCHECK_EQ(0u, read_offset_);
  return OK;
}

int ChunkedUploadDataStream::ReadInternal(IOBuffer* buf, int buf_len) {
  DCHECK_LT(0, buf_len);
  DCHECK(!read_buffer_.get());

  int result = ReadChunk(buf, buf_len);
  if (result == ERR_IO_PENDING) {
    read_buffer_ = buf;
    read_buffer_len_ = buf_len;
  }
  return result;
}

void ChunkedUploadDataStream::ResetInternal() {
  read_buffer_ = NULL;
  read_buffer_len_ = 0;
  read_index_ = 0;
  read_offset_ = 0;
}

int ChunkedUploadDataStream::ReadChunk(IOBuffer* buf, int buf_len) {
  // Copy as much data as possible from |upload_data_| to |buf|.
  int bytes_read = 0;
  while (read_index_ < upload_data_.size() && bytes_read < buf_len) {
    std::vector<char>* data = upload_data_[read_index_].get();
    size_t bytes_to_read =
        std::min(static_cast<size_t>(buf_len - bytes_read),
                 data->size() - read_offset_);
    memcpy(buf->data() + bytes_read,
           vector_as_array(data) + read_offset_,
           bytes_to_read);
    bytes_read += bytes_to_read;
    read_offset_ += bytes_to_read;
    if (read_offset_ == data->size()) {
      read_index_++;
      read_offset_ = 0;
    }
  }
  DCHECK_LE(bytes_read, buf_len);

  // If no data was written, and not all data has been appended, return
  // ERR_IO_PENDING. The read will be completed in the next call to AppendData.
  if (bytes_read == 0 && !all_data_appended_)
    return ERR_IO_PENDING;

  if (read_index_ == upload_data_.size() && all_data_appended_)
    SetIsFinalChunk();
  return bytes_read;
}

}  // namespace net
