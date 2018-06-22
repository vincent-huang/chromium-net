// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file intentionally does not have header guards, it's included
// inside a macro to generate values. The following line silences a
// presubmit warning that would otherwise be triggered by this:
// no-include-guard-because-multiply-included

// This file contains the list of QUIC protocol flags.

// Time period for which a given connection_id should live in the time-wait
// state.
QUIC_FLAG(int64_t, FLAGS_quic_time_wait_list_seconds, 200)

// Currently, this number is quite conservative.  The max QPS limit for an
// individual server silo is currently set to 1000 qps, though the actual max
// that we see in the wild is closer to 450 qps.  Regardless, this means that
// the longest time-wait list we should see is 200 seconds * 1000 qps, 200000.
// Of course, there are usually many queries per QUIC connection, so we allow a
// factor of 3 leeway.
//
// Maximum number of connections on the time-wait list. A negative value implies
// no configured limit.
QUIC_FLAG(int64_t, FLAGS_quic_time_wait_list_max_connections, 600000)

// Enables server-side support for QUIC stateless rejects.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_enable_quic_stateless_reject_support,
          true)

// If true, require handshake confirmation for QUIC connections, functionally
// disabling 0-rtt handshakes.
// TODO(rtenneti): Enable this flag after CryptoServerTest's are fixed.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_quic_require_handshake_confirmation,
          false)

// If true, disable pacing in QUIC.
QUIC_FLAG(bool, FLAGS_quic_disable_pacing_for_perf_tests, false)

// If true, QUIC will use cheap stateless rejects without creating a full
// connection.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_quic_use_cheap_stateless_rejects,
          true)

// If true, allows packets to be buffered in anticipation of a future CHLO, and
// allow CHLO packets to be buffered until next iteration of the event loop.
QUIC_FLAG(bool, FLAGS_quic_allow_chlo_buffering, true)

// If true, GFE sends spdy::SETTINGS_MAX_HEADER_LIST_SIZE to the client at the
// beginning of a connection.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_send_max_header_list_size, true)

// If greater than zero, mean RTT variation is multiplied by the specified
// factor and added to the congestion window limit.
QUIC_FLAG(double, FLAGS_quic_bbr_rtt_variation_weight, 0.0f)

// Congestion window gain for QUIC BBR during PROBE_BW phase.
QUIC_FLAG(double, FLAGS_quic_bbr_cwnd_gain, 2.0f)

// Simplify QUIC\'s adaptive time loss detection to measure the necessary
// reordering window for every spurious retransmit.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_fix_adaptive_time_loss, false)

// Allows the 3RTO QUIC connection option to close a QUIC connection after
// 3RTOs if there are no open streams.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_enable_3rtos, false)

// If true, enable experiment for testing PCC congestion-control.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_enable_pcc2, false)

// When true, defaults to BBR congestion control instead of Cubic.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_default_to_bbr, false)

// If buffered data in QUIC stream is less than this threshold, buffers all
// provided data or asks upper layer for more data.
QUIC_FLAG(uint32_t, FLAGS_quic_buffered_data_threshold, 8192u)

// Max size of data slice in bytes for QUIC stream send buffer.
QUIC_FLAG(uint32_t, FLAGS_quic_send_buffer_max_data_slice_size, 4096u)

// If true, QUIC supports both QUIC Crypto and TLS 1.3 for the handshake
// protocol.
QUIC_FLAG(bool, FLAGS_quic_supports_tls_handshake, false)

// Allow QUIC to accept initial packet numbers that are random, not 1.
QUIC_FLAG(bool, FLAGS_quic_restart_flag_quic_enable_accept_random_ipn, false)

// If true, enable QUIC v43.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_enable_version_43, true)

// Enables 3 new connection options to make PROBE_RTT more aggressive
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_bbr_less_probe_rtt, false)

// If true, limit quic stream length to be below 2^62.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_stream_too_long, false)

// If true, enable QUIC v99.
QUIC_FLAG(bool, FLAGS_quic_enable_version_99, false)

QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_disable_version_37, true)
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_disable_version_38, true)
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_disable_version_41, false)

// If true, framer will process and report ack frame incrementally.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_quic_use_incremental_ack_processing4,
          true)

// If this flag and
// FLAGS_quic_reloadable_flag_quic_fix_write_out_of_order_queued_packet_crash
// are both ture, QUIC will clear queued packets before sending connectivity
// probing packets.
QUIC_FLAG(
    bool,
    FLAGS_quic_reloadable_flag_quic_clear_queued_packets_before_sending_connectivity_probing,
    false)

// When true, set the initial congestion control window from connection options
// in QuicSentPacketManager rather than TcpCubicSenderBytes.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_unified_iw_options, false)

// Number of packets that the pacing sender allows in bursts during pacing.
QUIC_FLAG(int32_t, FLAGS_quic_lumpy_pacing_size, 1)

// Congestion window fraction that the pacing sender allows in bursts during
// pacing.
QUIC_FLAG(double, FLAGS_quic_lumpy_pacing_cwnd_fraction, 0.25f)

// Default enables QUIC ack decimation and adds a connection option to disable
// it.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_enable_ack_decimation, false)

// Enables the 1RTO connection option which only sends one packet on QUIC
// retransmission timeout, instead of 2.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_one_rto, false)

// When true, the NRTT QUIC connection option causes receivers to ignore
// incoming initial RTT values.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_no_irtt, false)

// Fixed QUIC's PROBE_BW logic to exit low gain mode based on bytes_in_flight,
// not prior_in_flight.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_bbr_fix_probe_bw, true)

// If true, changes when the dispatcher changes internal state.
QUIC_FLAG(bool, FLAGS_quic_restart_flag_quic_enable_l1_munge, true)

// Don't slow down the pacing rate in STARTUP upon loss if there hasn't been
// at least one non app-limited sample.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_bbr_slower_startup2, true)

// If true, put ScopedRetransmissionScheduler's functionality to
// ScopedPacketFlusher.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_quic_deprecate_scoped_scheduler2,
          true)

// If it's been more than SRTT since receiving a packet, set the ack alarm for
// 1ms instead of the standard delayed ack timer.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_quic_fast_ack_after_quiescence,
          false)

// If true, QUIC offload pacing when using USPS as egress method.
QUIC_FLAG(bool, FLAGS_quic_restart_flag_quic_offload_pacing_to_usps, false)

// Time that QUIC can pace packets into the future in ms.
QUIC_FLAG(int32_t, FLAGS_quic_pace_time_into_future_ms, 10)

// If true, enable QUIC v44.
QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_enable_version_44, true)

// If true, export packet write results in QuicConnection.
QUIC_FLAG(
    bool,
    FLAGS_quic_reloadable_flag_quic_export_connection_write_packet_results,
    true)

// If true, enable extra CHECKs in ack processing to debug b/110029150.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_quic_extra_checks_in_ack_processing,
          false)

// If true, close connection if largest observed in ack frame is greater than
// largest sent packet.
QUIC_FLAG(bool,
          FLAGS_quic_reloadable_flag_quic_validate_ack_largest_observed,
          false)

// If true, QuicConnection::ProcessPacket will not set send alarm if it is write
// blocked.
QUIC_FLAG(
    bool,
    FLAGS_quic_reloadable_flag_quic_no_send_alarm_in_process_packet_if_write_blocked,
    false)
