// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/quic_connection_helper.h"

#include "base/location.h"
#include "base/logging.h"
#include "base/task_runner.h"
#include "base/time.h"
#include "net/quic/congestion_control/quic_receipt_metrics_collector.h"
#include "net/quic/congestion_control/quic_send_scheduler.h"
#include "net/quic/quic_utils.h"

namespace net {

QuicConnectionHelper::QuicConnectionHelper(base::TaskRunner* task_runner,
                                           QuicClock* clock,
                                           DatagramClientSocket* socket)
    : ALLOW_THIS_IN_INITIALIZER_LIST(weak_factory_(this)),
      task_runner_(task_runner),
      socket_(socket),
      clock_(clock),
      send_alarm_registered_(false),
      timeout_alarm_registered_(false) {
}

QuicConnectionHelper::~QuicConnectionHelper() {
}

void QuicConnectionHelper::SetConnection(QuicConnection* connection) {
  connection_ = connection;
}

QuicClock* QuicConnectionHelper::GetClock() {
  return clock_;
}

int QuicConnectionHelper::WritePacketToWire(
    QuicPacketSequenceNumber sequence_number,
    const QuicEncryptedPacket& packet,
    bool resend,
    int* error) {
  if (connection_->ShouldSimulateLostPacket()) {
    DLOG(INFO) << "Dropping "
               << (resend ? "data bearing " : " ack only ")
               << "packet " << sequence_number
               << " due to fake packet loss.";
    *error = 0;
    return packet.length();
  }

  // TODO(rch): add udp socket write
  return packet.length();
}

void QuicConnectionHelper::SetResendAlarm(
    QuicPacketSequenceNumber sequence_number,
    uint64 delay_in_us) {
  // TODO(rch): Coalesce these alarms.
  task_runner_->PostDelayedTask(
      FROM_HERE,
      base::Bind(&QuicConnectionHelper::OnResendAlarm,
                 weak_factory_.GetWeakPtr(), sequence_number),
      base::TimeDelta::FromMicroseconds(delay_in_us));
}

void QuicConnectionHelper::SetSendAlarm(uint64 delay_in_us) {
  DCHECK(!send_alarm_registered_);
  send_alarm_registered_ = true;
  task_runner_->PostDelayedTask(
      FROM_HERE,
      base::Bind(&QuicConnectionHelper::OnSendAlarm,
                 weak_factory_.GetWeakPtr()),
      base::TimeDelta::FromMicroseconds(delay_in_us));
}

void QuicConnectionHelper::SetTimeoutAlarm(uint64 delay_in_us) {
  DCHECK(!timeout_alarm_registered_);
  timeout_alarm_registered_ = true;
  task_runner_->PostDelayedTask(
      FROM_HERE,
      base::Bind(&QuicConnectionHelper::OnTimeoutAlarm,
                 weak_factory_.GetWeakPtr()),
      base::TimeDelta::FromMicroseconds(delay_in_us));
}

bool QuicConnectionHelper::IsSendAlarmSet() {
  return send_alarm_registered_;
}

void QuicConnectionHelper::UnregisterSendAlarmIfRegistered() {
  send_alarm_registered_ = false;
}

void QuicConnectionHelper::OnResendAlarm(
    QuicPacketSequenceNumber sequence_number) {
  connection_->MaybeResendPacket(sequence_number);
}

void QuicConnectionHelper::OnSendAlarm() {
  if (send_alarm_registered_) {
    send_alarm_registered_ = false;
    connection_->OnCanWrite();
  }
}

void QuicConnectionHelper::OnTimeoutAlarm() {
  timeout_alarm_registered_ = false;
  connection_->CheckForTimeout();
}

}  // namespace net
