// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/tools/transport_security_state_generator/preloaded_state_generator.h"

#include <string>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "net/tools/huffman_trie/huffman/huffman_builder.h"
#include "net/tools/transport_security_state_generator/cert_util.h"
#include "net/tools/transport_security_state_generator/spki_hash.h"

namespace net {

namespace transport_security_state {

namespace {

static const char kNewLine[] = "\n";
static const char kIndent[] = "  ";

std::string FormatSPKIName(const std::string& name) {
  return "kSPKIHash_" + name;
}

std::string FormatAcceptedKeyName(const std::string& name) {
  return "k" + name + "AcceptableCerts";
}

std::string FormatRejectedKeyName(const std::string& name) {
  return "k" + name + "RejectedCerts";
}

std::string FormatReportURIName(const std::string& name) {
  return "k" + name + "ReportURI";
}

// Replaces the first occurrence of "[[" + name + "]]" in |*tpl| with
// |value|.
bool ReplaceTag(const std::string& name,
                const std::string& value,
                std::string* tpl) {
  std::string tag = "[[" + name + "]]";

  size_t start_pos = tpl->find(tag);
  if (start_pos == std::string::npos) {
    return false;
  }

  tpl->replace(start_pos, tag.length(), value);
  return true;
}

// Formats the bytes in |bytes| as an C++ array initializer and returns the
// resulting string.
std::string FormatVectorAsArray(const std::vector<uint8_t>& bytes) {
  std::string output = "{";
  output.append(kNewLine);
  output.append(kIndent);
  output.append(kIndent);

  size_t bytes_on_current_line = 0;

  for (size_t i = 0; i < bytes.size(); ++i) {
    base::StringAppendF(&output, "0x%02x,", bytes[i]);

    bytes_on_current_line++;
    if (bytes_on_current_line >= 12 && i + 1 < bytes.size()) {
      output.append(kNewLine);
      output.append(kIndent);
      output.append(kIndent);

      bytes_on_current_line = 0;
    } else if (i + 1 < bytes.size()) {
      output.append(" ");
    }
  }

  output.append(kNewLine);
  output.append("}");

  return output;
}

std::string WritePinsetList(const std::string& name,
                            const std::vector<std::string>& pins) {
  std::string output = "static const char* const " + name + "[] = {";
  output.append(kNewLine);

  for (const auto& pin_name : pins) {
    output.append(kIndent);
    output.append(kIndent);
    output.append(FormatSPKIName(pin_name));
    output.append(",");
    output.append(kNewLine);
  }

  output.append(kIndent);
  output.append(kIndent);
  output.append("nullptr,");
  output.append(kNewLine);
  output.append("};");

  return output;
}

huffman_trie::HuffmanRepresentationTable ApproximateHuffman(
    const TransportSecurityStateEntries& entries) {
  huffman_trie::HuffmanBuilder huffman_builder;
  for (const auto& entry : entries) {
    for (const auto& c : entry->hostname) {
      huffman_builder.RecordUsage(c);
    }

    huffman_builder.RecordUsage(TrieWriter::kTerminalValue);
    huffman_builder.RecordUsage(TrieWriter::kEndOfTableValue);
  }

  return huffman_builder.ToTable();
}

}  // namespace

PreloadedStateGenerator::PreloadedStateGenerator() = default;

PreloadedStateGenerator::~PreloadedStateGenerator() = default;

std::string PreloadedStateGenerator::Generate(
    const std::string& preload_template,
    const TransportSecurityStateEntries& entries,
    const Pinsets& pinsets) {
  std::string output = preload_template;

  ProcessSPKIHashes(pinsets, &output);

  NameIDMap expect_ct_report_uri_map;
  ProcessExpectCTURIs(entries, &expect_ct_report_uri_map, &output);

  NameIDMap expect_staple_report_uri_map;
  ProcessExpectStapleURIs(entries, &expect_staple_report_uri_map, &output);

  NameIDMap pinsets_map;
  ProcessPinsets(pinsets, &pinsets_map, &output);

  // The trie generation process is ran twice, the first time using an
  // approximate Huffman table. During this first run, the correct character
  // frequencies are collected which are then used to calculate the most space
  // efficient Huffman table for the given inputs. This table is used for the
  // second run.
  huffman_trie::HuffmanRepresentationTable table = ApproximateHuffman(entries);
  huffman_trie::HuffmanBuilder huffman_builder;
  TrieWriter writer(table, expect_ct_report_uri_map,
                    expect_staple_report_uri_map, pinsets_map,
                    &huffman_builder);
  uint32_t root_position;
  if (!writer.WriteEntries(entries, &root_position)) {
    return std::string();
  }

  huffman_trie::HuffmanRepresentationTable optimal_table =
      huffman_builder.ToTable();
  TrieWriter new_writer(optimal_table, expect_ct_report_uri_map,
                        expect_staple_report_uri_map, pinsets_map, nullptr);

  if (!new_writer.WriteEntries(entries, &root_position)) {
    return std::string();
  }

  uint32_t new_length = new_writer.position();
  std::vector<uint8_t> huffman_tree = huffman_builder.ToVector();
  new_writer.Flush();

  ReplaceTag("HUFFMAN_TREE", FormatVectorAsArray(huffman_tree), &output);
  ReplaceTag("HSTS_TRIE", FormatVectorAsArray(new_writer.bytes()), &output);

  ReplaceTag("HSTS_TRIE_BITS", base::NumberToString(new_length), &output);
  ReplaceTag("HSTS_TRIE_ROOT", base::NumberToString(root_position), &output);

  return output;
}

void PreloadedStateGenerator::ProcessSPKIHashes(const Pinsets& pinset,
                                                std::string* tpl) {
  std::string output;

  const SPKIHashMap& hashes = pinset.spki_hashes();
  for (const auto& current : hashes) {
    const std::string& name = current.first;
    const SPKIHash& hash = current.second;

    output.append("static const char " + FormatSPKIName(name) + "[] =");
    output.append(kNewLine);

    for (size_t i = 0; i < hash.size() / 16; ++i) {
      output.append(kIndent);
      output.append(kIndent);
      output.append("\"");

      for (size_t j = i * 16; j < ((i + 1) * 16); ++j) {
        base::StringAppendF(&output, "\\x%02x", hash.data()[j]);
      }

      output.append("\"");
      if (i + 1 == hash.size() / 16) {
        output.append(";");
      }
      output.append(kNewLine);
    }

    output.append(kNewLine);
  }

  base::TrimString(output, kNewLine, &output);
  ReplaceTag("SPKI_HASHES", output, tpl);
}

void PreloadedStateGenerator::ProcessExpectCTURIs(
    const TransportSecurityStateEntries& entries,
    NameIDMap* expect_ct_report_uri_map,
    std::string* tpl) {
  std::string output = "{";
  output.append(kNewLine);

  for (const auto& entry : entries) {
    const std::string& url = entry->expect_ct_report_uri;
    if (entry->expect_ct && url.size() &&
        expect_ct_report_uri_map->find(url) ==
            expect_ct_report_uri_map->cend()) {
      output.append(kIndent);
      output.append(kIndent);
      output.append("\"" + entry->expect_ct_report_uri + "\",");
      output.append(kNewLine);

      expect_ct_report_uri_map->insert(
          NameIDPair(entry->expect_ct_report_uri,
                     static_cast<uint32_t>(expect_ct_report_uri_map->size())));
    }
  }

  output.append(kIndent);
  output.append(kIndent);
  output.append("nullptr,");
  output.append(kNewLine);

  output.append("}");
  ReplaceTag("EXPECT_CT_REPORT_URIS", output, tpl);
}

void PreloadedStateGenerator::ProcessExpectStapleURIs(
    const TransportSecurityStateEntries& entries,
    NameIDMap* expect_staple_report_uri_map,
    std::string* tpl) {
  std::string output = "{";
  output.append(kNewLine);

  for (const auto& entry : entries) {
    const std::string& url = entry->expect_staple_report_uri;
    if (entry->expect_staple && url.size() &&
        expect_staple_report_uri_map->find(url) ==
            expect_staple_report_uri_map->cend()) {
      output.append(kIndent);
      output.append(kIndent);
      output.append("\"" + entry->expect_staple_report_uri + "\",");
      output.append(kNewLine);

      expect_staple_report_uri_map->insert(NameIDPair(
          entry->expect_staple_report_uri,
          static_cast<uint32_t>(expect_staple_report_uri_map->size())));
    }
  }

  output.append(kIndent);
  output.append(kIndent);
  output.append("nullptr,");
  output.append(kNewLine);

  output.append("}");
  ReplaceTag("EXPECT_STAPLE_REPORT_URIS", output, tpl);
}

void PreloadedStateGenerator::ProcessPinsets(const Pinsets& pinset,
                                             NameIDMap* pinset_map,
                                             std::string* tpl) {
  std::string certs_output;
  std::string pinsets_output = "{";
  pinsets_output.append(kNewLine);

  const PinsetMap& pinsets = pinset.pinsets();
  for (const auto& current : pinsets) {
    const std::unique_ptr<Pinset>& pinset = current.second;
    std::string uppercased_name = pinset->name();
    uppercased_name[0] = base::ToUpperASCII(uppercased_name[0]);

    const std::string& accepted_pins_names =
        FormatAcceptedKeyName(uppercased_name);
    certs_output.append(
        WritePinsetList(accepted_pins_names, pinset->static_spki_hashes()));
    certs_output.append(kNewLine);

    std::string rejected_pins_names = "kNoRejectedPublicKeys";
    if (pinset->bad_static_spki_hashes().size()) {
      rejected_pins_names = FormatRejectedKeyName(uppercased_name);
      certs_output.append(WritePinsetList(rejected_pins_names,
                                          pinset->bad_static_spki_hashes()));
      certs_output.append(kNewLine);
    }

    std::string report_uri = "kNoReportURI";
    if (pinset->report_uri().size()) {
      report_uri = FormatReportURIName(uppercased_name);
      certs_output.append("static const char " + report_uri + "[] = ");
      certs_output.append("\"");
      certs_output.append(pinset->report_uri());
      certs_output.append("\";");
      certs_output.append(kNewLine);
    }
    certs_output.append(kNewLine);

    pinsets_output.append(kIndent);
    pinsets_output.append(kIndent);
    pinsets_output.append("{" + accepted_pins_names + ", " +
                          rejected_pins_names + ", " + report_uri + "},");
    pinsets_output.append(kNewLine);

    pinset_map->insert(
        NameIDPair(pinset->name(), static_cast<uint32_t>(pinset_map->size())));
  }

  pinsets_output.append("}");

  base::TrimString(certs_output, kNewLine, &certs_output);

  ReplaceTag("ACCEPTABLE_CERTS", certs_output, tpl);
  ReplaceTag("PINSETS", pinsets_output, tpl);
}

}  // namespace transport_security_state

}  // namespace net
