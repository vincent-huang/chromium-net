// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_HTTP_HTTP_UTIL_H_
#define NET_HTTP_HTTP_UTIL_H_

#include <stddef.h>

#include <string>
#include <vector>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_tokenizer.h"
#include "base/time/time.h"
#include "net/base/net_export.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_version.h"
#include "url/gurl.h"

// This is a macro to support extending this string literal at compile time.
// Please excuse me polluting your global namespace!
#define HTTP_LWS " \t"

namespace net {

class NET_EXPORT HttpUtil {
 public:
  // Returns the absolute URL, to be used for the http request. This url is
  // made up of the protocol, host, [port], path, [query]. Everything else
  // is stripped (username, password, reference).
  static std::string SpecForRequest(const GURL& url);

  // Parses the value of a Content-Type header.  The resulting mime_type and
  // charset values are normalized to lowercase.  The mime_type and charset
  // output values are only modified if the content_type_str contains a mime
  // type and charset value, respectively.  The boundary output value is
  // optional and will be assigned the (quoted) value of the boundary
  // paramter, if any.
  static void ParseContentType(const std::string& content_type_str,
                               std::string* mime_type,
                               std::string* charset,
                               bool* had_charset,
                               std::string* boundary);

  // Scans the headers and look for the first "Range" header in |headers|,
  // if "Range" exists and the first one of it is well formatted then returns
  // true, |ranges| will contain a list of valid ranges. If return
  // value is false then values in |ranges| should not be used. The format of
  // "Range" header is defined in RFC 7233 Section 2.1.
  // https://tools.ietf.org/html/rfc7233#section-2.1
  static bool ParseRanges(const std::string& headers,
                          std::vector<HttpByteRange>* ranges);

  // Same thing as ParseRanges except the Range header is known and its value
  // is directly passed in, rather than requiring searching through a string.
  static bool ParseRangeHeader(const std::string& range_specifier,
                               std::vector<HttpByteRange>* ranges);

  // Parses a Retry-After header that is either an absolute date/time or a
  // number of seconds in the future. Interprets absolute times as relative to
  // |now|. If |retry_after_string| is successfully parsed and indicates a time
  // that is not in the past, fills in |*retry_after| and returns true;
  // otherwise, returns false.
  static bool ParseRetryAfterHeader(const std::string& retry_after_string,
                                    base::Time now,
                                    base::TimeDelta* retry_after);

  // Scans the '\r\n'-delimited headers for the given header name.  Returns
  // true if a match is found.  Input is assumed to be well-formed.
  // TODO(darin): kill this
  static bool HasHeader(const std::string& headers, const char* name);

  // Returns true if it is safe to allow users and scripts to specify the header
  // named |name|.
  static bool IsSafeHeader(const std::string& name);

  // Returns true if |name| is a valid HTTP header name.
  static bool IsValidHeaderName(const base::StringPiece& name);

  // Returns false if |value| contains NUL or CRLF. This method does not perform
  // a fully RFC-2616-compliant header value validation.
  static bool IsValidHeaderValue(const base::StringPiece& value);

  // Strips all header lines from |headers| whose name matches
  // |headers_to_remove|. |headers_to_remove| is a list of null-terminated
  // lower-case header names, with array length |headers_to_remove_len|.
  // Returns the stripped header lines list, separated by "\r\n".
  static std::string StripHeaders(const std::string& headers,
                                  const char* const headers_to_remove[],
                                  size_t headers_to_remove_len);

  // Multiple occurances of some headers cannot be coalesced into a comma-
  // separated list since their values are (or contain) unquoted HTTP-date
  // values, which may contain a comma (see RFC 2616 section 3.3.1).
  static bool IsNonCoalescingHeader(std::string::const_iterator name_begin,
                                    std::string::const_iterator name_end);
  static bool IsNonCoalescingHeader(const std::string& name) {
    return IsNonCoalescingHeader(name.begin(), name.end());
  }

  // Return true if the character is HTTP "linear white space" (SP | HT).
  // This definition corresponds with the HTTP_LWS macro, and does not match
  // newlines.
  static bool IsLWS(char c);

  // Trim HTTP_LWS chars from the beginning and end of the string.
  static void TrimLWS(std::string::const_iterator* begin,
                      std::string::const_iterator* end);
  static base::StringPiece TrimLWS(const base::StringPiece& string);

  // Whether the character is the start of a quotation mark.
  static bool IsQuote(char c);

  // Whether the string is a valid |token| as defined in RFC 2616 Sec 2.2, or
  // RFC 7230 Sec 3.2.6 (both definitions are equivalent).
  static bool IsToken(const base::StringPiece& str);

  // Whether the string is a valid |parmname| as defined in RFC 5987 Sec 3.2.1.
  static bool IsParmName(std::string::const_iterator begin,
                         std::string::const_iterator end);
  static bool IsParmName(const std::string& str) {
    return IsParmName(str.begin(), str.end());
  }

  // RFC 2616 Sec 2.2:
  // quoted-string = ( <"> *(qdtext | quoted-pair ) <"> )
  // Unquote() strips the surrounding quotemarks off a string, and unescapes
  // any quoted-pair to obtain the value contained by the quoted-string.
  // If the input is not quoted, then it works like the identity function.
  static std::string Unquote(std::string::const_iterator begin,
                             std::string::const_iterator end);

  // Same as above.
  static std::string Unquote(const std::string& str);

  // Similar to Unquote(), but additionally validates that the string being
  // unescaped actually is a valid quoted string. Returns false for an empty
  // string, a string without quotes, a string with mismatched quotes, and
  // a string with unescaped embeded quotes.
  // In accordance with RFC 2616 this method only allows double quotes to
  // enclose the string.
  static bool StrictUnquote(std::string::const_iterator begin,
                            std::string::const_iterator end,
                            std::string* out) WARN_UNUSED_RESULT;

  // Same as above.
  static bool StrictUnquote(const std::string& str,
                            std::string* out) WARN_UNUSED_RESULT;

  // The reverse of Unquote() -- escapes and surrounds with "
  static std::string Quote(const std::string& str);

  // Returns the start of the status line, or -1 if no status line was found.
  // This allows for 4 bytes of junk to precede the status line (which is what
  // mozilla does too).
  static int LocateStartOfStatusLine(const char* buf, int buf_len);

  // Returns index beyond the end-of-headers marker or -1 if not found.  RFC
  // 2616 defines the end-of-headers marker as a double CRLF; however, some
  // servers only send back LFs (e.g., Unix-based CGI scripts written using the
  // ASIS Apache module).  This function therefore accepts the pattern LF[CR]LF
  // as end-of-headers (just like Mozilla). The first line of |buf| is
  // considered the status line, even if empty.
  // The parameter |i| is the offset within |buf| to begin searching from.
  static int LocateEndOfHeaders(const char* buf, int buf_len, int i = 0);

  // Same as |LocateEndOfHeaders|, but does not expect a status line, so can be
  // used on multi-part responses or HTTP/1.x trailers.  As a result, if |buf|
  // starts with a single [CR]LF,  it is considered an empty header list, as
  // opposed to an empty status line above a header list.
  static int LocateEndOfAdditionalHeaders(const char* buf,
                                          int buf_len,
                                          int i = 0);

  // Assemble "raw headers" in the format required by HttpResponseHeaders.
  // This involves normalizing line terminators, converting [CR]LF to \0 and
  // handling HTTP line continuations (i.e., lines starting with LWS are
  // continuations of the previous line).  |buf_len| indicates the position of
  // the end-of-headers marker as defined by LocateEndOfHeaders.
  // If a \0 appears within the headers themselves, it will be stripped. This
  // is a workaround to avoid later code from incorrectly interpreting it as
  // a line terminator.
  //
  // TODO(eroman): we should use \n as the canonical line separator rather than
  //               \0 to avoid this problem. Unfortunately the persistence layer
  //               is already dependent on newlines being replaced by NULL so
  //               this is hard to change without breaking things.
  static std::string AssembleRawHeaders(const char* buf, int buf_len);

  // Converts assembled "raw headers" back to the HTTP response format. That is
  // convert each \0 occurence to CRLF. This is used by DevTools.
  // Since all line continuations info is already lost at this point, the result
  // consists of status line and then one line for each header.
  static std::string ConvertHeadersBackToHTTPResponse(const std::string& str);

  // Given a comma separated ordered list of language codes, return
  // the list with a qvalue appended to each language.
  // The way qvalues are assigned is rather simple. The qvalue
  // starts with 1.0 and is decremented by 0.2 for each successive entry
  // in the list until it reaches 0.2. All the entries after that are
  // assigned the same qvalue of 0.2. Also, note that the 1st language
  // will not have a qvalue added because the absence of a qvalue implicitly
  // means q=1.0.
  //
  // When making a http request, this should be used to determine what
  // to put in Accept-Language header. If a comma separated list of language
  // codes *without* qvalue is sent, web servers regard all
  // of them as having q=1.0 and pick one of them even though it may not
  // be at the beginning of the list (see http://crbug.com/5899).
  static std::string GenerateAcceptLanguageHeader(
      const std::string& raw_language_list);

  // Helper. If |*headers| already contains |header_name| do nothing,
  // otherwise add <header_name> ": " <header_value> to the end of the list.
  static void AppendHeaderIfMissing(const char* header_name,
                                    const std::string& header_value,
                                    std::string* headers);

  // Returns true if the parameters describe a response with a strong etag or
  // last-modified header.  See section 13.3.3 of RFC 2616.
  // An empty string should be passed for missing headers.
  static bool HasStrongValidators(HttpVersion version,
                                  const std::string& etag_header,
                                  const std::string& last_modified_header,
                                  const std::string& date_header);

  // Returns true if this response has any validator (either a Last-Modified or
  // an ETag) regardless of whether it is strong or weak.  See section 13.3.3 of
  // RFC 2616.
  // An empty string should be passed for missing headers.
  static bool HasValidators(HttpVersion version,
                            const std::string& etag_header,
                            const std::string& last_modified_header);

  // Gets a vector of common HTTP status codes for histograms of status
  // codes.  Currently returns everything in the range [100, 600), plus 0
  // (for invalid responses/status codes).
  static std::vector<int> GetStatusCodesForHistogram();

  // Maps an HTTP status code to one of the status codes in the vector
  // returned by GetStatusCodesForHistogram.
  static int MapStatusCodeForHistogram(int code);

  // Used to iterate over the name/value pairs of HTTP headers.  To iterate
  // over the values in a multi-value header, use ValuesIterator.
  // See AssembleRawHeaders for joining line continuations (this iterator
  // does not expect any).
  class NET_EXPORT HeadersIterator {
   public:
    HeadersIterator(std::string::const_iterator headers_begin,
                    std::string::const_iterator headers_end,
                    const std::string& line_delimiter);
    ~HeadersIterator();

    // Advances the iterator to the next header, if any.  Returns true if there
    // is a next header.  Use name* and values* methods to access the resultant
    // header name and values.
    bool GetNext();

    // Iterates through the list of headers, starting with the current position
    // and looks for the specified header.  Note that the name _must_ be
    // lower cased.
    // If the header was found, the return value will be true and the current
    // position points to the header.  If the return value is false, the
    // current position will be at the end of the headers.
    bool AdvanceTo(const char* lowercase_name);

    void Reset() {
      lines_.Reset();
    }

    std::string::const_iterator name_begin() const {
      return name_begin_;
    }
    std::string::const_iterator name_end() const {
      return name_end_;
    }
    std::string name() const {
      return std::string(name_begin_, name_end_);
    }

    std::string::const_iterator values_begin() const {
      return values_begin_;
    }
    std::string::const_iterator values_end() const {
      return values_end_;
    }
    std::string values() const {
      return std::string(values_begin_, values_end_);
    }

   private:
    base::StringTokenizer lines_;
    std::string::const_iterator name_begin_;
    std::string::const_iterator name_end_;
    std::string::const_iterator values_begin_;
    std::string::const_iterator values_end_;
  };

  // Iterates over delimited values in an HTTP header.  HTTP LWS is
  // automatically trimmed from the resulting values.
  //
  // When using this class to iterate over response header values, be aware that
  // for some headers (e.g., Last-Modified), commas are not used as delimiters.
  // This iterator should be avoided for headers like that which are considered
  // non-coalescing (see IsNonCoalescingHeader).
  //
  // This iterator is careful to skip over delimiters found inside an HTTP
  // quoted string.
  //
  class NET_EXPORT_PRIVATE ValuesIterator {
   public:
    ValuesIterator(std::string::const_iterator values_begin,
                   std::string::const_iterator values_end,
                   char delimiter);
    ValuesIterator(const ValuesIterator& other);
    ~ValuesIterator();

    // Set the characters to regard as quotes.  By default, this includes both
    // single and double quotes.
    void set_quote_chars(const char* quotes) {
      values_.set_quote_chars(quotes);
    }

    // Advances the iterator to the next value, if any.  Returns true if there
    // is a next value.  Use value* methods to access the resultant value.
    bool GetNext();

    std::string::const_iterator value_begin() const {
      return value_begin_;
    }
    std::string::const_iterator value_end() const {
      return value_end_;
    }
    std::string value() const {
      return std::string(value_begin_, value_end_);
    }

   private:
    base::StringTokenizer values_;
    std::string::const_iterator value_begin_;
    std::string::const_iterator value_end_;
  };

  // Iterates over a delimited sequence of name-value pairs in an HTTP header.
  // Each pair consists of a token (the name), an equals sign, and either a
  // token or quoted-string (the value). Arbitrary HTTP LWS is permitted outside
  // of and between names, values, and delimiters.
  //
  // String iterators returned from this class' methods may be invalidated upon
  // calls to GetNext() or after the NameValuePairsIterator is destroyed.
  class NET_EXPORT NameValuePairsIterator {
   public:
    // Whether or not values are optional. Values::NOT_REQUIRED allows
    // e.g. name1=value1;name2;name3=value3, whereas Vaues::REQUIRED
    // will treat it as a parse error because name2 does not have a
    // corresponding equals sign.
    enum class Values { NOT_REQUIRED, REQUIRED };

    // Whether or not unmatched quotes should be considered a failure. By
    // default this class is pretty lenient and does a best effort to parse
    // values with mismatched quotes. When set to STRICT_QUOTES a value with
    // mismatched or otherwise invalid quotes is considered a parse error.
    enum class Quotes { STRICT_QUOTES, NOT_STRICT };

    NameValuePairsIterator(std::string::const_iterator begin,
                           std::string::const_iterator end,
                           char delimiter,
                           Values optional_values,
                           Quotes strict_quotes);

    // Treats values as not optional by default (Values::REQUIRED) and
    // treats quotes as not strict.
    NameValuePairsIterator(std::string::const_iterator begin,
                           std::string::const_iterator end,
                           char delimiter);

    NameValuePairsIterator(const NameValuePairsIterator& other);

    ~NameValuePairsIterator();

    // Advances the iterator to the next pair, if any.  Returns true if there
    // is a next pair.  Use name* and value* methods to access the resultant
    // value.
    bool GetNext();

    // Returns false if there was a parse error.
    bool valid() const { return valid_; }

    // The name of the current name-value pair.
    std::string::const_iterator name_begin() const { return name_begin_; }
    std::string::const_iterator name_end() const { return name_end_; }
    std::string name() const { return std::string(name_begin_, name_end_); }

    // The value of the current name-value pair.
    std::string::const_iterator value_begin() const {
      return value_is_quoted_ ? unquoted_value_.begin() : value_begin_;
    }
    std::string::const_iterator value_end() const {
      return value_is_quoted_ ? unquoted_value_.end() : value_end_;
    }
    std::string value() const {
      return value_is_quoted_ ? unquoted_value_ : std::string(value_begin_,
                                                              value_end_);
    }

    bool value_is_quoted() const { return value_is_quoted_; }

    // The value before unquoting (if any).
    std::string raw_value() const { return std::string(value_begin_,
                                                       value_end_); }

   private:
    bool IsQuote(char c) const;

    HttpUtil::ValuesIterator props_;
    bool valid_;

    std::string::const_iterator name_begin_;
    std::string::const_iterator name_end_;

    std::string::const_iterator value_begin_;
    std::string::const_iterator value_end_;

    // Do not store iterators into this string. The NameValuePairsIterator
    // is copyable/assignable, and if copied the copy's iterators would point
    // into the original's unquoted_value_ member.
    std::string unquoted_value_;

    bool value_is_quoted_;

    // True if values are required for each name/value pair; false if a
    // name is permitted to appear without a corresponding value.
    bool values_optional_;

    // True if quotes values are required to be properly quoted; false if
    // mismatched quotes and other problems with quoted values should be more
    // or less gracefully treated as valid.
    bool strict_quotes_;
  };
};

}  // namespace net

#endif  // NET_HTTP_HTTP_UTIL_H_
