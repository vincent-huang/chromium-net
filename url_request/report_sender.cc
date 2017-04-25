// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/url_request/report_sender.h"

#include <utility>

#include "base/memory/ptr_util.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/load_flags.h"
#include "net/base/request_priority.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_status.h"

namespace {
const void* const kUserDataKey = &kUserDataKey;

class CallbackInfo : public base::SupportsUserData::Data {
 public:
  CallbackInfo(const net::ReportSender::SuccessCallback& success_callback,
               const net::ReportSender::ErrorCallback& error_callback)
      : success_callback_(success_callback), error_callback_(error_callback) {}

  ~CallbackInfo() override {}

  const net::ReportSender::SuccessCallback& success_callback() const {
    return success_callback_;
  }
  const net::ReportSender::ErrorCallback& error_callback() const {
    return error_callback_;
  }

 private:
  net::ReportSender::SuccessCallback success_callback_;
  net::ReportSender::ErrorCallback error_callback_;
};
}  // namespace

namespace net {

ReportSender::ReportSender(URLRequestContext* request_context,
                           CookiesPreference cookies_preference)
    : request_context_(request_context),
      cookies_preference_(cookies_preference) {}

ReportSender::~ReportSender() {
}

void ReportSender::Send(const GURL& report_uri,
                        base::StringPiece content_type,
                        base::StringPiece report,
                        const SuccessCallback& success_callback,
                        const ErrorCallback& error_callback) {
  DCHECK(!content_type.empty());
  std::unique_ptr<URLRequest> url_request =
      request_context_->CreateRequest(report_uri, DEFAULT_PRIORITY, this);
  url_request->SetUserData(
      &kUserDataKey,
      base::MakeUnique<CallbackInfo>(success_callback, error_callback));

  int load_flags =
      LOAD_BYPASS_CACHE | LOAD_DISABLE_CACHE | LOAD_DO_NOT_SEND_AUTH_DATA;
  if (cookies_preference_ != SEND_COOKIES) {
    load_flags |= LOAD_DO_NOT_SEND_COOKIES | LOAD_DO_NOT_SAVE_COOKIES;
  }
  url_request->SetLoadFlags(load_flags);

  HttpRequestHeaders extra_headers;
  extra_headers.SetHeader(HttpRequestHeaders::kContentType, content_type);
  url_request->SetExtraRequestHeaders(extra_headers);

  url_request->set_method("POST");

  std::vector<char> report_data(report.begin(), report.end());
  std::unique_ptr<UploadElementReader> reader(
      new UploadOwnedBytesElementReader(&report_data));
  url_request->set_upload(
      ElementsUploadDataStream::CreateWithReader(std::move(reader), 0));

  URLRequest* raw_url_request = url_request.get();
  inflight_requests_[raw_url_request] = std::move(url_request);
  raw_url_request->Start();
}

void ReportSender::OnResponseStarted(URLRequest* request, int net_error) {
  DCHECK_NE(ERR_IO_PENDING, net_error);

  CallbackInfo* callback_info =
      static_cast<CallbackInfo*>(request->GetUserData(&kUserDataKey));
  DCHECK(callback_info);
  if (net_error != OK) {
    DVLOG(1) << "Failed to send report for " << request->url().host();
    if (!callback_info->error_callback().is_null())
      callback_info->error_callback().Run(request->url(), net_error);
  } else if (!callback_info->success_callback().is_null()) {
    callback_info->success_callback().Run();
  }

  CHECK_GT(inflight_requests_.erase(request), 0u);
}

void ReportSender::OnReadCompleted(URLRequest* request, int bytes_read) {
  NOTREACHED();
}

}  // namespace net
