// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/proxy_resolution/dhcp_pac_file_fetcher_win.h"

#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/containers/queue.h"
#include "base/memory/free_deleter.h"
#include "base/synchronization/lock.h"
#include "base/task_runner.h"
#include "base/task_scheduler/post_task.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/values.h"
#include "net/base/net_errors.h"
#include "net/log/net_log.h"
#include "net/proxy_resolution/dhcp_pac_file_adapter_fetcher_win.h"

#include <winsock2.h>
#include <iphlpapi.h>

namespace net {

// This struct contains logging information describing how
// GetCandidateAdapterNames() performed, for output to NetLog.
struct DhcpAdapterNamesLoggingInfo {
  DhcpAdapterNamesLoggingInfo() = default;
  ~DhcpAdapterNamesLoggingInfo() = default;

  // The error that iphlpapi!GetAdaptersAddresses returned.
  ULONG error;

  // The adapters list that iphlpapi!GetAdaptersAddresses returned.
  std::unique_ptr<IP_ADAPTER_ADDRESSES, base::FreeDeleter> adapters;

  // The time immediately before GetCandidateAdapterNames was posted to a worker
  // thread from the origin thread.
  base::TimeTicks origin_thread_start_time;

  // The time when GetCandidateAdapterNames began running on the worker thread.
  base::TimeTicks worker_thread_start_time;

  // The time when GetCandidateAdapterNames completed running on the worker
  // thread.
  base::TimeTicks worker_thread_end_time;

  // The time when control returned to the origin thread
  // (OnGetCandidateAdapterNamesDone)
  base::TimeTicks origin_thread_end_time;

 private:
  DISALLOW_COPY_AND_ASSIGN(DhcpAdapterNamesLoggingInfo);
};

namespace {

// Maximum number of DHCP lookup tasks running concurrently. This is chosen
// based on the following UMA data:
// - When OnWaitTimer fires, ~99.8% of users have 6 or fewer network
//   adapters enabled for DHCP in total.
// - At the same measurement point, ~99.7% of users have 3 or fewer pending
//   DHCP adapter lookups.
// - There is however a very long and thin tail of users who have
//   systems reporting up to 100+ adapters (this must be some very weird
//   OS bug (?), probably the cause of http://crbug.com/240034).
//
// Th value is chosen such that DHCP lookup tasks don't prevent other tasks from
// running even on systems that report a huge number of network adapters, while
// giving a good chance of getting back results for any responsive adapters.
constexpr int kMaxConcurrentDhcpLookupTasks = 12;

// How long to wait at maximum after we get results (a PAC file or
// knowledge that no PAC file is configured) from whichever network
// adapter finishes first.
constexpr base::TimeDelta kMaxWaitAfterFirstResult =
    base::TimeDelta::FromMilliseconds(400);

// A TaskRunner that never schedules more than |kMaxConcurrentDhcpLookupTasks|
// tasks concurrently.
class TaskRunnerWithCap : public base::TaskRunner {
 public:
  TaskRunnerWithCap() = default;

  bool PostDelayedTask(const base::Location& from_here,
                       base::OnceClosure task,
                       base::TimeDelta delay) override {
    // Delayed tasks are not supported.
    DCHECK(delay.is_zero());

    // Wrap the task in a callback that runs |task|, then tries to schedule a
    // task from |pending_tasks_|.
    base::OnceClosure wrapped_task =
        base::BindOnce(&TaskRunnerWithCap::RunTaskAndSchedulePendingTask, this,
                       std::move(task));

    {
      base::AutoLock auto_lock(lock_);

      // If |kMaxConcurrentDhcpLookupTasks| tasks are scheduled, move the task
      // to |pending_tasks_|.
      DCHECK_LE(num_scheduled_tasks_, kMaxConcurrentDhcpLookupTasks);
      if (num_scheduled_tasks_ == kMaxConcurrentDhcpLookupTasks) {
        pending_tasks_.emplace(from_here, std::move(wrapped_task));
        return true;
      }

      // If less than |kMaxConcurrentDhcpLookupTasks| tasks are scheduled,
      // increment |num_scheduled_tasks_| and schedule the task.
      ++num_scheduled_tasks_;
    }

    task_runner_->PostTask(from_here, std::move(wrapped_task));
    return true;
  }

  bool RunsTasksInCurrentSequence() const override {
    return task_runner_->RunsTasksInCurrentSequence();
  }

 private:
  struct LocationAndTask {
    LocationAndTask() = default;
    LocationAndTask(const base::Location& from_here, base::OnceClosure task)
        : from_here(from_here), task(std::move(task)) {}
    base::Location from_here;
    base::OnceClosure task;
  };

  ~TaskRunnerWithCap() override = default;

  void RunTaskAndSchedulePendingTask(base::OnceClosure task) {
    // Run |task|.
    std::move(task).Run();

    // If |pending_tasks_| is non-empty, schedule a task from it. Otherwise,
    // decrement |num_scheduled_tasks_|.
    LocationAndTask task_to_schedule;

    {
      base::AutoLock auto_lock(lock_);

      DCHECK_GT(num_scheduled_tasks_, 0);
      if (pending_tasks_.empty()) {
        --num_scheduled_tasks_;
        return;
      }

      task_to_schedule = std::move(pending_tasks_.front());
      pending_tasks_.pop();
    }

    DCHECK(task_to_schedule.task);
    task_runner_->PostTask(task_to_schedule.from_here,
                           std::move(task_to_schedule.task));
  }

  const scoped_refptr<base::TaskRunner> task_runner_ =
      base::CreateTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN,
           base::TaskPriority::USER_VISIBLE});

  // Synchronizes access to members below.
  base::Lock lock_;

  // Number of tasks that are currently scheduled.
  int num_scheduled_tasks_ = 0;

  // Tasks that are waiting to be scheduled.
  base::queue<LocationAndTask> pending_tasks_;

  DISALLOW_COPY_AND_ASSIGN(TaskRunnerWithCap);
};

// Helper to set an integer value into a base::DictionaryValue. Because of
// C++'s implicit narrowing casts to |int|, this can be called with int64_t and
// ULONG too.
void SetInt(base::StringPiece key, int value, base::DictionaryValue* dict) {
  dict->SetKey(key, base::Value(value));
}

std::unique_ptr<base::Value> NetLogGetAdaptersDoneCallback(
    DhcpAdapterNamesLoggingInfo* info,
    NetLogCaptureMode /* capture_mode */) {
  std::unique_ptr<base::DictionaryValue> result =
      std::make_unique<base::DictionaryValue>();

  // Add information on each of the adapters enumerated (including those that
  // were subsequently skipped).
  base::ListValue adapters_value;
  for (IP_ADAPTER_ADDRESSES* adapter = info->adapters.get(); adapter;
       adapter = adapter->Next) {
    base::DictionaryValue adapter_value;

    adapter_value.SetKey("AdapterName", base::Value(adapter->AdapterName));
    SetInt("IfType", adapter->IfType, &adapter_value);
    SetInt("Flags", adapter->Flags, &adapter_value);
    SetInt("OperStatus", adapter->OperStatus, &adapter_value);
    SetInt("TunnelType", adapter->TunnelType, &adapter_value);

    // "skipped" means the adapter was not ultimately chosen as a candidate for
    // testing WPAD. This replicates the logic in GetAdapterNames().
    bool skipped = (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) ||
                   ((adapter->Flags & IP_ADAPTER_DHCP_ENABLED) == 0);
    adapter_value.SetKey("skipped", base::Value(skipped));

    adapters_value.GetList().push_back(std::move(adapter_value));
  }
  result->SetKey("adapters", std::move(adapters_value));

  SetInt("origin_to_worker_thread_hop_dt",
         (info->worker_thread_start_time - info->origin_thread_start_time)
             .InMilliseconds(),
         result.get());
  SetInt("worker_to_origin_thread_hop_dt",
         (info->origin_thread_end_time - info->worker_thread_end_time)
             .InMilliseconds(),
         result.get());
  SetInt("worker_dt",
         (info->worker_thread_end_time - info->worker_thread_start_time)
             .InMilliseconds(),
         result.get());

  if (info->error != ERROR_SUCCESS)
    SetInt("error", info->error, result.get());

  return result;
}

std::unique_ptr<base::Value> NetLogFetcherDoneCallback(
    int fetcher_index,
    int net_error,
    NetLogCaptureMode /* capture_mode */) {
  std::unique_ptr<base::DictionaryValue> result =
      std::make_unique<base::DictionaryValue>();

  result->SetKey("fetcher_index", base::Value(fetcher_index));
  result->SetKey("net_error", base::Value(net_error));

  return result;
}

}  // namespace

DhcpProxyScriptFetcherWin::DhcpProxyScriptFetcherWin(
    URLRequestContext* url_request_context)
    : state_(STATE_START),
      num_pending_fetchers_(0),
      destination_string_(NULL),
      url_request_context_(url_request_context),
      task_runner_(base::MakeRefCounted<TaskRunnerWithCap>()) {
  DCHECK(url_request_context_);
}

DhcpProxyScriptFetcherWin::~DhcpProxyScriptFetcherWin() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // Count as user-initiated if we are not yet in STATE_DONE.
  Cancel();
}

int DhcpProxyScriptFetcherWin::Fetch(base::string16* utf16_text,
                                     const CompletionCallback& callback,
                                     const NetLogWithSource& net_log) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (state_ != STATE_START && state_ != STATE_DONE) {
    NOTREACHED();
    return ERR_UNEXPECTED;
  }

  net_log_ = net_log;

  if (!url_request_context_)
    return ERR_CONTEXT_SHUT_DOWN;

  state_ = STATE_WAIT_ADAPTERS;
  callback_ = callback;
  destination_string_ = utf16_text;

  net_log.BeginEvent(NetLogEventType::WPAD_DHCP_WIN_FETCH);

  // TODO(eroman): This event is not ended in the case of cancellation.
  net_log.BeginEvent(NetLogEventType::WPAD_DHCP_WIN_GET_ADAPTERS);

  last_query_ = ImplCreateAdapterQuery();
  last_query_->logging_info()->origin_thread_start_time =
      base::TimeTicks::Now();

  task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::Bind(
          &DhcpProxyScriptFetcherWin::AdapterQuery::GetCandidateAdapterNames,
          last_query_.get()),
      base::Bind(&DhcpProxyScriptFetcherWin::OnGetCandidateAdapterNamesDone,
                 AsWeakPtr(), last_query_));

  return ERR_IO_PENDING;
}

void DhcpProxyScriptFetcherWin::Cancel() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  CancelImpl();
}

void DhcpProxyScriptFetcherWin::OnShutdown() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // Back up callback, if there is one, as CancelImpl() will destroy it.
  net::CompletionCallback callback = std::move(callback_);

  // Cancel current request, if there is one.
  CancelImpl();

  // Prevent future network requests.
  url_request_context_ = nullptr;

  // Invoke callback with error, if present.
  if (callback)
    callback.Run(ERR_CONTEXT_SHUT_DOWN);
}

void DhcpProxyScriptFetcherWin::CancelImpl() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (state_ != STATE_DONE) {
    callback_.Reset();
    wait_timer_.Stop();
    state_ = STATE_DONE;

    for (FetcherVector::iterator it = fetchers_.begin();
         it != fetchers_.end();
         ++it) {
      (*it)->Cancel();
    }

    fetchers_.clear();
  }
}

void DhcpProxyScriptFetcherWin::OnGetCandidateAdapterNamesDone(
    scoped_refptr<AdapterQuery> query) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // This can happen if this object is reused for multiple queries,
  // and a previous query was cancelled before it completed.
  if (query.get() != last_query_.get())
    return;
  last_query_ = NULL;

  DhcpAdapterNamesLoggingInfo* logging_info = query->logging_info();
  logging_info->origin_thread_end_time = base::TimeTicks::Now();

  net_log_.EndEvent(NetLogEventType::WPAD_DHCP_WIN_GET_ADAPTERS,
                    base::Bind(&NetLogGetAdaptersDoneCallback,
                               base::Unretained(logging_info)));

  // Enable unit tests to wait for this to happen; in production this function
  // call is a no-op.
  ImplOnGetCandidateAdapterNamesDone();

  // We may have been cancelled.
  if (state_ != STATE_WAIT_ADAPTERS)
    return;

  state_ = STATE_NO_RESULTS;

  const std::set<std::string>& adapter_names = query->adapter_names();

  if (adapter_names.empty()) {
    TransitionToDone();
    return;
  }

  for (const std::string& adapter_name : adapter_names) {
    std::unique_ptr<DhcpProxyScriptAdapterFetcher> fetcher(
        ImplCreateAdapterFetcher());
    size_t fetcher_index = fetchers_.size();
    fetcher->Fetch(adapter_name,
                   base::Bind(&DhcpProxyScriptFetcherWin::OnFetcherDone,
                              base::Unretained(this), fetcher_index));
    fetchers_.push_back(std::move(fetcher));
  }
  num_pending_fetchers_ = fetchers_.size();
}

std::string DhcpProxyScriptFetcherWin::GetFetcherName() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return "win";
}

const GURL& DhcpProxyScriptFetcherWin::GetPacURL() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK_EQ(state_, STATE_DONE);

  return pac_url_;
}

void DhcpProxyScriptFetcherWin::OnFetcherDone(size_t fetcher_index,
                                              int result) {
  DCHECK(state_ == STATE_NO_RESULTS || state_ == STATE_SOME_RESULTS);

  net_log_.AddEvent(
      NetLogEventType::WPAD_DHCP_WIN_ON_FETCHER_DONE,
      base::Bind(&NetLogFetcherDoneCallback, fetcher_index, result));

  if (--num_pending_fetchers_ == 0) {
    TransitionToDone();
    return;
  }

  // If the only pending adapters are those less preferred than one
  // with a valid PAC script, we do not need to wait any longer.
  for (FetcherVector::iterator it = fetchers_.begin();
       it != fetchers_.end();
       ++it) {
    bool did_finish = (*it)->DidFinish();
    int result = (*it)->GetResult();
    if (did_finish && result == OK) {
      TransitionToDone();
      return;
    }
    if (!did_finish || result != ERR_PAC_NOT_IN_DHCP) {
      break;
    }
  }

  // Once we have a single result, we set a maximum on how long to wait
  // for the rest of the results.
  if (state_ == STATE_NO_RESULTS) {
    state_ = STATE_SOME_RESULTS;
    net_log_.AddEvent(NetLogEventType::WPAD_DHCP_WIN_START_WAIT_TIMER);
    wait_timer_.Start(FROM_HERE,
        ImplGetMaxWait(), this, &DhcpProxyScriptFetcherWin::OnWaitTimer);
  }
}

void DhcpProxyScriptFetcherWin::OnWaitTimer() {
  DCHECK_EQ(state_, STATE_SOME_RESULTS);

  net_log_.AddEvent(NetLogEventType::WPAD_DHCP_WIN_ON_WAIT_TIMER);
  TransitionToDone();
}

void DhcpProxyScriptFetcherWin::TransitionToDone() {
  DCHECK(state_ == STATE_NO_RESULTS || state_ == STATE_SOME_RESULTS);

  int used_fetcher_index = -1;
  int result = ERR_PAC_NOT_IN_DHCP;  // Default if no fetchers.
  if (!fetchers_.empty()) {
    // Scan twice for the result; once through the whole list for success,
    // then if no success, return result for most preferred network adapter,
    // preferring "real" network errors to the ERR_PAC_NOT_IN_DHCP error.
    // Default to ERR_ABORTED if no fetcher completed.
    result = ERR_ABORTED;
    for (size_t i = 0; i < fetchers_.size(); ++i) {
      const auto& fetcher = fetchers_[i];
      if (fetcher->DidFinish() && fetcher->GetResult() == OK) {
        result = OK;
        *destination_string_ = fetcher->GetPacScript();
        pac_url_ = fetcher->GetPacURL();
        used_fetcher_index = i;
        break;
      }
    }
    if (result != OK) {
      destination_string_->clear();
      for (size_t i = 0; i < fetchers_.size(); ++i) {
        const auto& fetcher = fetchers_[i];
        if (fetcher->DidFinish()) {
          result = fetcher->GetResult();
          used_fetcher_index = i;
          if (result != ERR_PAC_NOT_IN_DHCP) {
            break;
          }
        }
      }
    }
  }

  CompletionCallback callback = callback_;
  CancelImpl();
  DCHECK_EQ(state_, STATE_DONE);
  DCHECK(fetchers_.empty());
  DCHECK(callback_.is_null());  // Invariant of data.

  net_log_.EndEvent(
      NetLogEventType::WPAD_DHCP_WIN_FETCH,
      base::Bind(&NetLogFetcherDoneCallback, used_fetcher_index, result));

  // We may be deleted re-entrantly within this outcall.
  callback.Run(result);
}

int DhcpProxyScriptFetcherWin::num_pending_fetchers() const {
  return num_pending_fetchers_;
}

URLRequestContext* DhcpProxyScriptFetcherWin::url_request_context() const {
  return url_request_context_;
}

scoped_refptr<base::TaskRunner> DhcpProxyScriptFetcherWin::GetTaskRunner() {
  return task_runner_;
}

DhcpProxyScriptAdapterFetcher*
    DhcpProxyScriptFetcherWin::ImplCreateAdapterFetcher() {
  return new DhcpProxyScriptAdapterFetcher(url_request_context_, task_runner_);
}

DhcpProxyScriptFetcherWin::AdapterQuery*
    DhcpProxyScriptFetcherWin::ImplCreateAdapterQuery() {
  return new AdapterQuery();
}

base::TimeDelta DhcpProxyScriptFetcherWin::ImplGetMaxWait() {
  return kMaxWaitAfterFirstResult;
}

bool DhcpProxyScriptFetcherWin::GetCandidateAdapterNames(
    std::set<std::string>* adapter_names,
    DhcpAdapterNamesLoggingInfo* info) {
  DCHECK(adapter_names);
  adapter_names->clear();

  // The GetAdaptersAddresses MSDN page recommends using a size of 15000 to
  // avoid reallocation.
  ULONG adapters_size = 15000;
  std::unique_ptr<IP_ADAPTER_ADDRESSES, base::FreeDeleter> adapters;
  ULONG error = ERROR_SUCCESS;
  int num_tries = 0;

  do {
    adapters.reset(static_cast<IP_ADAPTER_ADDRESSES*>(malloc(adapters_size)));
    // Return only unicast addresses, and skip information we do not need.
    base::ScopedBlockingCall scoped_blocking_call(
        base::BlockingType::MAY_BLOCK);
    error = GetAdaptersAddresses(AF_UNSPEC,
                                 GAA_FLAG_SKIP_ANYCAST |
                                 GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER |
                                 GAA_FLAG_SKIP_FRIENDLY_NAME,
                                 NULL,
                                 adapters.get(),
                                 &adapters_size);
    ++num_tries;
  } while (error == ERROR_BUFFER_OVERFLOW && num_tries <= 3);

  if (info)
    info->error = error;

  if (error == ERROR_NO_DATA) {
    // There are no adapters that we care about.
    return true;
  }

  if (error != ERROR_SUCCESS) {
    LOG(WARNING) << "Unexpected error retrieving WPAD configuration from DHCP.";
    return false;
  }

  IP_ADAPTER_ADDRESSES* adapter = NULL;
  for (adapter = adapters.get(); adapter; adapter = adapter->Next) {
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;
    if ((adapter->Flags & IP_ADAPTER_DHCP_ENABLED) == 0)
      continue;

    DCHECK(adapter->AdapterName);
    adapter_names->insert(adapter->AdapterName);
  }

  // Transfer the buffer containing the adapters, so it can be used later for
  // emitting NetLog parameters from the origin thread.
  if (info)
    info->adapters = std::move(adapters);
  return true;
}

DhcpProxyScriptFetcherWin::AdapterQuery::AdapterQuery()
    : logging_info_(new DhcpAdapterNamesLoggingInfo()) {}

void DhcpProxyScriptFetcherWin::AdapterQuery::GetCandidateAdapterNames() {
  logging_info_->error = ERROR_NO_DATA;
  logging_info_->adapters.reset();
  logging_info_->worker_thread_start_time = base::TimeTicks::Now();

  ImplGetCandidateAdapterNames(&adapter_names_, logging_info_.get());

  logging_info_->worker_thread_end_time = base::TimeTicks::Now();
}

const std::set<std::string>&
    DhcpProxyScriptFetcherWin::AdapterQuery::adapter_names() const {
  return adapter_names_;
}

bool DhcpProxyScriptFetcherWin::AdapterQuery::ImplGetCandidateAdapterNames(
    std::set<std::string>* adapter_names,
    DhcpAdapterNamesLoggingInfo* info) {
  return DhcpProxyScriptFetcherWin::GetCandidateAdapterNames(adapter_names,
                                                             info);
}

DhcpProxyScriptFetcherWin::AdapterQuery::~AdapterQuery() {
}

}  // namespace net
