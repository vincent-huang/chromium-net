// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/network_error_logging/network_error_logging_service.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/optional.h"
#include "base/rand_util.h"
#include "base/stl_util.h"
#include "base/time/clock.h"
#include "base/time/default_clock.h"
#include "base/time/time.h"
#include "base/values.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/log/net_log.h"
#include "net/reporting/reporting_service.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {

namespace {

const int kMaxJsonSize = 16 * 1024;
const int kMaxJsonDepth = 4;

const char kReportToKey[] = "report_to";
const char kMaxAgeKey[] = "max_age";
const char kIncludeSubdomainsKey[] = "include_subdomains";
const char kSuccessFractionKey[] = "success_fraction";
const char kFailureFractionKey[] = "failure_fraction";

// Returns the superdomain of a given domain, or the empty string if the given
// domain is just a single label. Note that this does not take into account
// anything like the Public Suffix List, so the superdomain may end up being a
// bare TLD.
//
// Examples:
//
// GetSuperdomain("assets.example.com") -> "example.com"
// GetSuperdomain("example.net") -> "net"
// GetSuperdomain("littlebox") -> ""
//
// TODO(juliatuttle): Deduplicate from Reporting in //net.
std::string GetSuperdomain(const std::string& domain) {
  size_t dot_pos = domain.find('.');
  if (dot_pos == std::string::npos)
    return "";

  return domain.substr(dot_pos + 1);
}

const char kApplicationPhase[] = "application";
const char kConnectionPhase[] = "connection";
const char kDnsPhase[] = "dns";

const char kDnsAddressChangedType[] = "dns.address_changed";
const char kHttpErrorType[] = "http.error";

const struct {
  Error error;
  const char* phase = nullptr;
  const char* type = nullptr;
} kErrorTypes[] = {
    {OK, kApplicationPhase, "ok"},

    // dns.unreachable?
    {ERR_NAME_NOT_RESOLVED, kDnsPhase, "dns.name_not_resolved"},
    {ERR_NAME_RESOLUTION_FAILED, kDnsPhase, "dns.failed"},
    {ERR_DNS_TIMED_OUT, kDnsPhase, "dns.timed_out"},
    {ERR_DNS_MALFORMED_RESPONSE, kDnsPhase, "dns.protocol"},
    {ERR_DNS_SERVER_FAILED, kDnsPhase, "dns.server"},

    {ERR_TIMED_OUT, kConnectionPhase, "tcp.timed_out"},
    {ERR_CONNECTION_TIMED_OUT, kConnectionPhase, "tcp.timed_out"},
    {ERR_CONNECTION_CLOSED, kConnectionPhase, "tcp.closed"},
    {ERR_CONNECTION_RESET, kConnectionPhase, "tcp.reset"},
    {ERR_CONNECTION_REFUSED, kConnectionPhase, "tcp.refused"},
    {ERR_CONNECTION_ABORTED, kConnectionPhase, "tcp.aborted"},
    {ERR_ADDRESS_INVALID, kConnectionPhase, "tcp.address_invalid"},
    {ERR_ADDRESS_UNREACHABLE, kConnectionPhase, "tcp.address_unreachable"},
    {ERR_CONNECTION_FAILED, kConnectionPhase, "tcp.failed"},

    {ERR_SSL_VERSION_OR_CIPHER_MISMATCH, kConnectionPhase,
     "tls.version_or_cipher_mismatch"},
    {ERR_BAD_SSL_CLIENT_AUTH_CERT, kConnectionPhase,
     "tls.bad_client_auth_cert"},
    {ERR_CERT_INVALID, kConnectionPhase, "tls.cert.invalid"},
    {ERR_CERT_COMMON_NAME_INVALID, kConnectionPhase, "tls.cert.name_invalid"},
    {ERR_CERT_DATE_INVALID, kConnectionPhase, "tls.cert.date_invalid"},
    {ERR_CERT_AUTHORITY_INVALID, kConnectionPhase,
     "tls.cert.authority_invalid"},
    {ERR_CERT_REVOKED, kConnectionPhase, "tls.cert.revoked"},
    {ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN, kConnectionPhase,
     "tls.cert.pinned_key_not_in_cert_chain"},
    {ERR_SSL_PROTOCOL_ERROR, kConnectionPhase, "tls.protocol.error"},
    {ERR_INSECURE_RESPONSE, kConnectionPhase, "tls.failed"},
    {ERR_SSL_UNRECOGNIZED_NAME_ALERT, kConnectionPhase,
     "tls.unrecognized_name_alert"},
    // tls.failed?

    {ERR_SPDY_PING_FAILED, kApplicationPhase, "h2.ping_failed"},
    {ERR_SPDY_PROTOCOL_ERROR, kConnectionPhase, "h2.protocol.error"},

    {ERR_QUIC_PROTOCOL_ERROR, kConnectionPhase, "h3.protocol.error"},

    // http.protocol.error?
    {ERR_TOO_MANY_REDIRECTS, kApplicationPhase, "http.response.redirect_loop"},
    {ERR_INVALID_RESPONSE, kApplicationPhase, "http.response.invalid"},
    {ERR_INVALID_HTTP_RESPONSE, kApplicationPhase, "http.response.invalid"},
    {ERR_EMPTY_RESPONSE, kApplicationPhase, "http.response.invalid.empty"},
    {ERR_CONTENT_LENGTH_MISMATCH, kApplicationPhase,
     "http.response.invalid.content_length_mismatch"},
    {ERR_INCOMPLETE_CHUNKED_ENCODING, kApplicationPhase,
     "http.response.invalid.incomplete_chunked_encoding"},
    {ERR_INVALID_CHUNKED_ENCODING, kApplicationPhase,
     "http.response.invalid.invalid_chunked_encoding"},
    {ERR_REQUEST_RANGE_NOT_SATISFIABLE, kApplicationPhase,
     "http.request.range_not_satisfiable"},
    {ERR_RESPONSE_HEADERS_TRUNCATED, kApplicationPhase,
     "http.response.headers.truncated"},
    {ERR_RESPONSE_HEADERS_MULTIPLE_CONTENT_DISPOSITION, kApplicationPhase,
     "http.response.headers.multiple_content_disposition"},
    {ERR_RESPONSE_HEADERS_MULTIPLE_CONTENT_LENGTH, kApplicationPhase,
     "http.response.headers.multiple_content_length"},
    // http.failed?

    {ERR_ABORTED, kApplicationPhase, "abandoned"},
    // unknown?

    // TODO(juliatuttle): Surely there are more errors we want here.
};

void GetPhaseAndTypeFromNetError(Error error,
                                 std::string* phase_out,
                                 std::string* type_out) {
  for (size_t i = 0; i < base::size(kErrorTypes); ++i) {
    DCHECK(kErrorTypes[i].phase != nullptr);
    DCHECK(kErrorTypes[i].type != nullptr);
    if (kErrorTypes[i].error == error) {
      *phase_out = kErrorTypes[i].phase;
      *type_out = kErrorTypes[i].type;
      return;
    }
  }
  *phase_out = IsCertificateError(error) ? kConnectionPhase : kApplicationPhase;
  *type_out = "unknown";
}

bool IsHttpError(const NetworkErrorLoggingService::RequestDetails& request) {
  return request.status_code >= 400 && request.status_code < 600;
}

void RecordHeaderOutcome(NetworkErrorLoggingService::HeaderOutcome outcome) {
  UMA_HISTOGRAM_ENUMERATION(NetworkErrorLoggingService::kHeaderOutcomeHistogram,
                            outcome,
                            NetworkErrorLoggingService::HeaderOutcome::MAX);
}

void RecordRequestOutcome(NetworkErrorLoggingService::RequestOutcome outcome) {
  UMA_HISTOGRAM_ENUMERATION(
      NetworkErrorLoggingService::kRequestOutcomeHistogram, outcome);
}

void RecordSignedExchangeRequestOutcome(
    NetworkErrorLoggingService::RequestOutcome outcome) {
  UMA_HISTOGRAM_ENUMERATION(
      NetworkErrorLoggingService::kSignedExchangeRequestOutcomeHistogram,
      outcome);
}

class NetworkErrorLoggingServiceImpl : public NetworkErrorLoggingService {
 public:
  explicit NetworkErrorLoggingServiceImpl(PersistentNELStore* store)
      : store_(store) {}

  ~NetworkErrorLoggingServiceImpl() override {
    if (store_)
      store_->Flush();
  }

  // NetworkErrorLoggingService implementation:

  void OnHeader(const url::Origin& origin,
                const IPAddress& received_ip_address,
                const std::string& value) override {
    if (shut_down_)
      return;

    // NEL is only available to secure origins, so don't permit insecure origins
    // to set policies.
    if (!origin.GetURL().SchemeIsCryptographic()) {
      RecordHeaderOutcome(HeaderOutcome::DISCARDED_INSECURE_ORIGIN);
      return;
    }

    NELPolicy policy;
    policy.origin = origin;
    policy.received_ip_address = received_ip_address;
    policy.last_used = clock_->Now();
    HeaderOutcome outcome = ParseHeader(value, clock_->Now(), &policy);
    RecordHeaderOutcome(outcome);
    if (outcome != HeaderOutcome::SET && outcome != HeaderOutcome::REMOVED)
      return;

    // If a policy for |origin| already existed, remove the old poliicy.
    auto it = policies_.find(origin);
    if (it != policies_.end())
      RemovePolicy(it);

    // A policy's |expires| field is set to a null time if the max_age was 0.
    // Having a max_age of 0 means that the policy should be removed, so return
    // here instead of continuing on to inserting the policy.
    if (policy.expires.is_null())
      return;

    DVLOG(1) << "Received NEL policy for " << origin;
    auto inserted = policies_.insert(std::make_pair(origin, policy));
    DCHECK(inserted.second);
    MaybeAddWildcardPolicy(origin, &inserted.first->second);

    // Evict policies if the policy limit is exceeded.
    if (policies_.size() > kMaxPolicies) {
      RemoveAllExpiredPolicies();
      while (policies_.size() > kMaxPolicies) {
        EvictStalestPolicy();
      }
    }
  }

  void OnRequest(RequestDetails details) override {
    if (shut_down_)
      return;

    if (!reporting_service_) {
      RecordRequestOutcome(RequestOutcome::kDiscardedNoReportingService);
      return;
    }

    // This method is only called on secure requests.
    DCHECK(details.uri.SchemeIsCryptographic());

    auto report_origin = url::Origin::Create(details.uri);
    const NELPolicy* policy = FindPolicyForOrigin(report_origin);
    if (!policy) {
      RecordRequestOutcome(RequestOutcome::kDiscardedNoOriginPolicy);
      return;
    }

    // Mark the policy used.
    policy->last_used = clock_->Now();

    Error type = details.type;
    // It is expected for Reporting uploads to terminate with ERR_ABORTED, since
    // the ReportingUploader cancels them after receiving the response code and
    // headers.
    if (details.reporting_upload_depth > 0 && type == ERR_ABORTED) {
      // TODO(juliatuttle): Modify ReportingUploader to drain successful uploads
      // instead of aborting them, so NEL can properly report on aborted
      // requests.
      type = OK;
    }

    std::string phase_string;
    std::string type_string;
    GetPhaseAndTypeFromNetError(type, &phase_string, &type_string);

    if (IsHttpError(details)) {
      phase_string = kApplicationPhase;
      type_string = kHttpErrorType;
    }

    // This check would go earlier, but the histogram bucket will be more
    // meaningful if it only includes reports that otherwise could have been
    // uploaded.
    if (details.reporting_upload_depth > kMaxNestedReportDepth) {
      RecordRequestOutcome(RequestOutcome::kDiscardedReportingUpload);
      return;
    }

    // If the server that handled the request is different than the server that
    // delivered the NEL policy (as determined by their IP address), then we
    // have to "downgrade" the NEL report, so that it only includes information
    // about DNS resolution.
    if (phase_string != kDnsPhase && details.server_ip.IsValid() &&
        details.server_ip != policy->received_ip_address) {
      phase_string = kDnsPhase;
      type_string = kDnsAddressChangedType;
      details.elapsed_time = base::TimeDelta();
      details.status_code = 0;
    }

    // include_subdomains policies are only allowed to report on DNS resolution
    // errors.
    if (phase_string != kDnsPhase &&
        IsMismatchingSubdomainReport(*policy, report_origin)) {
      RecordRequestOutcome(RequestOutcome::kDiscardedNonDNSSubdomainReport);
      return;
    }

    bool success = (type == OK) && !IsHttpError(details);
    const base::Optional<double> sampling_fraction =
        SampleAndReturnFraction(*policy, success);
    if (!sampling_fraction.has_value()) {
      RecordRequestOutcome(success
                               ? RequestOutcome::kDiscardedUnsampledSuccess
                               : RequestOutcome::kDiscardedUnsampledFailure);
      return;
    }

    DVLOG(1) << "Created NEL report (" << type_string
             << ", status=" << details.status_code
             << ", depth=" << details.reporting_upload_depth << ") for "
             << details.uri;
    reporting_service_->QueueReport(
        details.uri, details.user_agent, policy->report_to, kReportType,
        CreateReportBody(phase_string, type_string, sampling_fraction.value(),
                         details),
        details.reporting_upload_depth);
    RecordRequestOutcome(RequestOutcome::kQueued);
  }

  void QueueSignedExchangeReport(
      const SignedExchangeReportDetails& details) override {
    if (shut_down_)
      return;

    if (!reporting_service_) {
      RecordSignedExchangeRequestOutcome(
          RequestOutcome::kDiscardedNoReportingService);
      return;
    }
    if (!details.outer_url.SchemeIsCryptographic()) {
      RecordSignedExchangeRequestOutcome(
          RequestOutcome::kDiscardedInsecureOrigin);
      return;
    }
    const auto report_origin = url::Origin::Create(details.outer_url);
    const NELPolicy* policy = FindPolicyForOrigin(report_origin);
    if (!policy) {
      RecordSignedExchangeRequestOutcome(
          RequestOutcome::kDiscardedNoOriginPolicy);
      return;
    }

    // Mark the policy used.
    policy->last_used = clock_->Now();

    if (IsMismatchingSubdomainReport(*policy, report_origin)) {
      RecordSignedExchangeRequestOutcome(
          RequestOutcome::kDiscardedNonDNSSubdomainReport);
      return;
    }
    // Don't send the report when the IP addresses of the server and the policy
    // don’t match. This case is coverd by OnRequest() while processing the HTTP
    // response.
    // This happens if the server has set the NEL policy previously, but doesn't
    // set the NEL policy for the signed exchange response, and the IP address
    // has changed due to DNS round robin.
    if (details.server_ip_address != policy->received_ip_address) {
      RecordSignedExchangeRequestOutcome(
          RequestOutcome::kDiscardedIPAddressMismatch);
      return;
    }
    const base::Optional<double> sampling_fraction =
        SampleAndReturnFraction(*policy, details.success);
    if (!sampling_fraction.has_value()) {
      RecordSignedExchangeRequestOutcome(
          details.success ? RequestOutcome::kDiscardedUnsampledSuccess
                          : RequestOutcome::kDiscardedUnsampledFailure);
      return;
    }
    reporting_service_->QueueReport(
        details.outer_url, details.user_agent, policy->report_to, kReportType,
        CreateSignedExchangeReportBody(details, sampling_fraction.value()),
        0 /* depth */);
    RecordSignedExchangeRequestOutcome(RequestOutcome::kQueued);
  }

  void RemoveBrowsingData(const base::RepeatingCallback<bool(const GURL&)>&
                              origin_filter) override {
    for (auto it = policies_.begin(); it != policies_.end();) {
      const url::Origin& origin = it->first;
      // Remove policies matching the filter.
      if (origin_filter.Run(origin.GetURL())) {
        it = RemovePolicy(it);
      } else {
        ++it;
      }
    }
  }

  void RemoveAllBrowsingData() override {
    wildcard_policies_.clear();
    policies_.clear();
  }

  base::Value StatusAsValue() const override {
    base::Value dict(base::Value::Type::DICTIONARY);
    std::vector<base::Value> policy_list;
    // We wanted sorted (or at least reproducible) output; luckily, policies_ is
    // a std::map, and therefore already sorted.
    for (const auto& origin_and_policy : policies_) {
      const auto& origin = origin_and_policy.first;
      const auto& policy = origin_and_policy.second;
      base::Value policy_dict(base::Value::Type::DICTIONARY);
      policy_dict.SetKey("origin", base::Value(origin.Serialize()));
      policy_dict.SetKey("includeSubdomains",
                         base::Value(policy.include_subdomains));
      policy_dict.SetKey("reportTo", base::Value(policy.report_to));
      policy_dict.SetKey("expires",
                         base::Value(NetLog::TimeToString(policy.expires)));
      policy_dict.SetKey("successFraction",
                         base::Value(policy.success_fraction));
      policy_dict.SetKey("failureFraction",
                         base::Value(policy.failure_fraction));
      policy_list.push_back(std::move(policy_dict));
    }
    dict.SetKey("originPolicies", base::Value(std::move(policy_list)));
    return dict;
  }

  std::set<url::Origin> GetPolicyOriginsForTesting() override {
    std::set<url::Origin> origins;
    for (const auto& entry : policies_) {
      origins.insert(entry.first);
    }
    return origins;
  }

 private:
  // Map from origin to origin's (owned) policy.
  // Would be unordered_map, but url::Origin has no hash.
  using PolicyMap = std::map<url::Origin, NELPolicy>;

  // Wildcard policies are policies for which the include_subdomains flag is
  // set.
  //
  // Wildcard policies are accessed by domain name, not full origin, so there
  // can be multiple wildcard policies per domain name.
  //
  // This is a map from domain name to the set of pointers to wildcard policies
  // in that domain.
  //
  // Policies in the map are unowned; they are pointers to the original in the
  // PolicyMap.
  using WildcardPolicyMap = std::map<std::string, std::set<const NELPolicy*>>;

  PolicyMap policies_;
  WildcardPolicyMap wildcard_policies_;

  // The persistent store in which NEL policies will be stored to disk, if not
  // null. If |store_| is null, then NEL policies will be in-memory only.
  // The store is owned by the URLRequestContext because Reporting also needs
  // access to it.
  // TODO(chlily): Implement.
  PersistentNELStore* store_;

  HeaderOutcome ParseHeader(const std::string& json_value,
                            base::Time now,
                            NELPolicy* policy_out) const {
    DCHECK(policy_out);

    if (json_value.size() > kMaxJsonSize)
      return HeaderOutcome::DISCARDED_JSON_TOO_BIG;

    std::unique_ptr<base::Value> value = base::JSONReader::ReadDeprecated(
        json_value, base::JSON_PARSE_RFC, kMaxJsonDepth);
    if (!value)
      return HeaderOutcome::DISCARDED_JSON_INVALID;

    const base::DictionaryValue* dict = nullptr;
    if (!value->GetAsDictionary(&dict))
      return HeaderOutcome::DISCARDED_NOT_DICTIONARY;

    if (!dict->HasKey(kMaxAgeKey))
      return HeaderOutcome::DISCARDED_TTL_MISSING;
    int max_age_sec;
    if (!dict->GetInteger(kMaxAgeKey, &max_age_sec))
      return HeaderOutcome::DISCARDED_TTL_NOT_INTEGER;
    if (max_age_sec < 0)
      return HeaderOutcome::DISCARDED_TTL_NEGATIVE;

    std::string report_to;
    if (max_age_sec > 0) {
      if (!dict->HasKey(kReportToKey))
        return HeaderOutcome::DISCARDED_REPORT_TO_MISSING;
      if (!dict->GetString(kReportToKey, &report_to))
        return HeaderOutcome::DISCARDED_REPORT_TO_NOT_STRING;
    }

    bool include_subdomains = false;
    // include_subdomains is optional and defaults to false, so it's okay if
    // GetBoolean fails.
    dict->GetBoolean(kIncludeSubdomainsKey, &include_subdomains);

    double success_fraction = 0.0;
    // success_fraction is optional and defaults to 0.0, so it's okay if
    // GetDouble fails.
    dict->GetDouble(kSuccessFractionKey, &success_fraction);

    double failure_fraction = 1.0;
    // failure_fraction is optional and defaults to 1.0, so it's okay if
    // GetDouble fails.
    dict->GetDouble(kFailureFractionKey, &failure_fraction);

    policy_out->report_to = report_to;
    policy_out->include_subdomains = include_subdomains;
    policy_out->success_fraction = success_fraction;
    policy_out->failure_fraction = failure_fraction;
    if (max_age_sec > 0) {
      policy_out->expires = now + base::TimeDelta::FromSeconds(max_age_sec);
      return HeaderOutcome::SET;
    } else {
      policy_out->expires = base::Time();
      return HeaderOutcome::REMOVED;
    }
  }

  const NELPolicy* FindPolicyForOrigin(const url::Origin& origin) const {
    auto it = policies_.find(origin);
    if (it != policies_.end() && clock_->Now() < it->second.expires)
      return &it->second;

    std::string domain = origin.host();
    const NELPolicy* wildcard_policy = nullptr;
    while (!wildcard_policy && !domain.empty()) {
      wildcard_policy = FindWildcardPolicyForDomain(domain);
      domain = GetSuperdomain(domain);
    }

    return wildcard_policy;
  }

  const NELPolicy* FindWildcardPolicyForDomain(
      const std::string& domain) const {
    DCHECK(!domain.empty());

    auto it = wildcard_policies_.find(domain);
    if (it == wildcard_policies_.end())
      return nullptr;

    DCHECK(!it->second.empty());

    // TODO(juliatuttle): Come up with a deterministic way to resolve these.
    if (it->second.size() > 1) {
      LOG(WARNING) << "Domain " << domain
                   << " matches multiple origins with include_subdomains; "
                   << "choosing one arbitrarily.";
    }

    for (auto jt = it->second.begin(); jt != it->second.end(); ++jt) {
      if (clock_->Now() < (*jt)->expires)
        return *jt;
    }

    return nullptr;
  }

  void MaybeAddWildcardPolicy(const url::Origin& origin,
                              const NELPolicy* policy) {
    DCHECK(policy);
    DCHECK_EQ(policy, &policies_[origin]);

    if (!policy->include_subdomains)
      return;

    auto inserted = wildcard_policies_[origin.host()].insert(policy);
    DCHECK(inserted.second);
  }

  // Removes the policy pointed to by |policy_it|. Invalidates |policy_it|.
  // Returns the iterator to the next element.
  PolicyMap::iterator RemovePolicy(PolicyMap::iterator policy_it) {
    DCHECK(policy_it != policies_.end());
    NELPolicy* policy = &policy_it->second;
    MaybeRemoveWildcardPolicy(policy);
    return policies_.erase(policy_it);
  }

  void MaybeRemoveWildcardPolicy(const NELPolicy* policy) {
    DCHECK(policy);

    if (!policy->include_subdomains)
      return;

    const url::Origin& origin = policy->origin;
    DCHECK_EQ(policy, &policies_[origin]);

    auto wildcard_it = wildcard_policies_.find(origin.host());
    DCHECK(wildcard_it != wildcard_policies_.end());

    size_t erased = wildcard_it->second.erase(policy);
    DCHECK_EQ(1u, erased);
    if (wildcard_it->second.empty())
      wildcard_policies_.erase(wildcard_it);
  }

  void RemoveAllExpiredPolicies() {
    for (auto it = policies_.begin(); it != policies_.end();) {
      if (it->second.expires < clock_->Now()) {
        it = RemovePolicy(it);
      } else {
        ++it;
      }
    }
  }

  void EvictStalestPolicy() {
    PolicyMap::iterator stalest_it = policies_.begin();
    for (auto it = policies_.begin(); it != policies_.end(); ++it) {
      if (it->second.last_used < stalest_it->second.last_used)
        stalest_it = it;
    }

    // This should only be called if we have hit the max policy limit, so there
    // should be at least one policy.
    DCHECK(stalest_it != policies_.end());

    RemovePolicy(stalest_it);
  }

  std::unique_ptr<const base::Value> CreateReportBody(
      const std::string& phase,
      const std::string& type,
      double sampling_fraction,
      const RequestDetails& details) const {
    auto body = std::make_unique<base::DictionaryValue>();

    body->SetString(kReferrerKey, details.referrer.spec());
    body->SetDouble(kSamplingFractionKey, sampling_fraction);
    body->SetString(kServerIpKey, details.server_ip.ToString());
    body->SetString(kProtocolKey, details.protocol);
    body->SetString(kMethodKey, details.method);
    body->SetInteger(kStatusCodeKey, details.status_code);
    body->SetInteger(kElapsedTimeKey, details.elapsed_time.InMilliseconds());
    body->SetString(kPhaseKey, phase);
    body->SetString(kTypeKey, type);

    return std::move(body);
  }

  std::unique_ptr<const base::Value> CreateSignedExchangeReportBody(
      const SignedExchangeReportDetails& details,
      double sampling_fraction) const {
    auto body = std::make_unique<base::DictionaryValue>();
    body->SetString(kPhaseKey, kSignedExchangePhaseValue);
    body->SetString(kTypeKey, details.type);
    body->SetDouble(kSamplingFractionKey, sampling_fraction);
    body->SetString(kReferrerKey, details.referrer);
    body->SetString(kServerIpKey, details.server_ip_address.ToString());
    body->SetString(kProtocolKey, details.protocol);
    body->SetString(kMethodKey, details.method);
    body->SetInteger(kStatusCodeKey, details.status_code);
    body->SetInteger(kElapsedTimeKey, details.elapsed_time.InMilliseconds());

    auto sxg_body = std::make_unique<base::DictionaryValue>();
    sxg_body->SetKey(kOuterUrlKey, base::Value(details.outer_url.spec()));
    if (details.inner_url.is_valid())
      sxg_body->SetKey(kInnerUrlKey, base::Value(details.inner_url.spec()));

    base::Value cert_url_list = base::Value(base::Value::Type::LIST);
    if (details.cert_url.is_valid())
      cert_url_list.GetList().push_back(base::Value(details.cert_url.spec()));
    sxg_body->SetKey(kCertUrlKey, std::move(cert_url_list));
    body->SetDictionary(kSignedExchangeBodyKey, std::move(sxg_body));

    return std::move(body);
  }

  bool IsMismatchingSubdomainReport(const NELPolicy& policy,
                                    const url::Origin& report_origin) const {
    return policy.include_subdomains && (policy.origin != report_origin);
  }

  // Returns a valid value of matching fraction iff the event should be sampled.
  base::Optional<double> SampleAndReturnFraction(const NELPolicy& policy,
                                                 bool success) const {
    const double sampling_fraction =
        success ? policy.success_fraction : policy.failure_fraction;
    if (base::RandDouble() >= sampling_fraction)
      return base::nullopt;
    return sampling_fraction;
  }
};

}  // namespace

NetworkErrorLoggingService::NELPolicy::NELPolicy() = default;

NetworkErrorLoggingService::NELPolicy::NELPolicy(const NELPolicy& other) =
    default;

NetworkErrorLoggingService::NELPolicy::~NELPolicy() = default;

NetworkErrorLoggingService::RequestDetails::RequestDetails() = default;

NetworkErrorLoggingService::RequestDetails::RequestDetails(
    const RequestDetails& other) = default;

NetworkErrorLoggingService::RequestDetails::~RequestDetails() = default;

NetworkErrorLoggingService::SignedExchangeReportDetails::
    SignedExchangeReportDetails() = default;

NetworkErrorLoggingService::SignedExchangeReportDetails::
    SignedExchangeReportDetails(const SignedExchangeReportDetails& other) =
        default;

NetworkErrorLoggingService::SignedExchangeReportDetails::
    ~SignedExchangeReportDetails() = default;

const char NetworkErrorLoggingService::kHeaderName[] = "NEL";

const char NetworkErrorLoggingService::kReportType[] = "network-error";

const char NetworkErrorLoggingService::kHeaderOutcomeHistogram[] =
    "Net.NetworkErrorLogging.HeaderOutcome";

const char NetworkErrorLoggingService::kRequestOutcomeHistogram[] =
    "Net.NetworkErrorLogging.RequestOutcome";

const char
    NetworkErrorLoggingService::kSignedExchangeRequestOutcomeHistogram[] =
        "Net.NetworkErrorLogging.SignedExchangeRequestOutcome";

// Allow NEL reports on regular requests, plus NEL reports on Reporting uploads
// containing only regular requests, but do not allow NEL reports on Reporting
// uploads containing Reporting uploads.
//
// This prevents origins from building purposefully-broken Reporting endpoints
// that generate new NEL reports to bypass the age limit on Reporting reports.
const int NetworkErrorLoggingService::kMaxNestedReportDepth = 1;

const char NetworkErrorLoggingService::kReferrerKey[] = "referrer";
const char NetworkErrorLoggingService::kSamplingFractionKey[] =
    "sampling_fraction";
const char NetworkErrorLoggingService::kServerIpKey[] = "server_ip";
const char NetworkErrorLoggingService::kProtocolKey[] = "protocol";
const char NetworkErrorLoggingService::kMethodKey[] = "method";
const char NetworkErrorLoggingService::kStatusCodeKey[] = "status_code";
const char NetworkErrorLoggingService::kElapsedTimeKey[] = "elapsed_time";
const char NetworkErrorLoggingService::kPhaseKey[] = "phase";
const char NetworkErrorLoggingService::kTypeKey[] = "type";

const char NetworkErrorLoggingService::kSignedExchangePhaseValue[] = "sxg";
const char NetworkErrorLoggingService::kSignedExchangeBodyKey[] = "sxg";
const char NetworkErrorLoggingService::kOuterUrlKey[] = "outer_url";
const char NetworkErrorLoggingService::kInnerUrlKey[] = "inner_url";
const char NetworkErrorLoggingService::kCertUrlKey[] = "cert_url";

// See also: max number of Reporting endpoints specified in ReportingPolicy.
const size_t NetworkErrorLoggingService::kMaxPolicies = 1000u;

// static
void NetworkErrorLoggingService::
    RecordHeaderDiscardedForNoNetworkErrorLoggingService() {
  RecordHeaderOutcome(
      HeaderOutcome::DISCARDED_NO_NETWORK_ERROR_LOGGING_SERVICE);
}

// static
void NetworkErrorLoggingService::RecordHeaderDiscardedForInvalidSSLInfo() {
  RecordHeaderOutcome(HeaderOutcome::DISCARDED_INVALID_SSL_INFO);
}

// static
void NetworkErrorLoggingService::RecordHeaderDiscardedForCertStatusError() {
  RecordHeaderOutcome(HeaderOutcome::DISCARDED_CERT_STATUS_ERROR);
}

// static
void NetworkErrorLoggingService::
    RecordHeaderDiscardedForMissingRemoteEndpoint() {
  RecordHeaderOutcome(HeaderOutcome::DISCARDED_MISSING_REMOTE_ENDPOINT);
}

// static
void NetworkErrorLoggingService::
    RecordRequestDiscardedForNoNetworkErrorLoggingService() {
  RecordRequestOutcome(RequestOutcome::kDiscardedNoNetworkErrorLoggingService);
}

// static
void NetworkErrorLoggingService::RecordRequestDiscardedForInsecureOrigin() {
  RecordRequestOutcome(RequestOutcome::kDiscardedInsecureOrigin);
}

// static
std::unique_ptr<NetworkErrorLoggingService> NetworkErrorLoggingService::Create(
    PersistentNELStore* store) {
  return std::make_unique<NetworkErrorLoggingServiceImpl>(store);
}

NetworkErrorLoggingService::~NetworkErrorLoggingService() = default;

void NetworkErrorLoggingService::SetReportingService(
    ReportingService* reporting_service) {
  reporting_service_ = reporting_service;
}

void NetworkErrorLoggingService::OnShutdown() {
  shut_down_ = true;
  SetReportingService(nullptr);
}

void NetworkErrorLoggingService::SetClockForTesting(const base::Clock* clock) {
  clock_ = clock;
}

base::Value NetworkErrorLoggingService::StatusAsValue() const {
  NOTIMPLEMENTED();
  return base::Value();
}

std::set<url::Origin> NetworkErrorLoggingService::GetPolicyOriginsForTesting() {
  NOTIMPLEMENTED();
  return std::set<url::Origin>();
}

NetworkErrorLoggingService::NetworkErrorLoggingService()
    : clock_(base::DefaultClock::GetInstance()),
      reporting_service_(nullptr),
      shut_down_(false) {}

}  // namespace net
