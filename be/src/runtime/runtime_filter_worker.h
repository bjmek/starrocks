// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include <atomic>
#include <map>
#include <thread>

#include "common/global_types.h"
#include "common/object_pool.h"
#include "common/status.h"
#include "gen_cpp/InternalService_types.h"
#include "gen_cpp/PlanNodes_types.h"
#include "gen_cpp/Types_types.h"
#include "gen_cpp/internal_service.pb.h"
#include "util/blocking_queue.hpp"
#include "util/uid_util.h"
namespace starrocks {

class ExecEnv;
class RuntimeState;

namespace vectorized {
class JoinRuntimeFilter;
class RuntimeFilterProbeDescriptor;
class RuntimeFilterBuildDescriptor;
} // namespace vectorized

class RuntimeFilterRpcClosure;
// RuntimeFilterPort is bind to a fragment instance
// and it's to exchange RF(publish/receive) with outside world.
class RuntimeFilterPort {
public:
    RuntimeFilterPort(RuntimeState* state) : _state(state) {}
    void add_listener(vectorized::RuntimeFilterProbeDescriptor* rf_desc);
    void publish_runtime_filters(std::list<vectorized::RuntimeFilterBuildDescriptor*>& rf_descs);
    // receiver runtime filter allocated in this fragment instance(broadcast join generate it)
    // or allocated in this query(shuffle join generate global runtime filter)
    void receive_runtime_filter(int32_t filter_id, const vectorized::JoinRuntimeFilter* rf);
    void receive_shared_runtime_filter(int32_t filter_id,
                                       const std::shared_ptr<const vectorized::JoinRuntimeFilter>& rf);

private:
    std::map<int32_t, std::list<vectorized::RuntimeFilterProbeDescriptor*>> _listeners;
    RuntimeState* _state;
};

class RuntimeFilterMergerStatus {
public:
    RuntimeFilterMergerStatus() = default;
    RuntimeFilterMergerStatus(RuntimeFilterMergerStatus&& other)
            : arrives(std::move(other.arrives)),
              expect_number(other.expect_number),
              pool(std::move(other.pool)),
              filters(std::move(other.filters)),
              current_size(other.current_size),
              max_size(other.max_size),
              stop(other.stop),
              recv_first_filter_ts(other.recv_first_filter_ts),
              recv_last_filter_ts(other.recv_last_filter_ts),
              broadcast_filter_ts(other.broadcast_filter_ts) {}
    // which be number send this rf.
    std::unordered_set<int32_t> arrives;
    // how many partitioned rf we expect
    int32_t expect_number;
    ObjectPool pool;
    // each partitioned rf.
    std::map<int32_t, vectorized::JoinRuntimeFilter*> filters;
    size_t current_size = 0;
    size_t max_size = 0;
    bool stop = false;

    // statistics.
    // timestamp in ms since unix epoch;
    // we care about diff not abs value.
    int64_t recv_first_filter_ts = 0;
    int64_t recv_last_filter_ts = 0;
    int64_t broadcast_filter_ts = 0;
};

// RuntimeFilterMerger is to merge partitioned RF
// and sent merged RF to consumer nodes.
class RuntimeFilterMerger {
public:
    RuntimeFilterMerger(ExecEnv* env, UniqueId query_id, TQueryOptions query_options);
    Status init(const TRuntimeFilterParams& params);
    void merge_runtime_filter(PTransmitRuntimeFilterParams& params, RuntimeFilterRpcClosure* rpc_closure);

private:
    void _send_total_runtime_filter(int32_t filter_id, RuntimeFilterRpcClosure* rpc_closure);
    // filter_id -> where this filter should send to
    std::map<int32_t, std::vector<TRuntimeFilterProberParams>> _targets;
    std::map<int32_t, RuntimeFilterMergerStatus> _statuses;
    ExecEnv* _exec_env;
    UniqueId _query_id;
    TQueryOptions _query_options;
};

// RuntimeFilterWorker works in a separated thread, and does following jobs:
// 1. deserialize runtime filters.
// 2. merge runtime filters.

// it works in a event-driven way, and possible events are:
// - create a runtime filter merger for a query
// - receive partitioned RF, deserialize it and merge it, and sent total RF(for merge node)
// - receive total RF and send it to RuntimeFilterPort
// - send partitioned RF(for hash join node)
// - close a query(delete runtime filter merger)
class RuntimeFilterWorkerEvent;
class RuntimeFilterWorker {
public:
    RuntimeFilterWorker(ExecEnv* env);
    ~RuntimeFilterWorker();
    // open query for creating runtime filter merger.
    void open_query(TUniqueId query_id, TQueryOptions query_options, const TRuntimeFilterParams& params);
    void close_query(TUniqueId query_id);
    void receive_runtime_filter(const PTransmitRuntimeFilterParams& params);
    void execute();
    void send_part_runtime_filter(PTransmitRuntimeFilterParams&& params,
                                  const std::vector<starrocks::TNetworkAddress>& addrs, int timeout_ms);

private:
    void _receive_total_runtime_filter(PTransmitRuntimeFilterParams& params, RuntimeFilterRpcClosure* rpc_closure);
    UnboundedBlockingQueue<RuntimeFilterWorkerEvent> _queue;
    std::unordered_map<TUniqueId, RuntimeFilterMerger> _mergers;
    ExecEnv* _exec_env;
    std::atomic<bool> _stop;
    std::thread _thread;
};

}; // namespace starrocks
