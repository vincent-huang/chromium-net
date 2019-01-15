// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_SPDY_PLATFORM_IMPL_SPDY_UNSAFE_ARENA_IMPL_H_
#define NET_THIRD_PARTY_SPDY_PLATFORM_IMPL_SPDY_UNSAFE_ARENA_IMPL_H_

#include <memory>
#include <vector>

#include "net/third_party/spdy/platform/api/spdy_export.h"

namespace spdy {

// Allocates large blocks of memory, and doles them out in smaller chunks.
// Not thread-safe.
class SPDY_EXPORT_PRIVATE SpdyUnsafeArenaImpl {
 public:
  class Status {
   private:
    friend class SpdyUnsafeArenaImpl;
    size_t bytes_allocated_;

   public:
    Status() : bytes_allocated_(0) {}
    size_t bytes_allocated() const { return bytes_allocated_; }
  };

  // Blocks allocated by this arena will be at least |block_size| bytes.
  explicit SpdyUnsafeArenaImpl(size_t block_size);
  ~SpdyUnsafeArenaImpl();

  // Copy and assign are not allowed.
  SpdyUnsafeArenaImpl() = delete;
  SpdyUnsafeArenaImpl(const SpdyUnsafeArenaImpl&) = delete;
  SpdyUnsafeArenaImpl& operator=(const SpdyUnsafeArenaImpl&) = delete;

  // Move is allowed.
  SpdyUnsafeArenaImpl(SpdyUnsafeArenaImpl&& other);
  SpdyUnsafeArenaImpl& operator=(SpdyUnsafeArenaImpl&& other);

  char* Alloc(size_t size);
  char* Realloc(char* original, size_t oldsize, size_t newsize);
  char* Memdup(const char* data, size_t size);

  // If |data| and |size| describe the most recent allocation made from this
  // arena, the memory is reclaimed. Otherwise, this method is a no-op.
  void Free(char* data, size_t size);

  void Reset();

  Status status() const { return status_; }

 private:
  struct Block {
    std::unique_ptr<char[]> data;
    size_t size = 0;
    size_t used = 0;

    explicit Block(size_t s);
    ~Block();

    Block(Block&& other);
    Block& operator=(Block&& other);
  };

  void Reserve(size_t additional_space);
  void AllocBlock(size_t size);

  size_t block_size_;
  std::vector<Block> blocks_;
  Status status_;
};

}  // namespace spdy

#endif  // NET_THIRD_PARTY_SPDY_PLATFORM_IMPL_SPDY_UNSAFE_ARENA_IMPL_H_