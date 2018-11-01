// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/cert_database.h"

#include <Security/Security.h>

#include "base/location.h"
#include "base/logging.h"
#include "base/mac/mac_logging.h"
#include "base/message_loop/message_loop.h"
#include "base/message_loop/message_loop_current.h"
#include "base/observer_list_threadsafe.h"
#include "base/process/process_handle.h"
#include "base/single_thread_task_runner.h"
#include "base/synchronization/lock.h"
#include "crypto/mac_security_services_lock.h"
#include "net/base/net_errors.h"
#include "net/cert/x509_certificate.h"

namespace net {

// Helper that observes events from the Keychain and forwards them to the
// given CertDatabase.
class CertDatabase::Notifier {
 public:
  // Creates a new Notifier that will forward Keychain events to |cert_db|.
  // |message_loop| must refer to a thread with an associated CFRunLoop - a
  // TYPE_UI thread. Events will be dispatched from this message loop.
  Notifier(CertDatabase* cert_db,
           scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : cert_db_(cert_db),
        task_runner_(std::move(task_runner)),
        registered_(false),
        called_shutdown_(false) {
    // Ensure an associated CFRunLoop.
    DCHECK(base::MessageLoopForUI::IsCurrent());
    DCHECK(task_runner_->BelongsToCurrentThread());
    task_runner_->PostTask(FROM_HERE,
                           base::Bind(&Notifier::Init,
                                      base::Unretained(this)));
  }

  // Should be called from the |task_runner_|'s sequence. Use Shutdown()
  // to shutdown on arbitrary sequence.
  ~Notifier() {
    DCHECK(called_shutdown_);
    // Only unregister from the same sequence where registration was performed.
    if (registered_ && task_runner_->RunsTasksInCurrentSequence())
      SecKeychainRemoveCallback(&Notifier::KeychainCallback);
  }

  void Shutdown() {
    called_shutdown_ = true;
    if (!task_runner_->DeleteSoon(FROM_HERE, this)) {
      // If the task runner is no longer running, it's safe to just delete
      // the object, since no further events will or can be delivered by
      // Keychain Services.
      delete this;
    }
  }

 private:
  void Init() {
    SecKeychainEventMask event_mask =
        kSecKeychainListChangedMask | kSecTrustSettingsChangedEventMask;
    OSStatus status = SecKeychainAddCallback(&Notifier::KeychainCallback,
                                             event_mask, this);
    if (status == noErr)
      registered_ = true;
  }

  // SecKeychainCallback function that receives notifications from securityd
  // and forwards them to the |cert_db_|.
  static OSStatus KeychainCallback(SecKeychainEvent keychain_event,
                                   SecKeychainCallbackInfo* info,
                                   void* context);

  CertDatabase* const cert_db_;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  bool registered_;
  bool called_shutdown_;
};

// static
OSStatus CertDatabase::Notifier::KeychainCallback(
    SecKeychainEvent keychain_event,
    SecKeychainCallbackInfo* info,
    void* context) {
  Notifier* that = reinterpret_cast<Notifier*>(context);

  if (info->version > SEC_KEYCHAIN_SETTINGS_VERS1) {
    NOTREACHED();
    return errSecWrongSecVersion;
  }

  if (info->pid == base::GetCurrentProcId()) {
    // Ignore events generated by the current process, as the assumption is
    // that they have already been handled. This may miss events that
    // originated as a result of spawning native dialogs that allow the user
    // to modify Keychain settings. However, err on the side of missing
    // events rather than sending too many events.
    return errSecSuccess;
  }

  switch (keychain_event) {
    case kSecKeychainListChangedEvent:
    case kSecTrustSettingsChangedEvent:
      that->cert_db_->NotifyObserversCertDBChanged();
      break;

    default:
      break;
  }

  return errSecSuccess;
}

void CertDatabase::StartListeningForKeychainEvents() {
  ReleaseNotifier();
  notifier_ = new Notifier(this, base::ThreadTaskRunnerHandle::Get());
}

void CertDatabase::ReleaseNotifier() {
  // Shutdown will take care to delete the notifier on the right thread.
  if (notifier_) {
    notifier_->Shutdown();
    notifier_ = nullptr;
  }
}

}  // namespace net
