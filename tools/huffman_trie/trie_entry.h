// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_TOOLS_HUFFMAN_TRIE_TRIE_ENTRY_H_
#define NET_TOOLS_HUFFMAN_TRIE_TRIE_ENTRY_H_

#include <memory>
#include <string>
#include <vector>

namespace net {

namespace huffman_trie {

class TrieBitBuffer;

class TrieEntry {
 public:
  TrieEntry();
  virtual ~TrieEntry();

  virtual std::string name() const = 0;
  virtual bool WriteEntry(huffman_trie::TrieBitBuffer* writer) const = 0;
};

// std::unique_ptr's are not covariant, so operations on TrieEntry uses a vector
// of raw pointers instead.
using TrieEntries = std::vector<TrieEntry*>;

struct ReversedEntry {
  ReversedEntry(std::vector<uint8_t> reversed_name, const TrieEntry* entry);
  ~ReversedEntry();

  std::vector<uint8_t> reversed_name;
  const TrieEntry* entry;
};

using ReversedEntries = std::vector<std::unique_ptr<ReversedEntry>>;

}  // namespace huffman_trie

}  // namespace net

#endif  // NET_TOOLS_HUFFMAN_TRIE_TRIE_ENTRY_H_
