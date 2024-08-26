// Copyright 2024, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/engine_shard.h"

#include <absl/strings/match.h>

#include "base/flags.h"
#include "io/proc_reader.h"

extern "C" {
#include "redis/zmalloc.h"
}

#include "server/engine_shard_set.h"
#include "server/namespaces.h"
#include "server/search/doc_index.h"
#include "server/server_state.h"
#include "server/tiered_storage.h"
#include "server/transaction.h"
#include "util/fibers/proactor_base.h"

using namespace std;

ABSL_FLAG(float, mem_defrag_threshold, 0.7,
          "Minimum percentage of used memory relative to maxmemory cap before running "
          "defragmentation");

ABSL_FLAG(uint32_t, mem_defrag_check_sec_interval, 10,
          "Number of seconds between every defragmentation necessity check");

ABSL_FLAG(float, mem_defrag_waste_threshold, 0.2,
          "The ratio of wasted/committed memory above which we run defragmentation");

ABSL_FLAG(float, mem_defrag_page_utilization_threshold, 0.8,
          "memory page under utilization threshold. Ratio between used and committed size, below "
          "this, memory in this page will defragmented");

ABSL_FLAG(uint32_t, hz, 100,
          "Base frequency at which the server performs other background tasks. "
          "Warning: not advised to decrease in production.");

ABSL_FLAG(string, shard_round_robin_prefix, "",
          "When non-empty, keys which start with this prefix are not distributed across shards "
          "based on their value but instead via round-robin. Use cautiously! This can efficiently "
          "support up to a few hundreds of prefixes. Note: prefix is looked inside hash tags when "
          "cluster mode is enabled.");

ABSL_FLAG(string, tiered_prefix, "",
          "Enables tiered storage if set. "
          "The string denotes the path and prefix of the files "
          " associated with tiered storage. Stronly advised to use "
          "high performance NVME ssd disks for this. Also, seems that pipeline_squash does "
          "not work well with tiered storage, so it's advised to set it to 0.");

ABSL_FLAG(float, tiered_offload_threshold, 0.5,
          "The ratio of used/max memory above which we start offloading values to disk");

ABSL_FLAG(bool, enable_heartbeat_eviction, true,
          "Enable eviction during heartbeat when memory is under pressure.");

namespace dfly {

using absl::GetFlag;
using namespace util;

namespace {

constexpr uint64_t kCursorDoneState = 0u;

struct ShardMemUsage {
  std::size_t commited = 0;
  std::size_t used = 0;
  std::size_t wasted_mem = 0;
};

std::ostream& operator<<(std::ostream& os, const ShardMemUsage& mem) {
  return os << "commited: " << mem.commited << " vs used " << mem.used << ", wasted memory "
            << mem.wasted_mem;
}

ShardMemUsage ReadShardMemUsage(float wasted_ratio) {
  ShardMemUsage usage;
  zmalloc_get_allocator_wasted_blocks(wasted_ratio, &usage.used, &usage.commited,
                                      &usage.wasted_mem);
  return usage;
}

// RoundRobinSharder implements a way to distribute keys that begin with some prefix.
// Round-robin is disabled by default. It is not a general use-case optimization, but instead only
// reasonable when there are a few highly contended keys, which we'd like to spread between the
// shards evenly.
// When enabled, the distribution is done via hash table: the hash of the key is used to look into
// a pre-allocated vector. This means that collisions are possible, but are very unlikely if only
// a few keys are used.
// Thread safe.
class RoundRobinSharder {
 public:
  static void Init() {
    round_robin_prefix_ = absl::GetFlag(FLAGS_shard_round_robin_prefix);

    if (IsEnabled()) {
      // ~100k entries will consume 200kb per thread, and will allow 100 keys with < 2.5% collision
      // probability. Since this has a considerable footprint, we only allocate when enabled. We're
      // using a prime number close to 100k for better utilization.
      constexpr size_t kRoundRobinSize = 100'003;
      round_robin_shards_tl_cache_.resize(kRoundRobinSize);
      std::fill(round_robin_shards_tl_cache_.begin(), round_robin_shards_tl_cache_.end(),
                kInvalidSid);

      std::lock_guard guard(mutex_);
      if (round_robin_shards_.empty()) {
        round_robin_shards_ = round_robin_shards_tl_cache_;
      }
    }
  }

  static void Destroy() {
    round_robin_shards_tl_cache_.clear();

    std::lock_guard guard(mutex_);
    round_robin_shards_.clear();
  }

  static bool IsEnabled() {
    return !round_robin_prefix_.empty();
  }

  static optional<ShardId> TryGetShardId(string_view key, XXH64_hash_t key_hash) {
    DCHECK(!round_robin_shards_tl_cache_.empty());

    if (!absl::StartsWith(key, round_robin_prefix_)) {
      return nullopt;
    }

    size_t index = key_hash % round_robin_shards_tl_cache_.size();
    ShardId sid = round_robin_shards_tl_cache_[index];

    if (sid == kInvalidSid) {
      std::lock_guard guard(mutex_);
      sid = round_robin_shards_[index];
      if (sid == kInvalidSid) {
        sid = next_shard_;
        round_robin_shards_[index] = sid;
        next_shard_ = (next_shard_ + 1) % shard_set->size();
      }
      round_robin_shards_tl_cache_[index] = sid;
    }

    return sid;
  }

 private:
  static thread_local string round_robin_prefix_;
  static thread_local vector<ShardId> round_robin_shards_tl_cache_;
  static vector<ShardId> round_robin_shards_ ABSL_GUARDED_BY(mutex_);
  static ShardId next_shard_ ABSL_GUARDED_BY(mutex_);
  static fb2::Mutex mutex_;
};

bool HasContendedLocks(ShardId shard_id, Transaction* trx, const DbTable* table) {
  auto is_contended = [table](LockFp fp) { return table->trans_locks.Find(fp)->IsContended(); };

  if (trx->IsMulti()) {
    auto fps = trx->GetMultiFps();
    for (const auto& [sid, fp] : fps) {
      if (sid == shard_id && is_contended(fp))
        return true;
    }
  } else {
    KeyLockArgs lock_args = trx->GetLockArgs(shard_id);
    for (size_t i = 0; i < lock_args.fps.size(); ++i) {
      if (is_contended(lock_args.fps[i]))
        return true;
    }
  }

  return false;
}

thread_local string RoundRobinSharder::round_robin_prefix_;
thread_local vector<ShardId> RoundRobinSharder::round_robin_shards_tl_cache_;
vector<ShardId> RoundRobinSharder::round_robin_shards_;
ShardId RoundRobinSharder::next_shard_;
fb2::Mutex RoundRobinSharder::mutex_;

constexpr size_t kQueueLen = 64;

}  // namespace

__thread EngineShard* EngineShard::shard_ = nullptr;
uint64_t TEST_current_time_ms = 0;

ShardId Shard(string_view v, ShardId shard_num) {
  if (cluster::IsClusterShardedByTag()) {
    v = LockTagOptions::instance().Tag(v);
  }

  XXH64_hash_t hash = XXH64(v.data(), v.size(), 120577240643ULL);

  if (RoundRobinSharder::IsEnabled()) {
    auto round_robin = RoundRobinSharder::TryGetShardId(v, hash);
    if (round_robin.has_value()) {
      return *round_robin;
    }
  }

  return hash % shard_num;
}

EngineShard::Stats& EngineShard::Stats::operator+=(const EngineShard::Stats& o) {
  static_assert(sizeof(Stats) == 48);

  defrag_attempt_total += o.defrag_attempt_total;
  defrag_realloc_total += o.defrag_realloc_total;
  defrag_task_invocation_total += o.defrag_task_invocation_total;
  poll_execution_total += o.poll_execution_total;
  tx_ooo_total += o.tx_ooo_total;
  tx_immediate_total += o.tx_immediate_total;

  return *this;
}

void EngineShard::DefragTaskState::UpdateScanState(uint64_t cursor_val) {
  cursor = cursor_val;
  // Once we're done with a db, jump to the next
  if (cursor == kCursorDoneState) {
    dbid++;
  }
}

void EngineShard::DefragTaskState::ResetScanState() {
  dbid = cursor = 0u;
}

// This function checks 3 things:
// 1. Don't try memory fragmentation if we don't use "enough" memory (control by
// mem_defrag_threshold flag)
// 2. We have memory blocks that can be better utilized (there is a "wasted memory" in them).
// 3. in case the above is OK, make sure that we have a "gap" between usage and commited memory
// (control by mem_defrag_waste_threshold flag)
bool EngineShard::DefragTaskState::CheckRequired() {
  if (is_force_defrag || cursor > kCursorDoneState) {
    is_force_defrag = false;
    VLOG(2) << "cursor: " << cursor << " and is_force_defrag " << is_force_defrag;
    return true;
  }

  const std::size_t memory_per_shard = max_memory_limit / shard_set->size();
  if (memory_per_shard < (1 << 16)) {  // Too small.
    return false;
  }

  const std::size_t global_threshold = max_memory_limit * GetFlag(FLAGS_mem_defrag_threshold);
  if (global_threshold > rss_mem_current.load(memory_order_relaxed)) {
    return false;
  }

  const auto now = time(nullptr);
  const auto seconds_from_prev_check = now - last_check_time;
  const auto mem_defrag_interval = GetFlag(FLAGS_mem_defrag_check_sec_interval);

  if (seconds_from_prev_check < mem_defrag_interval) {
    return false;
  }
  last_check_time = now;

  ShardMemUsage usage = ReadShardMemUsage(GetFlag(FLAGS_mem_defrag_page_utilization_threshold));

  const double waste_threshold = GetFlag(FLAGS_mem_defrag_waste_threshold);
  if (usage.wasted_mem > (uint64_t(usage.commited * waste_threshold))) {
    VLOG(1) << "memory issue found for memory " << usage;
    return true;
  }

  return false;
}

void EngineShard::ForceDefrag() {
  defrag_state_.is_force_defrag = true;
}

bool EngineShard::DoDefrag() {
  // --------------------------------------------------------------------------
  // NOTE: This task is running with exclusive access to the shard.
  // i.e. - Since we are using shared nothing access here, and all access
  // are done using fibers, This fiber is run only when no other fiber in the
  // context of the controlling thread will access this shard!
  // --------------------------------------------------------------------------

  constexpr size_t kMaxTraverses = 40;
  const float threshold = GetFlag(FLAGS_mem_defrag_page_utilization_threshold);

  // TODO: enable tiered storage on non-default db slice
  DbSlice& slice = namespaces.GetDefaultNamespace().GetDbSlice(shard_->shard_id());

  // If we moved to an invalid db, skip as long as it's not the last one
  while (!slice.IsDbValid(defrag_state_.dbid) && defrag_state_.dbid + 1 < slice.db_array_size())
    defrag_state_.dbid++;

  // If we found no valid db, we finished traversing and start from scratch next time
  if (!slice.IsDbValid(defrag_state_.dbid)) {
    defrag_state_.ResetScanState();
    return false;
  }

  DCHECK(slice.IsDbValid(defrag_state_.dbid));
  auto [prime_table, expire_table] = slice.GetTables(defrag_state_.dbid);
  PrimeTable::Cursor cur = defrag_state_.cursor;
  uint64_t reallocations = 0;
  unsigned traverses_count = 0;
  uint64_t attempts = 0;

  do {
    cur = slice.Traverse(prime_table, cur, [&](PrimeIterator it) {
      // for each value check whether we should move it because it
      // seats on underutilized page of memory, and if so, do it.
      bool did = it->second.DefragIfNeeded(threshold);
      attempts++;
      if (did) {
        reallocations++;
      }
    });
    traverses_count++;
  } while (traverses_count < kMaxTraverses && cur && namespaces.IsInitialized());

  defrag_state_.UpdateScanState(cur.value());

  if (reallocations > 0) {
    VLOG(1) << "shard " << slice.shard_id() << ": successfully defrag  " << reallocations
            << " times, did it in " << traverses_count << " cursor is at the "
            << (defrag_state_.cursor == kCursorDoneState ? "end" : "in progress");
  } else {
    VLOG(1) << "shard " << slice.shard_id() << ": run the defrag " << traverses_count
            << " times out of maximum " << kMaxTraverses << ", with cursor at "
            << (defrag_state_.cursor == kCursorDoneState ? "end" : "in progress")
            << " but no location for defrag were found";
  }

  stats_.defrag_realloc_total += reallocations;
  stats_.defrag_task_invocation_total++;
  stats_.defrag_attempt_total += attempts;

  return true;
}

// the memory defragmentation task is as follow:
//  1. Check if memory usage is high enough
//  2. Check if diff between commited and used memory is high enough
//  3. if all the above pass -> scan this shard and try to find whether we can move pointer to
//  underutilized pages values
//     if the cursor returned from scan is not in done state, schedule the task to run at high
//     priority.
//     otherwise lower the task priority so that it would not use the CPU when not required
uint32_t EngineShard::DefragTask() {
  if (!namespaces.IsInitialized()) {
    return util::ProactorBase::kOnIdleMaxLevel;
  }

  constexpr uint32_t kRunAtLowPriority = 0u;

  if (defrag_state_.CheckRequired()) {
    VLOG(2) << shard_id_ << ": need to run defrag memory cursor state: " << defrag_state_.cursor;
    if (DoDefrag()) {
      // we didn't finish the scan
      return util::ProactorBase::kOnIdleMaxLevel;
    }
  }
  return kRunAtLowPriority;
}

EngineShard::EngineShard(util::ProactorBase* pb, mi_heap_t* heap)
    : queue_(kQueueLen, 1, 1),
      queue2_(kQueueLen / 2, 2, 2),
      txq_([](const Transaction* t) { return t->txid(); }),
      mi_resource_(heap),
      shard_id_(pb->GetPoolIndex()) {
  tmp_str1 = sdsempty();

  defrag_task_ = pb->AddOnIdleTask([this]() { return DefragTask(); });
  queue_.Start(absl::StrCat("shard_queue_", shard_id()));
  queue2_.Start(absl::StrCat("l2_queue_", shard_id()));
}

EngineShard::~EngineShard() {
  sdsfree(tmp_str1);
}

void EngineShard::Shutdown() {
  DVLOG(1) << "EngineShard::Shutdown";

  queue_.Shutdown();
  queue2_.Shutdown();
  DCHECK(!fiber_periodic_.IsJoinable());

  ProactorBase::me()->RemoveOnIdleTask(defrag_task_);
}

void EngineShard::StopPeriodicFiber() {
  fiber_periodic_done_.Notify();
  if (fiber_periodic_.IsJoinable()) {
    fiber_periodic_.Join();
  }
}

void EngineShard::StartPeriodicFiber(util::ProactorBase* pb, std::function<void()> global_handler) {
  uint32_t clock_cycle_ms = 1000 / std::max<uint32_t>(1, GetFlag(FLAGS_hz));
  if (clock_cycle_ms == 0)
    clock_cycle_ms = 1;

  fiber_periodic_ = MakeFiber([this, index = pb->GetPoolIndex(), period_ms = clock_cycle_ms,
                               handler = std::move(global_handler)] {
    ThisFiber::SetName(absl::StrCat("shard_periodic", index));
    RunPeriodic(std::chrono::milliseconds(period_ms), std::move(handler));
  });
}

void EngineShard::InitThreadLocal(ProactorBase* pb) {
  CHECK(shard_ == nullptr) << pb->GetPoolIndex();

  mi_heap_t* data_heap = ServerState::tlocal()->data_heap();
  void* ptr = mi_heap_malloc_aligned(data_heap, sizeof(EngineShard), alignof(EngineShard));
  shard_ = new (ptr) EngineShard(pb, data_heap);

  CompactObj::InitThreadLocal(shard_->memory_resource());
  SmallString::InitThreadLocal(data_heap);

  RoundRobinSharder::Init();

  shard_->shard_search_indices_.reset(new ShardDocIndices());
}

void EngineShard::InitTieredStorage(ProactorBase* pb, size_t max_file_size) {
  if (string backing_prefix = GetFlag(FLAGS_tiered_prefix); !backing_prefix.empty()) {
    LOG_IF(FATAL, pb->GetKind() != ProactorBase::IOURING)
        << "Only ioring based backing storage is supported. Exiting...";

    // TODO: enable tiered storage on non-default namespace
    DbSlice& db_slice = namespaces.GetDefaultNamespace().GetDbSlice(shard_id());
    auto* shard = EngineShard::tlocal();
    shard->tiered_storage_ = make_unique<TieredStorage>(max_file_size, &db_slice);
    error_code ec = shard->tiered_storage_->Open(backing_prefix);
    CHECK(!ec) << ec.message();
  }
}

void EngineShard::DestroyThreadLocal() {
  if (!shard_)
    return;

  uint32_t shard_id = shard_->shard_id();
  mi_heap_t* tlh = shard_->mi_resource_.heap();

  shard_->Shutdown();

  shard_->~EngineShard();
  mi_free(shard_);
  shard_ = nullptr;
  CompactObj::InitThreadLocal(nullptr);
  mi_heap_delete(tlh);
  RoundRobinSharder::Destroy();
  VLOG(1) << "Shard reset " << shard_id;
}

// Is called by Transaction::ExecuteAsync in order to run transaction tasks.
// Only runs in its own thread.
void EngineShard::PollExecution(const char* context, Transaction* trans) {
  DVLOG(2) << "PollExecution " << context << " " << (trans ? trans->DebugId() : "") << " "
           << txq_.size() << " " << (continuation_trans_ ? continuation_trans_->DebugId() : "");

  ShardId sid = shard_id();
  stats_.poll_execution_total++;

  // If any of the following flags are present, we are guaranteed to run in this function:
  // 1. AWAKED_Q -> Blocking transactions are executed immediately after waking up, they don't
  // occupy a place in txq and have highest priority
  // 2. SUSPENDED_Q -> Suspended shards are run to clean up and finalize blocking keys
  // 3. OUT_OF_ORDER -> Transactions without conflicting keys can run earlier than their position in
  // txq is reached
  uint16_t flags = Transaction::AWAKED_Q | Transaction::SUSPENDED_Q | Transaction::OUT_OF_ORDER;
  auto [trans_mask, disarmed] =
      trans ? trans->DisarmInShardWhen(sid, flags) : make_pair(uint16_t(0), false);

  if (trans && trans_mask == 0)  // If not armed, it means that this poll task expired
    return;

  if (trans_mask & Transaction::AWAKED_Q) {
    CHECK(trans->GetNamespace().GetBlockingController(shard_id_)->HasAwakedTransaction());
    CHECK(continuation_trans_ == nullptr)
        << continuation_trans_->DebugId() << " when polling " << trans->DebugId()
        << "cont_mask: " << continuation_trans_->DEBUG_GetLocalMask(sid) << " vs "
        << trans->DEBUG_GetLocalMask(sid);

    // Commands like BRPOPLPUSH don't conclude immediately
    if (trans->RunInShard(this, false)) {
      // execution is blocked while HasAwakedTransaction() returns true, so no need to set
      // continuation_trans_. Moreover, setting it for wakened multi-hop transactions may lead to
      // inconcistency, see BLMoveSimultaneously test.
      // continuation_trans_ = trans;
      return;
    }

    trans = nullptr;  // Avoid handling the caller below
    continuation_trans_ = nullptr;
  }

  string dbg_id;
  bool update_stats = false;

  auto run = [this, &dbg_id, &update_stats](Transaction* tx, bool is_ooo) -> bool /* keep */ {
    dbg_id = VLOG_IS_ON(1) ? tx->DebugId() : "";
    bool keep = tx->RunInShard(this, is_ooo);
    DLOG_IF(INFO, !dbg_id.empty()) << dbg_id << ", keep " << keep << ", ooo " << is_ooo;
    update_stats = true;
    return keep;
  };

  // Check the currently running transaction, we have to handle it first until it concludes
  if (continuation_trans_) {
    bool is_self = continuation_trans_ == trans;
    if (is_self)
      trans = nullptr;

    if ((is_self && disarmed) || continuation_trans_->DisarmInShard(sid)) {
      auto bc = continuation_trans_->GetNamespace().GetBlockingController(shard_id_);
      if (bool keep = run(continuation_trans_, false); !keep) {
        // if this holds, we can remove this check altogether.
        DCHECK(continuation_trans_ == nullptr);
        continuation_trans_ = nullptr;
      }
      if (bc && bc->HasAwakedTransaction()) {
        // Break if there are any awakened transactions, as we must give way to them
        // before continuing to handle regular transactions from the queue.
        return;
      }
    }
  }

  // Progress on the transaction queue if no transaction is running currently.
  Transaction* head = nullptr;

  while (continuation_trans_ == nullptr && !txq_.Empty()) {
    head = get<Transaction*>(txq_.Front());

    // Break if there are any awakened transactions, as we must give way to them
    // before continuing to handle regular transactions from the queue.
    if (head->GetNamespace().GetBlockingController(shard_id_) &&
        head->GetNamespace().GetBlockingController(shard_id_)->HasAwakedTransaction())
      break;

    VLOG(2) << "Considering head " << head->DebugId()
            << " isarmed: " << head->DEBUG_IsArmedInShard(sid);

    // If the transaction isn't armed yet, it will be handled by a successive poll
    bool should_run = (head == trans && disarmed) || head->DisarmInShard(sid);
    if (!should_run)
      break;

    // Avoid processing the caller transaction below if we found it in the queue,
    // because it most likely won't have enough time to arm itself again.
    if (head == trans)
      trans = nullptr;

    TxId txid = head->txid();

    // Update commited_txid before running, because RunInShard might block on i/o.
    // This way scheduling transactions won't see an understated value.
    DCHECK_LT(committed_txid_, txid);  //  strictly increasing when processed via txq
    committed_txid_ = txid;

    if (bool keep = run(head, false); keep)
      continuation_trans_ = head;
  }

  // If we disarmed, but didn't find ourselves in the loop, run now.
  if (trans && disarmed) {
    DCHECK(trans != head);
    DCHECK(trans_mask & (Transaction::OUT_OF_ORDER | Transaction::SUSPENDED_Q));

    bool is_ooo = trans_mask & Transaction::OUT_OF_ORDER;
    bool keep = run(trans, is_ooo);
    if (is_ooo && !keep) {
      stats_.tx_ooo_total++;
    }

    // If the transaction concluded, it must remove itself from the tx queue.
    // Otherwise it is required to stay there to keep the relative order.
    if (is_ooo && !trans->IsMulti())
      DCHECK_EQ(keep, trans->DEBUG_GetTxqPosInShard(sid) != TxQueue::kEnd);
  }
  if (update_stats) {
    CacheStats();
  }
}

void EngineShard::RemoveContTx(Transaction* tx) {
  if (continuation_trans_ == tx) {
    continuation_trans_ = nullptr;
  }
}

void EngineShard::Heartbeat() {
  DCHECK(namespaces.IsInitialized());

  CacheStats();

  if (!IsReplica()) {  // Never run expiry/evictions on replica.
    RetireExpiredAndEvict();
  }

  // TODO: iterate over all namespaces
  DbSlice& db_slice = namespaces.GetDefaultNamespace().GetDbSlice(shard_id());

  // Offset CoolMemoryUsage when consider background offloading.
  // TODO: Another approach could be is to align the approach  similarly to how we do with
  // FreeMemWithEvictionStep, i.e. if memory_budget is below the limit.
  size_t tiering_offload_threshold =
      tiered_storage_ ? tiered_storage_->CoolMemoryUsage() +
                            size_t(max_memory_limit * GetFlag(FLAGS_tiered_offload_threshold)) /
                                shard_set->size()
                      : std::numeric_limits<size_t>::max();
  size_t used_memory = UsedMemory();
  if (used_memory > tiering_offload_threshold) {
    VLOG(1) << "Running Offloading, memory=" << used_memory
            << " tiering_threshold: " << tiering_offload_threshold
            << ", cool memory: " << tiered_storage_->CoolMemoryUsage();

    for (unsigned i = 0; i < db_slice.db_array_size(); ++i) {
      if (!db_slice.IsDbValid(i))
        continue;
      tiered_storage_->RunOffloading(i);
    }
  }
}

void EngineShard::RetireExpiredAndEvict() {
  // TODO: iterate over all namespaces
  DbSlice& db_slice = namespaces.GetDefaultNamespace().GetDbSlice(shard_id());
  // Some of the functions below might acquire the same lock again so we need to unlock it
  // asap. We won't yield before we relock the mutex again, so the code below is atomic
  // in respect to preemptions of big values. An example of that is the call to
  // DeleteExpiredStep() below, which eventually calls ExpireIfNeeded()
  // and within that the call to RecordExpiry() will trigger the registered
  // callback OnJournalEntry which locks the exact same mutex.
  // We need to lock below and immediately release because there should be no other fiber
  // that is serializing a big value.
  { std::unique_lock lk(db_slice.GetSerializationMutex()); }
  constexpr double kTtlDeleteLimit = 200;
  constexpr double kRedLimitFactor = 0.1;

  uint32_t traversed = GetMovingSum6(TTL_TRAVERSE);
  uint32_t deleted = GetMovingSum6(TTL_DELETE);
  unsigned ttl_delete_target = 5;

  if (deleted > 10) {
    // deleted should be <= traversed.
    // hence we map our delete/traversed ratio into a range [0, kTtlDeleteLimit).
    // The higher ttl_delete_target the more likely we have lots of expired items that need
    // to be deleted.
    ttl_delete_target = kTtlDeleteLimit * double(deleted) / (double(traversed) + 10);
  }

  ssize_t eviction_redline = size_t(max_memory_limit * kRedLimitFactor) / shard_set->size();

  DbContext db_cntx;
  db_cntx.time_now_ms = GetCurrentTimeMs();

  for (unsigned i = 0; i < db_slice.db_array_size(); ++i) {
    if (!db_slice.IsDbValid(i))
      continue;

    db_cntx.db_index = i;
    auto [pt, expt] = db_slice.GetTables(i);
    if (expt->size() > pt->size() / 4) {
      DbSlice::DeleteExpiredStats stats = db_slice.DeleteExpiredStep(db_cntx, ttl_delete_target);

      counter_[TTL_TRAVERSE].IncBy(stats.traversed);
      counter_[TTL_DELETE].IncBy(stats.deleted);
    }

    // if our budget is below the limit
    if (db_slice.memory_budget() < eviction_redline && GetFlag(FLAGS_enable_heartbeat_eviction)) {
      uint32_t starting_segment_id = rand() % pt->GetSegmentCount();
      db_slice.FreeMemWithEvictionStep(i, starting_segment_id,
                                       eviction_redline - db_slice.memory_budget());
    }
  }

  // Journal entries for expired entries are not writen to socket in the loop above.
  // Trigger write to socket when loop finishes.
  if (auto journal = EngineShard::tlocal()->journal(); journal) {
    TriggerJournalWriteToSink();
  }
}

void EngineShard::RunPeriodic(std::chrono::milliseconds period_ms,
                              std::function<void()> shard_handler) {
  VLOG(1) << "RunPeriodic with period " << period_ms.count() << "ms";

  bool runs_global_periodic = (shard_id() == 0);  // Only shard 0 runs global periodic.
  unsigned global_count = 0;
  int64_t last_stats_time = time(nullptr);
  int64_t last_heartbeat_ms = INT64_MAX;
  int64_t last_handler_ms = 0;

  while (true) {
    if (fiber_periodic_done_.WaitFor(period_ms)) {
      VLOG(2) << "finished running engine shard periodic task";
      return;
    }

    int64_t now_ms = fb2::ProactorBase::GetMonotonicTimeNs() / 1000000;
    if (now_ms - 5 * period_ms.count() > last_heartbeat_ms) {
      VLOG(1) << "This heartbeat-sleep took " << now_ms - last_heartbeat_ms << "ms";
    }
    Heartbeat();
    last_heartbeat_ms = fb2::ProactorBase::GetMonotonicTimeNs() / 1000000;
    if (shard_handler && last_handler_ms + 100 < last_heartbeat_ms) {
      last_handler_ms = last_heartbeat_ms;
      shard_handler();
    }

    if (runs_global_periodic) {
      ++global_count;

      // Every 8 runs, update the global stats.
      if (global_count % 8 == 0) {
        DVLOG(2) << "Global periodic";

        uint64_t mem_current = used_mem_current.load(std::memory_order_relaxed);

        // Single writer, so no races.
        if (mem_current > used_mem_peak.load(memory_order_relaxed))
          used_mem_peak.store(mem_current, memory_order_relaxed);

        int64_t cur_time = time(nullptr);
        if (cur_time != last_stats_time) {
          last_stats_time = cur_time;
          io::Result<io::StatusData> sdata_res = io::ReadStatusInfo();
          if (sdata_res) {
            size_t total_rss = sdata_res->vm_rss + sdata_res->hugetlb_pages;
            rss_mem_current.store(total_rss, memory_order_relaxed);
            if (rss_mem_peak.load(memory_order_relaxed) < total_rss)
              rss_mem_peak.store(total_rss, memory_order_relaxed);
          }
        }
      }
    }
  }
}

void EngineShard::CacheStats() {
  uint64_t now = fb2::ProactorBase::GetMonotonicTimeNs();
  if (cache_stats_time_ + 1000000 > now)  // 1ms
    return;

  cache_stats_time_ = now;
  // Used memory for this shard.
  size_t used_mem = UsedMemory();
  DbSlice& db_slice = namespaces.GetDefaultNamespace().GetDbSlice(shard_id());

  // delta can wrap if used_memory is smaller than last_cached_used_memory_ and it's fine.
  size_t delta = used_mem - last_cached_used_memory_;
  last_cached_used_memory_ = used_mem;
  size_t current = used_mem_current.fetch_add(delta, memory_order_relaxed) + delta;
  ssize_t free_mem = max_memory_limit - current;

  size_t entries = db_slice.entries_count();
  size_t table_memory = db_slice.table_memory();

  if (tiered_storage_) {
    table_memory += tiered_storage_->CoolMemoryUsage();
  }
  size_t obj_memory = table_memory <= used_mem ? used_mem - table_memory : 0;

  size_t bytes_per_obj = entries > 0 ? obj_memory / entries : 0;
  db_slice.SetCachedParams(free_mem / shard_set->size(), bytes_per_obj);
}

size_t EngineShard::UsedMemory() const {
  return mi_resource_.used() + zmalloc_used_memory_tl + SmallString::UsedThreadLocal() +
         search_indices()->GetUsedMemory();
}

bool EngineShard::ShouldThrottleForTiering() const {  // see header for formula justification
  if (!tiered_storage_)
    return false;

  size_t tiering_redline =
      (max_memory_limit * GetFlag(FLAGS_tiered_offload_threshold)) / shard_set->size();

  // UsedMemory includes CoolMemoryUsage, so we are offsetting it to remove the cool cache impact.
  return tiered_storage_->WriteDepthUsage() > 0.3 &&
         (UsedMemory() > tiering_redline + tiered_storage_->CoolMemoryUsage());
}

auto EngineShard::AnalyzeTxQueue() const -> TxQueueInfo {
  const TxQueue* queue = txq();

  ShardId sid = shard_id();
  TxQueueInfo info;

  if (queue->Empty())
    return info;

  auto cur = queue->Head();
  info.tx_total = queue->size();
  unsigned max_db_id = 0;

  auto& db_slice = namespaces.GetDefaultNamespace().GetCurrentDbSlice();

  do {
    auto value = queue->At(cur);
    Transaction* trx = std::get<Transaction*>(value);
    // find maximum index of databases used by transactions
    if (trx->GetDbIndex() > max_db_id) {
      max_db_id = trx->GetDbIndex();
    }

    bool is_armed = trx->DEBUG_IsArmedInShard(sid);
    DVLOG(1) << "Inspecting " << trx->DebugId() << " is_armed " << is_armed;
    if (is_armed) {
      info.tx_armed++;

      if (trx->IsGlobal() || (trx->IsMulti() && trx->GetMultiMode() == Transaction::GLOBAL)) {
        info.tx_global++;
      } else {
        const DbTable* table = db_slice.GetDBTable(trx->GetDbIndex());
        bool can_run = !HasContendedLocks(sid, trx, table);
        if (can_run) {
          info.tx_runnable++;
        }
      }
    }
    cur = queue->Next(cur);
  } while (cur != queue->Head());

  // Analyze locks
  for (unsigned i = 0; i <= max_db_id; ++i) {
    const DbTable* table = db_slice.GetDBTable(i);
    if (table == nullptr)
      continue;

    info.total_locks += table->trans_locks.Size();
    for (const auto& [key, lock] : table->trans_locks) {
      if (lock.IsContended()) {
        info.contended_locks++;
        if (lock.ContentionScore() > info.max_contention_score) {
          info.max_contention_score = lock.ContentionScore();
          info.max_contention_lock = key;
        }
      }
    }
  }

  return info;
}

}  // namespace dfly