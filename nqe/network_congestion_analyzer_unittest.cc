// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/nqe/network_congestion_analyzer.h"
#include <map>
#include <unordered_map>

#include "base/macros.h"
#include "base/optional.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/time/time.h"
#include "net/nqe/network_quality.h"
#include "net/nqe/observation_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace nqe {

namespace internal {

namespace {

constexpr float kEpsilon = 0.001f;

// These values should remain synchronized with the values in
// net/nqe/network_congestion_analyzer.cc.
constexpr int64_t kHighQueueingDelayMsec = 5000;
constexpr int64_t kMinEmptyQueueObservingTimeMsec = 1500;

// Verify that the network queueing delay is computed correctly based on RTT
// and downlink throughput observations.
TEST(NetworkCongestionAnalyzerTest, TestComputingQueueingDelay) {
  base::SimpleTestTickClock tick_clock;

  NetworkCongestionAnalyzer analyzer(&tick_clock);
  std::map<uint64_t, CanonicalStats> recent_rtt_stats;
  std::map<uint64_t, CanonicalStats> historical_rtt_stats;
  int32_t downlink_kbps = nqe::internal::INVALID_RTT_THROUGHPUT;

  // Checks that no result is updated when providing empty RTT observations and
  // an invalid downlink throughput observation.
  analyzer.ComputeRecentQueueingDelay(recent_rtt_stats, historical_rtt_stats,
                                      downlink_kbps);
  EXPECT_TRUE(analyzer.recent_queueing_delay().is_zero());

  const uint64_t host_1 = 0x101010UL;
  const uint64_t host_2 = 0x202020UL;
  // Checks that the queueing delay is updated based on hosts with valid RTT
  // observations. For example, the computation should be done by using data
  // from host 1 only because host 2 does not provide a valid min RTT value.
  std::map<int32_t, int32_t> recent_stat_host_1 = {{kStatVal0p, 1100}};
  std::map<int32_t, int32_t> historical_stat_host_1 = {{kStatVal0p, 600}};
  CanonicalStats recent_rtt_host_1 =
      CanonicalStats(recent_stat_host_1, 1400, 5);
  CanonicalStats historical_rtt_host_1 =
      CanonicalStats(historical_stat_host_1, 1400, 15);

  std::map<int32_t, int32_t> recent_stat_host_2 = {{kStatVal0p, 1200}};
  std::map<int32_t, int32_t> historical_stat_host_2 = {{kStatVal50p, 1200}};
  CanonicalStats recent_rtt_host_2 =
      CanonicalStats(recent_stat_host_2, 1600, 3);
  CanonicalStats historical_rtt_host_2 =
      CanonicalStats(historical_stat_host_2, 1600, 8);
  recent_rtt_stats.emplace(host_1, recent_rtt_host_1);
  recent_rtt_stats.emplace(host_2, recent_rtt_host_2);
  historical_rtt_stats.emplace(host_1, historical_rtt_host_1);
  historical_rtt_stats.emplace(host_2, historical_rtt_host_2);

  analyzer.ComputeRecentQueueingDelay(recent_rtt_stats, historical_rtt_stats,
                                      downlink_kbps);
  EXPECT_EQ(800, analyzer.recent_queueing_delay().InMilliseconds());

  // Checks that the queueing delay is updated correctly based on all hosts when
  // RTT observations and the throughput observation are valid.
  historical_rtt_stats[host_2].canonical_pcts[kStatVal0p] = 1000;
  downlink_kbps = 120;
  analyzer.ComputeRecentQueueingDelay(recent_rtt_stats, historical_rtt_stats,
                                      downlink_kbps);
  EXPECT_EQ(700, analyzer.recent_queueing_delay().InMilliseconds());
  EXPECT_NEAR(7.0, analyzer.recent_queue_length().value_or(0), kEpsilon);
}

}  // namespace

// Verify that the peak queueing delay is correctly mapped to the count of
// in-flight requests that are responsible for that delay.
TEST(NetworkCongestionAnalyzerTest, TestUpdatePeakDelayMapping) {
  base::SimpleTestTickClock tick_clock;

  NetworkCongestionAnalyzer analyzer(&tick_clock);
  EXPECT_EQ(base::nullopt,
            analyzer.count_inflight_requests_causing_high_delay());

  // Checks that a measurement period starts correctly when an empty queue
  // observation shows up.
  EXPECT_FALSE(analyzer.ShouldStartNewMeasurement(
      base::TimeDelta::FromMilliseconds(500), 2));
  EXPECT_TRUE(analyzer.ShouldStartNewMeasurement(
      base::TimeDelta::FromMilliseconds(500), 0));

  // Checks that a new measurement period starts after waiting for a sufficient
  // time interval when the number of in-flight requests is relatively low (=2).
  EXPECT_FALSE(analyzer.ShouldStartNewMeasurement(
      base::TimeDelta::FromMilliseconds(500), 2));
  tick_clock.Advance(
      base::TimeDelta::FromMilliseconds(kMinEmptyQueueObservingTimeMsec / 2));
  EXPECT_FALSE(analyzer.ShouldStartNewMeasurement(
      base::TimeDelta::FromMilliseconds(500), 2));
  tick_clock.Advance(
      base::TimeDelta::FromMilliseconds(kMinEmptyQueueObservingTimeMsec / 2));
  EXPECT_TRUE(analyzer.ShouldStartNewMeasurement(
      base::TimeDelta::FromMilliseconds(500), 2));

  // Checks that the count of in-flight requests for peak queueing delay is
  // correctly recorded.
  // Case #1: the peak queueing delay was observed after the max count (7) of
  // in-flight requests was observed.
  const size_t expected_count_requests_1 = 7;
  std::vector<std::pair<base::TimeDelta, size_t>> queueing_delay_samples_1 = {
      std::make_pair(base::TimeDelta::FromMilliseconds(10), 1),
      std::make_pair(base::TimeDelta::FromMilliseconds(10), 3),
      std::make_pair(base::TimeDelta::FromMilliseconds(400), 5),
      std::make_pair(base::TimeDelta::FromMilliseconds(800),
                     expected_count_requests_1),
      std::make_pair(base::TimeDelta::FromMilliseconds(kHighQueueingDelayMsec),
                     5),
      std::make_pair(base::TimeDelta::FromMilliseconds(1000), 3),
      std::make_pair(base::TimeDelta::FromMilliseconds(700), 3),
      std::make_pair(base::TimeDelta::FromMilliseconds(600), 1),
      std::make_pair(base::TimeDelta::FromMilliseconds(300), 0),
  };
  for (const auto& sample : queueing_delay_samples_1) {
    analyzer.UpdatePeakDelayMapping(sample.first, sample.second);
  }
  EXPECT_EQ(expected_count_requests_1,
            analyzer.count_inflight_requests_causing_high_delay().value_or(0));

  // Case #2: the peak queueing delay is observed before the max count (11) of
  // in-flight requests was observed. The 8 requests should be responsible for
  // the peak queueing delay.
  const size_t expected_count_requests_2 = 10;
  std::vector<std::pair<base::TimeDelta, size_t>> queueing_delay_samples_2 = {
      std::make_pair(base::TimeDelta::FromMilliseconds(10), 1),
      std::make_pair(base::TimeDelta::FromMilliseconds(10), 3),
      std::make_pair(base::TimeDelta::FromMilliseconds(400), 5),
      std::make_pair(base::TimeDelta::FromMilliseconds(800), 5),
      std::make_pair(base::TimeDelta::FromMilliseconds(kHighQueueingDelayMsec),
                     expected_count_requests_2),
      std::make_pair(base::TimeDelta::FromMilliseconds(3000), 11),
      std::make_pair(base::TimeDelta::FromMilliseconds(700), 3),
      std::make_pair(base::TimeDelta::FromMilliseconds(600), 1),
      std::make_pair(base::TimeDelta::FromMilliseconds(300), 0),
  };
  for (const auto& sample : queueing_delay_samples_2) {
    analyzer.UpdatePeakDelayMapping(sample.first, sample.second);
  }
  EXPECT_EQ(expected_count_requests_2,
            analyzer.count_inflight_requests_causing_high_delay().value_or(0));
}

}  // namespace internal

}  // namespace nqe

}  // namespace net