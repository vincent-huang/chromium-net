// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/filter/gzip_source_stream.h"

#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <memory>

#include "base/memory/ref_counted.h"
#include "net/base/io_buffer.h"
#include "net/base/test_completion_callback.h"
#include "net/filter/fuzzed_source_stream.h"

// Fuzzer for GzipSourceStream.
//
// |data| is used to create a FuzzedSourceStream.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Bound the input to 256 KiB. If the input is divided into one-byte chunks,
  // the fuzzer may time out. See https://crbug.com/1014767.
  size = std::min(size, size_t(256 * 1024));

  net::TestCompletionCallback callback;
  FuzzedDataProvider data_provider(data, size);
  auto fuzzed_source_stream =
      std::make_unique<net::FuzzedSourceStream>(&data_provider);

  // Gzip has a maximum compression ratio of 1032x. While, strictly speaking,
  // linear, this means the fuzzer will often get stuck. Bound the output. See
  // https://crbug.com/921075.
  size_t max_output = 512 * 1024;

  const net::SourceStream::SourceType kGzipTypes[] = {
      net::SourceStream::TYPE_GZIP, net::SourceStream::TYPE_DEFLATE};
  net::SourceStream::SourceType type =
      data_provider.PickValueInArray(kGzipTypes);
  std::unique_ptr<net::GzipSourceStream> gzip_stream =
      net::GzipSourceStream::Create(std::move(fuzzed_source_stream), type);
  size_t bytes_read = 0;
  while (true) {
    scoped_refptr<net::IOBufferWithSize> io_buffer =
        base::MakeRefCounted<net::IOBufferWithSize>(64);
    int result = gzip_stream->Read(io_buffer.get(), io_buffer->size(),
                                   callback.callback());
    // Releasing the pointer to IOBuffer immediately is more likely to lead to a
    // use-after-free.
    io_buffer = nullptr;
    result = callback.GetResult(result);
    if (result <= 0)
      break;
    bytes_read += static_cast<size_t>(result);
    if (bytes_read >= max_output)
      break;
  }

  return 0;
}
