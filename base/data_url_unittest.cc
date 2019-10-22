// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/data_url.h"

#include "base/memory/ref_counted.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_version.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace net {

namespace {

struct ParseTestData {
  const char* url;
  bool is_valid;
  const char* mime_type;
  const char* charset;
  const std::string data;
};

}  // namespace

TEST(DataURLTest, Parse) {
  const ParseTestData tests[] = {
      {"data:", false, "", "", ""},

      {"data:,", true, "text/plain", "US-ASCII", ""},

      {"data:;base64,", true, "text/plain", "US-ASCII", ""},

      {"data:;charset=,test", false, "", "", ""},

      {"data:TeXt/HtMl,<b>x</b>", true, "text/html", "", "<b>x</b>"},

      {"data:,foo", true, "text/plain", "US-ASCII", "foo"},

      {"data:;base64,aGVsbG8gd29ybGQ=", true, "text/plain", "US-ASCII",
       "hello world"},

      // Allow invalid mediatype for backward compatibility but set mime_type to
      // "text/plain" instead of the invalid mediatype.
      {"data:foo,boo", true, "text/plain", "US-ASCII", "boo"},

      // When accepting an invalid mediatype, override charset with "US-ASCII"
      {"data:foo;charset=UTF-8,boo", true, "text/plain", "US-ASCII", "boo"},

      // Invalid mediatype. Includes a slash but the type part is not a token.
      {"data:f(oo/bar;baz=1;charset=kk,boo", true, "text/plain", "US-ASCII",
       "boo"},

      {"data:foo/bar;baz=1;charset=kk,boo", true, "foo/bar", "kk", "boo"},

      {"data:foo/bar;charset=kk;baz=1,boo", true, "foo/bar", "kk", "boo"},

      {"data:text/html,%3Chtml%3E%3Cbody%3E%3Cb%3Ehello%20world"
       "%3C%2Fb%3E%3C%2Fbody%3E%3C%2Fhtml%3E",
       true, "text/html", "", "<html><body><b>hello world</b></body></html>"},

      {"data:text/html,<html><body><b>hello world</b></body></html>", true,
       "text/html", "", "<html><body><b>hello world</b></body></html>"},

      // the comma cannot be url-escaped!
      {"data:%2Cblah", false, "", "", ""},

      // invalid base64 content
      {"data:;base64,aGVs_-_-", false, "", "", ""},

      // Spaces should be removed from non-text data URLs (we already tested
      // spaces above).
      {"data:image/fractal,a b c d e f g", true, "image/fractal", "",
       "abcdefg"},

      // Spaces should also be removed from anything base-64 encoded
      {"data:;base64,aGVs bG8gd2  9ybGQ=", true, "text/plain", "US-ASCII",
       "hello world"},

      // Other whitespace should also be removed from anything base-64 encoded.
      {"data:;base64,aGVs bG8gd2  \n9ybGQ=", true, "text/plain", "US-ASCII",
       "hello world"},

      // In base64 encoding, escaped whitespace should be stripped.
      // (This test was taken from acid3)
      // http://b/1054495
      {"data:text/javascript;base64,%20ZD%20Qg%0D%0APS%20An%20Zm91cic%0D%0A%207"
       "%20",
       true, "text/javascript", "", "d4 = 'four';"},

      // Only unescaped whitespace should be stripped in non-base64.
      // http://b/1157796
      {"data:img/png,A  B  %20  %0A  C", true, "img/png", "", "AB \nC"},

      {"data:text/plain;charset=utf-8;base64,SGVsbMO2", true, "text/plain",
       "utf-8", "Hell\xC3\xB6"},

      // no mimetype
      {"data:;charset=utf-8;base64,SGVsbMO2", true, "text/plain", "utf-8",
       "Hell\xC3\xB6"},

      // Not sufficiently padded.
      {"data:;base64,aGVsbG8gd29ybGQ", true, "text/plain", "US-ASCII",
       "hello world"},

      // Bad encoding (truncated).
      {"data:;base64,aGVsbG8gd29yb", false, "", "", ""},

      // BiDi control characters should be unescaped and preserved as is, and
      // should not be replaced with % versions. In the below case, \xE2\x80\x8F
      // is the RTL mark and the parsed text should preserve it as is.
      {"data:text/plain;charset=utf-8,\xE2\x80\x8Ftest", true, "text/plain",
       "utf-8", "\xE2\x80\x8Ftest"},

      // Same as above but with Arabic text after RTL mark.
      {"data:text/plain;charset=utf-8,"
       "\xE2\x80\x8F\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1",
       true, "text/plain", "utf-8",
       "\xE2\x80\x8F\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1"},

      // RTL mark encoded as %E2%80%8F should be unescaped too. Note that when
      // wrapped in a GURL, this URL and the next effectively become the same as
      // the previous two URLs.
      {"data:text/plain;charset=utf-8,%E2%80%8Ftest", true, "text/plain",
       "utf-8", "\xE2\x80\x8Ftest"},

      // Same as above but with Arabic text after RTL mark.
      {"data:text/plain;charset=utf-8,"
       "%E2%80%8F\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1",
       true, "text/plain", "utf-8",
       "\xE2\x80\x8F\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1"},

      // The 'data' of a data URI does not include any ref it has.
      {"data:text/plain,this/is/a/test/%23include/#dontinclude", true,
       "text/plain", "", "this/is/a/test/#include/"},

      // More unescaping tests and tests with nulls.
      {"data:%00text/plain%41,foo", true, "%00text/plain%41", "", "foo"},
      {"data:text/plain;charset=%00US-ASCII%41,foo", true, "text/plain",
       "%00US-ASCII%41", "foo"},
      {"data:text/plain,%00_%41", true, "text/plain", "",
       std::string("\x00_A", 3)},
      {"data:text/plain;base64,AA//", true, "text/plain", "",
       std::string("\x00\x0F\xFF", 3)},
      // "%62ase64" unescapes to base64, but should not be treated as such.
      {"data:text/plain;%62ase64,AA//", true, "text/plain", "", "AA//"},
  };

  for (const auto& test : tests) {
    std::string mime_type;
    std::string charset;
    std::string data;
    bool ok = DataURL::Parse(GURL(test.url), &mime_type, &charset, &data);
    EXPECT_EQ(ok, test.is_valid);
    if (test.is_valid) {
      EXPECT_EQ(test.mime_type, mime_type);
      EXPECT_EQ(test.charset, charset);
      EXPECT_EQ(test.data, data);
    }
  }
}

TEST(DataURLTest, BuildResponseSimple) {
  std::string mime_type;
  std::string charset;
  std::string data;
  scoped_refptr<HttpResponseHeaders> headers(
      new HttpResponseHeaders(std::string()));

  ASSERT_EQ(OK, DataURL::BuildResponse(GURL("data:,Hello"), "GET", &mime_type,
                                       &charset, &data, headers.get()));

  EXPECT_EQ("text/plain", mime_type);
  EXPECT_EQ("US-ASCII", charset);
  EXPECT_EQ("Hello", data);

  const HttpVersion& version = headers->GetHttpVersion();
  EXPECT_EQ(1, version.major_value());
  EXPECT_EQ(1, version.minor_value());
  EXPECT_EQ("OK", headers->GetStatusText());
  std::string value;
  EXPECT_TRUE(headers->GetNormalizedHeader("Content-Type", &value));
  EXPECT_EQ(value, "text/plain;charset=US-ASCII");
  value.clear();
}

TEST(DataURLTest, BuildResponseHead) {
  for (const char* method : {"HEAD", "head", "hEaD"}) {
    SCOPED_TRACE(method);

    std::string mime_type;
    std::string charset;
    std::string data;
    scoped_refptr<HttpResponseHeaders> headers =
        HttpResponseHeaders::TryToCreate("");
    ASSERT_EQ(OK,
              DataURL::BuildResponse(GURL("data:,Hello"), method, &mime_type,
                                     &charset, &data, headers.get()));

    EXPECT_EQ("text/plain", mime_type);
    EXPECT_EQ("US-ASCII", charset);
    EXPECT_EQ("", data);

    HttpVersion version = headers->GetHttpVersion();
    EXPECT_EQ(1, version.major_value());
    EXPECT_EQ(1, version.minor_value());
    EXPECT_EQ("OK", headers->GetStatusText());
    std::string content_type;
    EXPECT_TRUE(headers->GetNormalizedHeader("Content-Type", &content_type));
    EXPECT_EQ(content_type, "text/plain;charset=US-ASCII");
  }
}

TEST(DataURLTest, BuildResponseInput) {
  std::string mime_type;
  std::string charset;
  std::string data;
  scoped_refptr<HttpResponseHeaders> headers(
      new HttpResponseHeaders(std::string()));

  ASSERT_EQ(ERR_INVALID_URL,
            DataURL::BuildResponse(GURL("bogus"), "GET", &mime_type, &charset,
                                   &data, headers.get()));
}

TEST(DataURLTest, BuildResponseInvalidMimeType) {
  std::string mime_type;
  std::string charset;
  std::string data;
  scoped_refptr<HttpResponseHeaders> headers(
      new HttpResponseHeaders(std::string()));

  // MIME type contains delimiters. Must be accepted but Content-Type header
  // should be generated as if the mediatype was text/plain.
  ASSERT_EQ(OK,
            DataURL::BuildResponse(GURL("data:f(o/b)r,test"), "GET", &mime_type,
                                   &charset, &data, headers.get()));

  std::string value;
  EXPECT_TRUE(headers->GetNormalizedHeader("Content-Type", &value));
  EXPECT_EQ(value, "text/plain;charset=US-ASCII");
}

TEST(DataURLTest, InvalidCharset) {
  std::string mime_type;
  std::string charset;
  std::string data;
  scoped_refptr<HttpResponseHeaders> headers(
      new HttpResponseHeaders(std::string()));

  // MIME type contains delimiters. Must be rejected.
  ASSERT_EQ(ERR_INVALID_URL, DataURL::BuildResponse(
                                 GURL("data:text/html;charset=(),test"), "GET",
                                 &mime_type, &charset, &data, headers.get()));
}

}  // namespace net
