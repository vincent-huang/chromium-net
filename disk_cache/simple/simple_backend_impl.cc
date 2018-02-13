// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/disk_cache/simple/simple_backend_impl.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>

#if defined(OS_POSIX)
#include <sys/resource.h>
#endif

#include "base/bind.h"
#include "base/callback.h"
#include "base/files/file_util.h"
#include "base/lazy_instance.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/field_trial_params.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/single_thread_task_runner.h"
#include "base/sys_info.h"
#include "base/task_runner_util.h"
#include "base/task_scheduler/post_task.h"
#include "base/task_scheduler/task_scheduler.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "base/trace_event/memory_usage_estimator.h"
#include "base/trace_event/process_memory_dump.h"
#include "build/build_config.h"
#include "net/base/net_errors.h"
#include "net/disk_cache/backend_cleanup_tracker.h"
#include "net/disk_cache/cache_util.h"
#include "net/disk_cache/simple/simple_entry_format.h"
#include "net/disk_cache/simple/simple_entry_impl.h"
#include "net/disk_cache/simple/simple_experiment.h"
#include "net/disk_cache/simple/simple_file_tracker.h"
#include "net/disk_cache/simple/simple_histogram_macros.h"
#include "net/disk_cache/simple/simple_index.h"
#include "net/disk_cache/simple/simple_index_file.h"
#include "net/disk_cache/simple/simple_synchronous_entry.h"
#include "net/disk_cache/simple/simple_util.h"
#include "net/disk_cache/simple/simple_version_upgrade.h"

using base::Callback;
using base::Closure;
using base::FilePath;
using base::Time;
using base::DirectoryExists;
using base::CreateDirectory;

namespace disk_cache {

namespace {

const base::FeatureParam<base::TaskPriority>::Option prio_modes[] = {
    {base::TaskPriority::USER_BLOCKING, "default"},
    {base::TaskPriority::USER_VISIBLE, "user_visible"}};
const base::Feature kSimpleCachePriorityExperiment = {
    "SimpleCachePriorityExperiment", base::FEATURE_DISABLED_BY_DEFAULT};
const base::FeatureParam<base::TaskPriority> priority_mode{
    &kSimpleCachePriorityExperiment, "mode", base::TaskPriority::USER_BLOCKING,
    &prio_modes};

base::TaskPriority PriorityToUse() {
  return priority_mode.Get();
}

// Maximum fraction of the cache that one entry can consume.
const int kMaxFileRatio = 8;

scoped_refptr<base::SequencedTaskRunner> FallbackToInternalIfNull(
    const scoped_refptr<base::SequencedTaskRunner>& cache_runner) {
  if (cache_runner)
    return cache_runner;
  return base::CreateSequencedTaskRunnerWithTraits(
      {base::MayBlock(), PriorityToUse(),
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
}

bool g_fd_limit_histogram_has_been_populated = false;

void MaybeHistogramFdLimit() {
  if (g_fd_limit_histogram_has_been_populated)
    return;

  // Used in histograms; add new entries at end.
  enum FdLimitStatus {
    FD_LIMIT_STATUS_UNSUPPORTED = 0,
    FD_LIMIT_STATUS_FAILED      = 1,
    FD_LIMIT_STATUS_SUCCEEDED   = 2,
    FD_LIMIT_STATUS_MAX         = 3
  };
  FdLimitStatus fd_limit_status = FD_LIMIT_STATUS_UNSUPPORTED;
  int soft_fd_limit = 0;
  int hard_fd_limit = 0;

#if defined(OS_POSIX) && !defined(OS_FUCHSIA)
  struct rlimit nofile;
  if (!getrlimit(RLIMIT_NOFILE, &nofile)) {
    soft_fd_limit = nofile.rlim_cur;
    hard_fd_limit = nofile.rlim_max;
    fd_limit_status = FD_LIMIT_STATUS_SUCCEEDED;
  } else {
    fd_limit_status = FD_LIMIT_STATUS_FAILED;
  }
#endif

  UMA_HISTOGRAM_ENUMERATION("SimpleCache.FileDescriptorLimitStatus",
                            fd_limit_status, FD_LIMIT_STATUS_MAX);
  if (fd_limit_status == FD_LIMIT_STATUS_SUCCEEDED) {
    base::UmaHistogramSparse("SimpleCache.FileDescriptorLimitSoft",
                             soft_fd_limit);
    base::UmaHistogramSparse("SimpleCache.FileDescriptorLimitHard",
                             hard_fd_limit);
  }

  g_fd_limit_histogram_has_been_populated = true;
}

// Global context of all the files we have open --- this permits some to be
// closed on demand if too many FDs are being used, to avoid running out.
base::LazyInstance<SimpleFileTracker>::Leaky g_simple_file_tracker =
    LAZY_INSTANCE_INITIALIZER;

// Detects if the files in the cache directory match the current disk cache
// backend type and version. If the directory contains no cache, occupies it
// with the fresh structure.
bool FileStructureConsistent(const base::FilePath& path,
                             const SimpleExperiment& experiment) {
  if (!base::PathExists(path) && !base::CreateDirectory(path)) {
    LOG(ERROR) << "Failed to create directory: " << path.LossyDisplayName();
    return false;
  }
  return disk_cache::UpgradeSimpleCacheOnDisk(path, experiment);
}

// A context used by a BarrierCompletionCallback to track state.
struct BarrierContext {
  explicit BarrierContext(int expected)
      : expected(expected), count(0), had_error(false) {}

  const int expected;
  int count;
  bool had_error;
};

void BarrierCompletionCallbackImpl(
    BarrierContext* context,
    const net::CompletionCallback& final_callback,
    int result) {
  DCHECK_GT(context->expected, context->count);
  if (context->had_error)
    return;
  if (result != net::OK) {
    context->had_error = true;
    final_callback.Run(result);
    return;
  }
  ++context->count;
  if (context->count == context->expected)
    final_callback.Run(net::OK);
}

// A barrier completion callback is a net::CompletionCallback that waits for
// |count| successful results before invoking |final_callback|. In the case of
// an error, the first error is passed to |final_callback| and all others
// are ignored.
net::CompletionCallback MakeBarrierCompletionCallback(
    int count,
    const net::CompletionCallback& final_callback) {
  BarrierContext* context = new BarrierContext(count);
  return base::Bind(&BarrierCompletionCallbackImpl,
                    base::Owned(context), final_callback);
}

// A short bindable thunk that ensures a completion callback is always called
// after running an operation asynchronously.
void RunOperationAndCallback(
    const Callback<int(const net::CompletionCallback&)>& operation,
    const net::CompletionCallback& operation_callback) {
  const int operation_result = operation.Run(operation_callback);
  if (operation_result != net::ERR_IO_PENDING)
    operation_callback.Run(operation_result);
}

void RecordIndexLoad(net::CacheType cache_type,
                     base::TimeTicks constructed_since,
                     int result) {
  const base::TimeDelta creation_to_index = base::TimeTicks::Now() -
                                            constructed_since;
  if (result == net::OK) {
    SIMPLE_CACHE_UMA(TIMES, "CreationToIndex", cache_type, creation_to_index);
  } else {
    SIMPLE_CACHE_UMA(TIMES,
                     "CreationToIndexFail", cache_type, creation_to_index);
  }
}

}  // namespace

// Static function which is called by base::trace_event::EstimateMemoryUsage()
// to estimate the memory of SimpleEntryImpl* type.
// This needs to be in disk_cache namespace.
size_t EstimateMemoryUsage(const SimpleEntryImpl* const& entry_impl) {
  return sizeof(SimpleEntryImpl) + entry_impl->EstimateMemoryUsage();
}

class SimpleBackendImpl::ActiveEntryProxy
    : public SimpleEntryImpl::ActiveEntryProxy {
 public:
  ~ActiveEntryProxy() override {
    if (backend_) {
      DCHECK_EQ(1U, backend_->active_entries_.count(entry_hash_));
      backend_->active_entries_.erase(entry_hash_);
    }
  }

  static std::unique_ptr<SimpleEntryImpl::ActiveEntryProxy> Create(
      int64_t entry_hash,
      SimpleBackendImpl* backend) {
    std::unique_ptr<SimpleEntryImpl::ActiveEntryProxy> proxy(
        new ActiveEntryProxy(entry_hash, backend));
    return proxy;
  }

 private:
  ActiveEntryProxy(uint64_t entry_hash, SimpleBackendImpl* backend)
      : entry_hash_(entry_hash), backend_(backend->AsWeakPtr()) {}

  uint64_t entry_hash_;
  base::WeakPtr<SimpleBackendImpl> backend_;
};

SimpleBackendImpl::SimpleBackendImpl(
    const FilePath& path,
    scoped_refptr<BackendCleanupTracker> cleanup_tracker,
    SimpleFileTracker* file_tracker,
    int max_bytes,
    net::CacheType cache_type,
    const scoped_refptr<base::SequencedTaskRunner>& cache_runner,
    net::NetLog* net_log)
    : cleanup_tracker_(std::move(cleanup_tracker)),
      file_tracker_(file_tracker ? file_tracker
                                 : g_simple_file_tracker.Pointer()),
      path_(path),
      cache_type_(cache_type),
      cache_runner_(FallbackToInternalIfNull(cache_runner)),
      orig_max_size_(max_bytes),
      entry_operations_mode_(cache_type == net::DISK_CACHE
                                 ? SimpleEntryImpl::OPTIMISTIC_OPERATIONS
                                 : SimpleEntryImpl::NON_OPTIMISTIC_OPERATIONS),
      net_log_(net_log) {
  // Treat negative passed-in sizes same as SetMaxSize would here and in other
  // backends, as default (if first call).
  if (orig_max_size_ < 0)
    orig_max_size_ = 0;
  MaybeHistogramFdLimit();
}

SimpleBackendImpl::~SimpleBackendImpl() {
  index_->WriteToDisk(SimpleIndex::INDEX_WRITE_REASON_SHUTDOWN);
}

int SimpleBackendImpl::Init(const CompletionCallback& completion_callback) {
  worker_pool_ = base::TaskScheduler::GetInstance()->CreateTaskRunnerWithTraits(
      {base::MayBlock(), base::WithBaseSyncPrimitives(), PriorityToUse(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN});

  index_ = std::make_unique<SimpleIndex>(
      base::ThreadTaskRunnerHandle::Get(), cleanup_tracker_.get(), this,
      cache_type_,
      std::make_unique<SimpleIndexFile>(cache_runner_, worker_pool_.get(),
                                        cache_type_, path_));
  index_->ExecuteWhenReady(
      base::Bind(&RecordIndexLoad, cache_type_, base::TimeTicks::Now()));

  PostTaskAndReplyWithResult(
      cache_runner_.get(), FROM_HERE,
      base::Bind(&SimpleBackendImpl::InitCacheStructureOnDisk, path_,
                 orig_max_size_, GetSimpleExperiment(cache_type_)),
      base::Bind(&SimpleBackendImpl::InitializeIndex, AsWeakPtr(),
                 completion_callback));
  return net::ERR_IO_PENDING;
}

bool SimpleBackendImpl::SetMaxSize(int max_bytes) {
  if (max_bytes < 0)
    return false;
  orig_max_size_ = max_bytes;
  index_->SetMaxSize(max_bytes);
  return true;
}

int SimpleBackendImpl::GetMaxFileSize() const {
  return static_cast<int>(index_->max_size() / kMaxFileRatio);
}

void SimpleBackendImpl::OnDoomStart(uint64_t entry_hash) {
  DCHECK_EQ(0u, entries_pending_doom_.count(entry_hash));
  entries_pending_doom_.insert(
      std::make_pair(entry_hash, std::vector<PostDoomWaiter>()));
}

void SimpleBackendImpl::OnDoomComplete(uint64_t entry_hash) {
  DCHECK_EQ(1u, entries_pending_doom_.count(entry_hash));
  std::unordered_map<uint64_t, std::vector<PostDoomWaiter>>::iterator it =
      entries_pending_doom_.find(entry_hash);
  std::vector<PostDoomWaiter> to_handle_waiters;
  to_handle_waiters.swap(it->second);
  entries_pending_doom_.erase(it);

  SIMPLE_CACHE_UMA(COUNTS_1000, "NumOpsBlockedByPendingDoom", cache_type_,
                   to_handle_waiters.size());

  for (const PostDoomWaiter& post_doom : to_handle_waiters) {
    SIMPLE_CACHE_UMA(TIMES, "QueueLatency.PendingDoom", cache_type_,
                     (base::TimeTicks::Now() - post_doom.time_queued));
    post_doom.run_post_doom.Run();
  }
}

void SimpleBackendImpl::DoomEntries(std::vector<uint64_t>* entry_hashes,
                                    const net::CompletionCallback& callback) {
  std::unique_ptr<std::vector<uint64_t>> mass_doom_entry_hashes(
      new std::vector<uint64_t>());
  mass_doom_entry_hashes->swap(*entry_hashes);

  std::vector<uint64_t> to_doom_individually_hashes;

  // For each of the entry hashes, there are two cases:
  // 1. There are corresponding entries in active set, pending doom, or both
  //    sets, and so the hash should be doomed individually to avoid flakes.
  // 2. The hash is not in active use at all, so we can call
  //    SimpleSynchronousEntry::DeleteEntrySetFiles and delete the files en
  //    masse.
  for (int i = mass_doom_entry_hashes->size() - 1; i >= 0; --i) {
    const uint64_t entry_hash = (*mass_doom_entry_hashes)[i];
    if (!active_entries_.count(entry_hash) &&
        !entries_pending_doom_.count(entry_hash)) {
      continue;
    }

    to_doom_individually_hashes.push_back(entry_hash);

    (*mass_doom_entry_hashes)[i] = mass_doom_entry_hashes->back();
    mass_doom_entry_hashes->resize(mass_doom_entry_hashes->size() - 1);
  }

  net::CompletionCallback barrier_callback =
      MakeBarrierCompletionCallback(to_doom_individually_hashes.size() + 1,
                                    callback);
  for (std::vector<uint64_t>::const_iterator
           it = to_doom_individually_hashes.begin(),
           end = to_doom_individually_hashes.end();
       it != end; ++it) {
    const int doom_result = DoomEntryFromHash(*it, barrier_callback);
    DCHECK_EQ(net::ERR_IO_PENDING, doom_result);
    index_->Remove(*it);
  }

  for (std::vector<uint64_t>::const_iterator
           it = mass_doom_entry_hashes->begin(),
           end = mass_doom_entry_hashes->end();
       it != end; ++it) {
    index_->Remove(*it);
    OnDoomStart(*it);
  }

  // Taking this pointer here avoids undefined behaviour from calling
  // base::Passed before mass_doom_entry_hashes.get().
  std::vector<uint64_t>* mass_doom_entry_hashes_ptr =
      mass_doom_entry_hashes.get();
  PostTaskAndReplyWithResult(
      worker_pool_.get(), FROM_HERE,
      base::Bind(&SimpleSynchronousEntry::DeleteEntrySetFiles,
                 mass_doom_entry_hashes_ptr, path_),
      base::Bind(&SimpleBackendImpl::DoomEntriesComplete, AsWeakPtr(),
                 base::Passed(&mass_doom_entry_hashes), barrier_callback));
}

net::CacheType SimpleBackendImpl::GetCacheType() const {
  return net::DISK_CACHE;
}

int32_t SimpleBackendImpl::GetEntryCount() const {
  // TODO(pasko): Use directory file count when index is not ready.
  return index_->GetEntryCount();
}

int SimpleBackendImpl::OpenEntry(const std::string& key,
                                 Entry** entry,
                                 const CompletionCallback& callback) {
  const uint64_t entry_hash = simple_util::GetEntryHashKey(key);

  std::vector<PostDoomWaiter>* post_doom = nullptr;
  scoped_refptr<SimpleEntryImpl> simple_entry =
      CreateOrFindActiveOrDoomedEntry(entry_hash, key, &post_doom);
  if (!simple_entry) {
    if (post_doom->empty() &&
        entry_operations_mode_ == SimpleEntryImpl::OPTIMISTIC_OPERATIONS) {
      // The entry is doomed, and no other backend operations are queued for the
      // entry, thus the open must fail and it's safe to return synchronously.
      net::NetLogWithSource log_for_entry(net::NetLogWithSource::Make(
          net_log_, net::NetLogSourceType::DISK_CACHE_ENTRY));
      log_for_entry.AddEvent(
          net::NetLogEventType::SIMPLE_CACHE_ENTRY_OPEN_CALL);
      log_for_entry.AddEventWithNetErrorCode(
          net::NetLogEventType::SIMPLE_CACHE_ENTRY_OPEN_END, net::ERR_FAILED);
      return net::ERR_FAILED;
    }

    Callback<int(const net::CompletionCallback&)> operation =
        base::Bind(&SimpleBackendImpl::OpenEntry,
                   base::Unretained(this), key, entry);
    post_doom->emplace_back(
        base::Bind(&RunOperationAndCallback, operation, callback));
    return net::ERR_IO_PENDING;
  }
  return simple_entry->OpenEntry(entry, callback);
}

int SimpleBackendImpl::CreateEntry(const std::string& key,
                                   Entry** entry,
                                   const CompletionCallback& callback) {
  DCHECK_LT(0u, key.size());
  const uint64_t entry_hash = simple_util::GetEntryHashKey(key);

  std::vector<PostDoomWaiter>* post_doom = nullptr;
  scoped_refptr<SimpleEntryImpl> simple_entry =
      CreateOrFindActiveOrDoomedEntry(entry_hash, key, &post_doom);

  if (!simple_entry) {
    // We would like to optimistically have create go ahead, for benefit of
    // HTTP cache use. This can only be sanely done if we are the only op
    // serialized after doom's completion.
    if (post_doom->empty() &&
        entry_operations_mode_ == SimpleEntryImpl::OPTIMISTIC_OPERATIONS) {
      simple_entry = new SimpleEntryImpl(
          cache_type_, path_, cleanup_tracker_.get(), entry_hash,
          entry_operations_mode_, this, file_tracker_, net_log_);
      simple_entry->SetKey(key);
      simple_entry->SetActiveEntryProxy(
          ActiveEntryProxy::Create(entry_hash, this));
      simple_entry->SetCreatePendingDoom();
      std::pair<EntryMap::iterator, bool> insert_result =
          active_entries_.insert(
              EntryMap::value_type(entry_hash, simple_entry.get()));
      post_doom->emplace_back(base::Bind(
          &SimpleEntryImpl::NotifyDoomBeforeCreateComplete, simple_entry));
      DCHECK(insert_result.second);
    } else {
      Callback<int(const net::CompletionCallback&)> operation = base::Bind(
          &SimpleBackendImpl::CreateEntry, base::Unretained(this), key, entry);
      post_doom->emplace_back(
          base::Bind(&RunOperationAndCallback, operation, callback));
      return net::ERR_IO_PENDING;
    }
  }

  return simple_entry->CreateEntry(entry, callback);
}

int SimpleBackendImpl::DoomEntry(const std::string& key,
                                 const net::CompletionCallback& callback) {
  const uint64_t entry_hash = simple_util::GetEntryHashKey(key);

  std::vector<PostDoomWaiter>* post_doom = nullptr;
  scoped_refptr<SimpleEntryImpl> simple_entry =
      CreateOrFindActiveOrDoomedEntry(entry_hash, key, &post_doom);
  if (!simple_entry) {
    // At first glance, it appears exceedingly silly to queue up a doom
    // when we get here because the files corresponding to our key are being
    // deleted... but it's possible that one of the things in post_doom is a
    // create for our key, in which case we still have work to do.
    Callback<int(const net::CompletionCallback&)> operation =
        base::Bind(&SimpleBackendImpl::DoomEntry, base::Unretained(this), key);
    post_doom->emplace_back(
        base::Bind(&RunOperationAndCallback, operation, callback));
    return net::ERR_IO_PENDING;
  }

  return simple_entry->DoomEntry(callback);
}

int SimpleBackendImpl::DoomAllEntries(const CompletionCallback& callback) {
  return DoomEntriesBetween(Time(), Time(), callback);
}

int SimpleBackendImpl::DoomEntriesBetween(
    const Time initial_time,
    const Time end_time,
    const CompletionCallback& callback) {
  return index_->ExecuteWhenReady(
      base::Bind(&SimpleBackendImpl::IndexReadyForDoom, AsWeakPtr(),
                 initial_time, end_time, callback));
}

int SimpleBackendImpl::DoomEntriesSince(
    const Time initial_time,
    const CompletionCallback& callback) {
  return DoomEntriesBetween(initial_time, Time(), callback);
}

int SimpleBackendImpl::CalculateSizeOfAllEntries(
    const CompletionCallback& callback) {
  return index_->ExecuteWhenReady(base::Bind(
      &SimpleBackendImpl::IndexReadyForSizeCalculation, AsWeakPtr(), callback));
}

int SimpleBackendImpl::CalculateSizeOfEntriesBetween(
    base::Time initial_time,
    base::Time end_time,
    const CompletionCallback& callback) {
  return index_->ExecuteWhenReady(
      base::Bind(&SimpleBackendImpl::IndexReadyForSizeBetweenCalculation,
                 AsWeakPtr(), initial_time, end_time, callback));
}

class SimpleBackendImpl::SimpleIterator final : public Iterator {
 public:
  explicit SimpleIterator(base::WeakPtr<SimpleBackendImpl> backend)
      : backend_(backend),
        weak_factory_(this) {
  }

  // From Backend::Iterator:
  int OpenNextEntry(Entry** next_entry,
                    const CompletionCallback& callback) override {
    CompletionCallback open_next_entry_impl =
        base::Bind(&SimpleIterator::OpenNextEntryImpl,
                   weak_factory_.GetWeakPtr(), next_entry, callback);
    return backend_->index_->ExecuteWhenReady(open_next_entry_impl);
  }

  void OpenNextEntryImpl(Entry** next_entry,
                         const CompletionCallback& callback,
                         int index_initialization_error_code) {
    if (!backend_) {
      callback.Run(net::ERR_FAILED);
      return;
    }
    if (index_initialization_error_code != net::OK) {
      callback.Run(index_initialization_error_code);
      return;
    }
    if (!hashes_to_enumerate_)
      hashes_to_enumerate_ = backend_->index()->GetAllHashes();

    while (!hashes_to_enumerate_->empty()) {
      uint64_t entry_hash = hashes_to_enumerate_->back();
      hashes_to_enumerate_->pop_back();
      if (backend_->index()->Has(entry_hash)) {
        *next_entry = NULL;
        CompletionCallback continue_iteration = base::Bind(
            &SimpleIterator::CheckIterationReturnValue,
            weak_factory_.GetWeakPtr(),
            next_entry,
            callback);
        int error_code_open = backend_->OpenEntryFromHash(entry_hash,
                                                          next_entry,
                                                          continue_iteration);
        if (error_code_open == net::ERR_IO_PENDING)
          return;
        if (error_code_open != net::ERR_FAILED) {
          callback.Run(error_code_open);
          return;
        }
      }
    }
    callback.Run(net::ERR_FAILED);
  }

  void CheckIterationReturnValue(Entry** entry,
                                 const CompletionCallback& callback,
                                 int error_code) {
    if (error_code == net::ERR_FAILED) {
      OpenNextEntry(entry, callback);
      return;
    }
    callback.Run(error_code);
  }

 private:
  base::WeakPtr<SimpleBackendImpl> backend_;
  std::unique_ptr<std::vector<uint64_t>> hashes_to_enumerate_;
  base::WeakPtrFactory<SimpleIterator> weak_factory_;
};

std::unique_ptr<Backend::Iterator> SimpleBackendImpl::CreateIterator() {
  return std::unique_ptr<Iterator>(new SimpleIterator(AsWeakPtr()));
}

void SimpleBackendImpl::GetStats(base::StringPairs* stats) {
  std::pair<std::string, std::string> item;
  item.first = "Cache type";
  item.second = "Simple Cache";
  stats->push_back(item);
}

void SimpleBackendImpl::OnExternalCacheHit(const std::string& key) {
  index_->UseIfExists(simple_util::GetEntryHashKey(key));
}

size_t SimpleBackendImpl::DumpMemoryStats(
    base::trace_event::ProcessMemoryDump* pmd,
    const std::string& parent_absolute_name) const {
  base::trace_event::MemoryAllocatorDump* dump =
      pmd->CreateAllocatorDump(parent_absolute_name + "/simple_backend");

  size_t size = base::trace_event::EstimateMemoryUsage(index_) +
                base::trace_event::EstimateMemoryUsage(active_entries_);
  // TODO(xunjieli): crbug.com/669108. Track |entries_pending_doom_| once
  // base::Closure is suppported in memory_usage_estimator.h.
  dump->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
                  base::trace_event::MemoryAllocatorDump::kUnitsBytes, size);
  return size;
}

uint8_t SimpleBackendImpl::GetEntryInMemoryData(const std::string& key) {
  const uint64_t entry_hash = simple_util::GetEntryHashKey(key);
  return index_->GetEntryInMemoryData(entry_hash);
}

void SimpleBackendImpl::SetEntryInMemoryData(const std::string& key,
                                             uint8_t data) {
  const uint64_t entry_hash = simple_util::GetEntryHashKey(key);
  index_->SetEntryInMemoryData(entry_hash, data);
}

SimpleBackendImpl::PostDoomWaiter::PostDoomWaiter() {}

SimpleBackendImpl::PostDoomWaiter::PostDoomWaiter(
    const base::Closure& to_run_post_doom)
    : time_queued(base::TimeTicks::Now()), run_post_doom(to_run_post_doom) {}

SimpleBackendImpl::PostDoomWaiter::PostDoomWaiter(const PostDoomWaiter& other)
    : time_queued(other.time_queued), run_post_doom(other.run_post_doom) {}

SimpleBackendImpl::PostDoomWaiter::~PostDoomWaiter() {}

void SimpleBackendImpl::InitializeIndex(const CompletionCallback& callback,
                                        const DiskStatResult& result) {
  if (result.net_error == net::OK) {
    index_->SetMaxSize(result.max_size);
    index_->Initialize(result.cache_dir_mtime);
  }
  callback.Run(result.net_error);
}

void SimpleBackendImpl::IndexReadyForDoom(Time initial_time,
                                          Time end_time,
                                          const CompletionCallback& callback,
                                          int result) {
  if (result != net::OK) {
    callback.Run(result);
    return;
  }
  std::unique_ptr<std::vector<uint64_t>> removed_key_hashes(
      index_->GetEntriesBetween(initial_time, end_time).release());
  DoomEntries(removed_key_hashes.get(), callback);
}

void SimpleBackendImpl::IndexReadyForSizeCalculation(
    const CompletionCallback& callback,
    int result) {
  if (result == net::OK)
    result = static_cast<int>(index_->GetCacheSize());
  callback.Run(result);
}

void SimpleBackendImpl::IndexReadyForSizeBetweenCalculation(
    base::Time initial_time,
    base::Time end_time,
    const CompletionCallback& callback,
    int result) {
  if (result == net::OK) {
    result =
        static_cast<int>(index_->GetCacheSizeBetween(initial_time, end_time));
  }
  callback.Run(result);
}

// static
SimpleBackendImpl::DiskStatResult SimpleBackendImpl::InitCacheStructureOnDisk(
    const base::FilePath& path,
    uint64_t suggested_max_size,
    const SimpleExperiment& experiment) {
  DiskStatResult result;
  result.max_size = suggested_max_size;
  result.net_error = net::OK;
  if (!FileStructureConsistent(path, experiment)) {
    LOG(ERROR) << "Simple Cache Backend: wrong file structure on disk: "
               << path.LossyDisplayName();
    result.net_error = net::ERR_FAILED;
  } else {
    bool mtime_result =
        disk_cache::simple_util::GetMTime(path, &result.cache_dir_mtime);
    DCHECK(mtime_result);
    if (!result.max_size) {
      int64_t available = base::SysInfo::AmountOfFreeDiskSpace(path);
      result.max_size = disk_cache::PreferredCacheSize(available);

      if (experiment.type == SimpleExperimentType::SIZE) {
        int64_t adjusted_max_size = (result.max_size * experiment.param) / 100;
        adjusted_max_size =
            std::min(static_cast<int64_t>(std::numeric_limits<int32_t>::max()),
                     adjusted_max_size);
        result.max_size = adjusted_max_size;
      }
    }
    DCHECK(result.max_size);
  }
  return result;
}

scoped_refptr<SimpleEntryImpl>
SimpleBackendImpl::CreateOrFindActiveOrDoomedEntry(
    const uint64_t entry_hash,
    const std::string& key,
    std::vector<PostDoomWaiter>** post_doom) {
  DCHECK_EQ(entry_hash, simple_util::GetEntryHashKey(key));

  // If there is a doom pending, we would want to serialize after it.
  std::unordered_map<uint64_t, std::vector<PostDoomWaiter>>::iterator doom_it =
      entries_pending_doom_.find(entry_hash);
  if (doom_it != entries_pending_doom_.end()) {
    *post_doom = &doom_it->second;
    return nullptr;
  }

  std::pair<EntryMap::iterator, bool> insert_result =
      active_entries_.insert(EntryMap::value_type(entry_hash, NULL));
  EntryMap::iterator& it = insert_result.first;
  const bool did_insert = insert_result.second;
  if (did_insert) {
    SimpleEntryImpl* entry = it->second = new SimpleEntryImpl(
        cache_type_, path_, cleanup_tracker_.get(), entry_hash,
        entry_operations_mode_, this, file_tracker_, net_log_);
    entry->SetKey(key);
    entry->SetActiveEntryProxy(ActiveEntryProxy::Create(entry_hash, this));
  }
  DCHECK(it->second);
  // It's possible, but unlikely, that we have an entry hash collision with a
  // currently active entry.
  if (key != it->second->key()) {
    it->second->Doom();
    DCHECK_EQ(0U, active_entries_.count(entry_hash));
    DCHECK_EQ(1U, entries_pending_doom_.count(entry_hash));
    // Re-run ourselves to handle the now-pending doom.
    return CreateOrFindActiveOrDoomedEntry(entry_hash, key, post_doom);
  }
  return base::WrapRefCounted(it->second);
}

int SimpleBackendImpl::OpenEntryFromHash(uint64_t entry_hash,
                                         Entry** entry,
                                         const CompletionCallback& callback) {
  std::unordered_map<uint64_t, std::vector<PostDoomWaiter>>::iterator it =
      entries_pending_doom_.find(entry_hash);
  if (it != entries_pending_doom_.end()) {
    Callback<int(const net::CompletionCallback&)> operation =
        base::Bind(&SimpleBackendImpl::OpenEntryFromHash,
                   base::Unretained(this), entry_hash, entry);
    it->second.emplace_back(
        base::Bind(&RunOperationAndCallback, operation, callback));
    return net::ERR_IO_PENDING;
  }

  EntryMap::iterator has_active = active_entries_.find(entry_hash);
  if (has_active != active_entries_.end()) {
    return OpenEntry(has_active->second->key(), entry, callback);
  }

  scoped_refptr<SimpleEntryImpl> simple_entry = new SimpleEntryImpl(
      cache_type_, path_, cleanup_tracker_.get(), entry_hash,
      entry_operations_mode_, this, file_tracker_, net_log_);
  CompletionCallback backend_callback =
      base::Bind(&SimpleBackendImpl::OnEntryOpenedFromHash,
                 AsWeakPtr(), entry_hash, entry, simple_entry, callback);
  return simple_entry->OpenEntry(entry, backend_callback);
}

int SimpleBackendImpl::DoomEntryFromHash(uint64_t entry_hash,
                                         const CompletionCallback& callback) {
  Entry** entry = new Entry*();
  std::unique_ptr<Entry*> scoped_entry(entry);

  std::unordered_map<uint64_t, std::vector<PostDoomWaiter>>::iterator
      pending_it = entries_pending_doom_.find(entry_hash);
  if (pending_it != entries_pending_doom_.end()) {
    Callback<int(const net::CompletionCallback&)> operation =
        base::Bind(&SimpleBackendImpl::DoomEntryFromHash,
                   base::Unretained(this), entry_hash);
    pending_it->second.emplace_back(
        base::Bind(&RunOperationAndCallback, operation, callback));
    return net::ERR_IO_PENDING;
  }

  EntryMap::iterator active_it = active_entries_.find(entry_hash);
  if (active_it != active_entries_.end())
    return active_it->second->DoomEntry(callback);

  // There's no pending dooms, nor any open entry. We can make a trivial
  // call to DoomEntries() to delete this entry.
  std::vector<uint64_t> entry_hash_vector;
  entry_hash_vector.push_back(entry_hash);
  DoomEntries(&entry_hash_vector, callback);
  return net::ERR_IO_PENDING;
}

void SimpleBackendImpl::OnEntryOpenedFromHash(
    uint64_t hash,
    Entry** entry,
    const scoped_refptr<SimpleEntryImpl>& simple_entry,
    const CompletionCallback& callback,
    int error_code) {
  if (error_code != net::OK) {
    callback.Run(error_code);
    return;
  }
  DCHECK(*entry);
  std::pair<EntryMap::iterator, bool> insert_result =
      active_entries_.insert(EntryMap::value_type(hash, simple_entry.get()));
  EntryMap::iterator& it = insert_result.first;
  const bool did_insert = insert_result.second;
  if (did_insert) {
    // There was no active entry corresponding to this hash. We've already put
    // the entry opened from hash in the |active_entries_|. We now provide the
    // proxy object to the entry.
    it->second->SetActiveEntryProxy(ActiveEntryProxy::Create(hash, this));
    callback.Run(net::OK);
  } else {
    // The entry was made active while we waiting for the open from hash to
    // finish. The entry created from hash needs to be closed, and the one
    // in |active_entries_| can be returned to the caller.
    simple_entry->Close();
    it->second->OpenEntry(entry, callback);
  }
}

void SimpleBackendImpl::DoomEntriesComplete(
    std::unique_ptr<std::vector<uint64_t>> entry_hashes,
    const net::CompletionCallback& callback,
    int result) {
  for (const uint64_t& entry_hash : *entry_hashes)
    OnDoomComplete(entry_hash);
  callback.Run(result);
}

// static
void SimpleBackendImpl::FlushWorkerPoolForTesting() {
  // TODO(morlovich): Remove this, move everything over to disk_cache:: use.
  base::TaskScheduler::GetInstance()->FlushForTesting();
}

}  // namespace disk_cache
