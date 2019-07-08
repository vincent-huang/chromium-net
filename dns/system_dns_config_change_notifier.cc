// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/dns/system_dns_config_change_notifier.h"

#include <map>
#include <utility>

#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/sequenced_task_runner.h"
#include "base/synchronization/lock.h"
#include "base/task/post_task.h"
#include "base/task/task_traits.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "net/dns/dns_config_service.h"

namespace net {

namespace {

// Internal information and handling for a registered Observer. Handles
// posting to and DCHECKing the correct sequence for the Observer.
class WrappedObserver {
 public:
  explicit WrappedObserver(SystemDnsConfigChangeNotifier::Observer* observer)
      : task_runner_(base::SequencedTaskRunnerHandle::Get()),
        observer_(observer) {}

  ~WrappedObserver() { DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_); }

  void OnNotifyThreadsafe(base::Optional<DnsConfig> config) {
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&WrappedObserver::OnNotify,
                       weak_ptr_factory_.GetWeakPtr(), std::move(config)));
  }

  void OnNotify(base::Optional<DnsConfig> config) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    DCHECK(!config || config.value().IsValid());

    observer_->OnSystemDnsConfigChanged(std::move(config));
  }

 private:
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  SystemDnsConfigChangeNotifier::Observer* const observer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<WrappedObserver> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(WrappedObserver);
};

}  // namespace

// Internal core to be destroyed via base::OnTaskRunnerDeleter to ensure
// sequence safety.
class SystemDnsConfigChangeNotifier::Core {
 public:
  Core(scoped_refptr<base::SequencedTaskRunner> task_runner,
       std::unique_ptr<DnsConfigService> dns_config_service)
      : task_runner_(std::move(task_runner)),
        dns_config_service_(std::move(dns_config_service)) {
    DCHECK(task_runner_);
    DCHECK(dns_config_service_);

    DETACH_FROM_SEQUENCE(sequence_checker_);

    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&Core::StartWatching, weak_ptr_factory_.GetWeakPtr()));
  }

  ~Core() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    DCHECK(wrapped_observers_.empty());
  }

  void AddObserver(Observer* observer) {
    // Create wrapped observer outside locking in case construction requires
    // complex side effects.
    auto wrapped_observer = std::make_unique<WrappedObserver>(observer);

    {
      base::AutoLock lock(lock_);

      if (config_) {
        // Even though this is the same sequence as the observer, use the
        // threadsafe OnNotify to post the notification for both lock and
        // reentrancy safety.
        wrapped_observer->OnNotifyThreadsafe(config_);
      }

      DCHECK_EQ(0u, wrapped_observers_.count(observer));
      wrapped_observers_.emplace(observer, std::move(wrapped_observer));
    }
  }

  void RemoveObserver(Observer* observer) {
    // Destroy wrapped observer outside locking in case destruction requires
    // complex side effects.
    std::unique_ptr<WrappedObserver> removed_wrapped_observer;

    {
      base::AutoLock lock(lock_);
      auto it = wrapped_observers_.find(observer);
      DCHECK(it != wrapped_observers_.end());
      removed_wrapped_observer = std::move(it->second);
      wrapped_observers_.erase(it);
    }
  }

  void RefreshConfig() {
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(&Core::TriggerRefreshConfig,
                                          weak_ptr_factory_.GetWeakPtr()));
  }

 private:
  void StartWatching() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    dns_config_service_->WatchConfig(base::BindRepeating(
        &Core::OnConfigChanged, weak_ptr_factory_.GetWeakPtr()));
  }

  void OnConfigChanged(const DnsConfig& config) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    base::AutoLock lock(lock_);

    // |config_| is |base::nullopt| if most recent config was invalid (or no
    // valid config has yet been read), so convert |config| to a similar form
    // before comparing for change.
    base::Optional<DnsConfig> new_config;
    if (config.IsValid())
      new_config = config;

    if (config_ == new_config)
      return;

    config_ = std::move(new_config);

    for (auto& wrapped_observer : wrapped_observers_) {
      wrapped_observer.second->OnNotifyThreadsafe(config_);
    }
  }

  void TriggerRefreshConfig() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    dns_config_service_->RefreshConfig();
  }

  // Fields that may be accessed from any sequence. Must protect access using
  // |lock_|.
  mutable base::Lock lock_;
  // Only stores valid configs. |base::nullopt| if most recent config was
  // invalid (or no valid config has yet been read).
  base::Optional<DnsConfig> config_;
  std::map<Observer*, std::unique_ptr<WrappedObserver>> wrapped_observers_;

  // Fields valid only on |task_runner_|.
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  SEQUENCE_CHECKER(sequence_checker_);
  std::unique_ptr<DnsConfigService> dns_config_service_;
  base::WeakPtrFactory<Core> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(Core);
};

SystemDnsConfigChangeNotifier::SystemDnsConfigChangeNotifier()
    : SystemDnsConfigChangeNotifier(
          base::CreateSequencedTaskRunnerWithTraits({base::MayBlock()}),
          DnsConfigService::CreateSystemService()) {}

SystemDnsConfigChangeNotifier::SystemDnsConfigChangeNotifier(
    scoped_refptr<base::SequencedTaskRunner> task_runner,
    std::unique_ptr<DnsConfigService> dns_config_service)
    : core_(new Core(task_runner, std::move(dns_config_service)),
            base::OnTaskRunnerDeleter(task_runner)) {}

SystemDnsConfigChangeNotifier::~SystemDnsConfigChangeNotifier() = default;

void SystemDnsConfigChangeNotifier::AddObserver(Observer* observer) {
  core_->AddObserver(observer);
}

void SystemDnsConfigChangeNotifier::RemoveObserver(Observer* observer) {
  core_->RemoveObserver(observer);
}

void SystemDnsConfigChangeNotifier::RefreshConfig() {
  core_->RefreshConfig();
}

}  // namespace net
