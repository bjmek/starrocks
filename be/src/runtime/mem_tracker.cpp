// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/mem_tracker.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/mem_tracker.h"

#include <stdint.h>

#include <boost/algorithm/string/join.hpp>
#include <limits>
#include <memory>

#include "exec/exec_node.h"
#include "gutil/strings/substitute.h"
#include "runtime/bufferpool/reservation_tracker_counters.h"
#include "runtime/current_thread.h"
#include "runtime/exec_env.h"
#include "runtime/runtime_state.h"
#include "util/debug_util.h"
#include "util/mem_info.h"
#include "util/pretty_printer.h"
#include "util/stack_util.h"
#include "util/starrocks_metrics.h"
#include "util/uid_util.h"

namespace starrocks {

const std::string MemTracker::COUNTER_NAME = "PeakMemoryUsage";

MemTracker::MemTracker(int64_t byte_limit, const std::string& label, MemTracker* parent, bool auto_unregister,
                       bool log_usage_if_zero)
        : _type(NO_SET),
          _limit(byte_limit),
          _label(label),
          _parent(parent),
          _consumption(&_local_counter),
          _local_counter(TUnit::BYTES),
          _log_usage_if_zero(log_usage_if_zero),
          _auto_unregister(auto_unregister) {
    if (parent != NULL) _parent->add_child_tracker(this);
    Init();
}

MemTracker::MemTracker(Type type, int64_t byte_limit, const std::string& label, MemTracker* parent,
                       bool auto_unregister, bool log_usage_if_zero)
        : _type(type),
          _limit(byte_limit),
          _label(label),
          _parent(parent),
          _consumption(&_local_counter),
          _local_counter(TUnit::BYTES),
          _log_usage_if_zero(log_usage_if_zero),
          _auto_unregister(auto_unregister) {
    if (parent != NULL) _parent->add_child_tracker(this);
    Init();
}

MemTracker::MemTracker(RuntimeProfile* profile, int64_t byte_limit, const std::string& label, MemTracker* parent,
                       bool auto_unregister)
        : _type(NO_SET),
          _limit(byte_limit),
          _label(label),
          _parent(parent),
          _consumption(profile->AddHighWaterMarkCounter(COUNTER_NAME, TUnit::BYTES)),
          _local_counter(TUnit::BYTES),
          _log_usage_if_zero(true),
          _auto_unregister(auto_unregister) {
    if (parent != NULL) _parent->add_child_tracker(this);
    Init();
}

void MemTracker::Init() {
    DCHECK_GE(_limit, -1);
    // populate _all_trackers and _limit_trackers
    MemTracker* tracker = this;
    while (tracker != NULL) {
        _all_trackers.push_back(tracker);
        if (tracker->has_limit()) _limit_trackers.push_back(tracker);
        tracker = tracker->_parent;
    }
    DCHECK_GT(_all_trackers.size(), 0);
    DCHECK_EQ(_all_trackers[0], this);
}

// TODO chenhao , set MemTracker close state
void MemTracker::close() {}

void MemTracker::enable_reservation_reporting(const ReservationTrackerCounters& counters) {
    ReservationTrackerCounters* old = nullptr;
    ReservationTrackerCounters* new_counters = new ReservationTrackerCounters(counters);
    if (!_reservation_counters.compare_exchange_strong(old, new_counters, std::memory_order_relaxed)) {
        delete new_counters;
    }
}

MemTracker::~MemTracker() {
    DCHECK_EQ(0, consumption()) << CurrentThread::query_id_string();
    if (UNLIKELY(consumption() > 0)) {
        release(consumption());
    }
    delete _reservation_counters.load(std::memory_order_relaxed);
    if (_auto_unregister && parent()) {
        unregister_from_parent();
    }
}

// Calling this on the query tracker results in output like:
//
//  Query(4a4c81fedaed337d:4acadfda00000000) Limit=10.00 GB Total=508.28 MB Peak=508.45 MB
//    Fragment 4a4c81fedaed337d:4acadfda00000000: Total=8.00 KB Peak=8.00 KB
//      EXCHANGE_NODE (id=4): Total=0 Peak=0
//      DataStreamRecvr: Total=0 Peak=0
//    Block Manager: Limit=6.68 GB Total=394.00 MB Peak=394.00 MB
//    Fragment 4a4c81fedaed337d:4acadfda00000006: Total=233.72 MB Peak=242.24 MB
//      AGGREGATION_NODE (id=1): Total=139.21 MB Peak=139.84 MB
//      HDFS_SCAN_NODE (id=0): Total=93.94 MB Peak=102.24 MB
//      DataStreamSender (dst_id=2): Total=45.99 KB Peak=85.99 KB
//    Fragment 4a4c81fedaed337d:4acadfda00000003: Total=274.55 MB Peak=274.62 MB
//      AGGREGATION_NODE (id=3): Total=274.50 MB Peak=274.50 MB
//      EXCHANGE_NODE (id=2): Total=0 Peak=0
//      DataStreamRecvr: Total=45.91 KB Peak=684.07 KB
//      DataStreamSender (dst_id=4): Total=680.00 B Peak=680.00 B
//
// If 'reservation_metrics_' are set, we ge a more granular breakdown:
//   TrackerName: Limit=5.00 MB BufferPoolUsed/Reservation=0/5.00 MB OtherMemory=1.04 MB
//                Total=6.04 MB Peak=6.45 MB
//
std::string MemTracker::LogUsage(int max_recursive_depth, const std::string& prefix,
                                 int64_t* logged_consumption) const {
    int64_t curr_consumption = consumption();
    int64_t peak_consumption = _consumption->value();
    if (logged_consumption != nullptr) *logged_consumption = curr_consumption;

    if (!_log_usage_if_zero && curr_consumption == 0) return "";

    std::stringstream ss;
    ss << prefix << _label << ":";
    //if (CheckLimitExceeded()) ss << " memory limit exceeded.";
    if (limit_exceeded()) ss << " memory limit exceeded.";
    if (_limit > 0) ss << " Limit=" << PrettyPrinter::print(_limit, TUnit::BYTES);

    ReservationTrackerCounters* reservation_counters = _reservation_counters.load(std::memory_order_relaxed);
    if (reservation_counters != nullptr) {
        int64_t reservation = reservation_counters->peak_reservation->current_value();
        int64_t used_reservation = reservation_counters->peak_used_reservation->current_value();
        int64_t reservation_limit = 0;
        //TODO chenhao, reservation_limit is null when ReservationTracker
        // does't have reservation limit
        if (reservation_counters->reservation_limit != nullptr) {
            reservation_limit = reservation_counters->reservation_limit->value();
        }
        ss << " BufferPoolUsed/Reservation=" << PrettyPrinter::print(used_reservation, TUnit::BYTES) << "/"
           << PrettyPrinter::print(reservation, TUnit::BYTES);
        if (reservation_limit != std::numeric_limits<int64_t>::max()) {
            ss << " BufferPoolLimit=" << PrettyPrinter::print(reservation_limit, TUnit::BYTES);
        }
        ss << " OtherMemory=" << PrettyPrinter::print(curr_consumption - reservation, TUnit::BYTES);
    }
    ss << " Total=" << PrettyPrinter::print(curr_consumption, TUnit::BYTES)
       << " Peak=" << PrettyPrinter::print(peak_consumption, TUnit::BYTES);

    // This call does not need the children, so return early.
    if (max_recursive_depth == 0) return ss.str();

    std::string new_prefix = strings::Substitute("  $0", prefix);
    int64_t child_consumption;
    std::string child_trackers_usage;
    {
        std::lock_guard<std::mutex> l(_child_trackers_lock);
        child_trackers_usage = LogUsage(max_recursive_depth - 1, new_prefix, _child_trackers, &child_consumption);
    }
    if (!child_trackers_usage.empty()) ss << "\n" << child_trackers_usage;

    if (_consumption_metric != nullptr) {
        // Log the difference between the metric value and children as "untracked" memory so
        // that the values always add up. This value is not always completely accurate because
        // we did not necessarily get a consistent snapshot of the consumption values for all
        // children at a single moment in time, but is good enough for our purposes.
        int64_t untracked_bytes = curr_consumption - child_consumption;
        ss << "\n" << new_prefix << "Untracked Memory: Total=";
        ss << "\n" << new_prefix << "Untracked Memory: Total=" << PrettyPrinter::print(untracked_bytes, TUnit::BYTES);
    }

    return ss.str();
}

std::string MemTracker::LogUsage(int max_recursive_depth, const std::string& prefix,
                                 const std::list<MemTracker*>& trackers, int64_t* logged_consumption) {
    *logged_consumption = 0;
    std::vector<std::string> usage_strings;
    for (MemTracker* tracker : trackers) {
        int64_t tracker_consumption;
        std::string usage_string = tracker->LogUsage(max_recursive_depth, prefix, &tracker_consumption);
        if (!usage_string.empty()) usage_strings.push_back(usage_string);
        *logged_consumption += tracker_consumption;
    }
    return boost::join(usage_strings, "\n");
}

Status MemTracker::MemLimitExceeded(RuntimeState* state, const std::string& details, int64_t failed_allocation_size) {
    DCHECK_GE(failed_allocation_size, 0);
    std::stringstream ss;
    if (details.size() != 0) ss << details << std::endl;
    if (failed_allocation_size != 0) {
        ss << label() << " could not allocate " << PrettyPrinter::print(failed_allocation_size, TUnit::BYTES)
           << " without exceeding limit." << std::endl;
    }
    //ss << "Error occurred on backend " << GetBackendString();
    if (state != nullptr) ss << " by fragment " << state->fragment_instance_id();
    ss << std::endl;
    ExecEnv* exec_env = ExecEnv::GetInstance();
    //ExecEnv* exec_env = nullptr;
    MemTracker* process_tracker = exec_env->process_mem_tracker();
    const int64_t process_capacity = process_tracker->spare_capacity();
    ss << "Memory left in process limit: " << PrettyPrinter::print(process_capacity, TUnit::BYTES) << std::endl;

    // Choose which tracker to log the usage of. Default to the process tracker so we can
    // get the full view of memory consumption.
    // FIXME(cmy): call LogUsage() lead to crash here, fix it later
    // MemTracker* tracker_to_log = process_tracker;
    // if (state != nullptr && state->query_mem_tracker()->has_limit()) {
    //     MemTracker* query_tracker = state->query_mem_tracker();
    //     const int64_t query_capacity = query_tracker->limit() - query_tracker->consumption();
    //     ss << "Memory left in query limit: "
    //         << PrettyPrinter::print(query_capacity, TUnit::BYTES) << std::endl;
    //     // Log the query tracker only if the query limit was closer to being exceeded.
    //     if (query_capacity < process_capacity) tracker_to_log = query_tracker;
    // }
    // ss << tracker_to_log->LogUsage();
    // Status status = Status::MemLimitExceeded(ss.str());
    LIMIT_EXCEEDED(this, state, ss.str());
}

bool MemTracker::GcMemory(int64_t max_consumption) {
    if (max_consumption < 0) return true;
    std::lock_guard<std::mutex> l(_gc_lock);
    if (_consumption_metric != NULL) RefreshConsumptionFromMetric();
    int64_t pre_gc_consumption = consumption();
    // Check if someone gc'd before us
    if (pre_gc_consumption < max_consumption) return false;
    if (_num_gcs_metric != NULL) _num_gcs_metric->increment(1);

    int64_t curr_consumption = pre_gc_consumption;
    // Try to free up some memory
    for (int i = 0; i < _gc_functions.size(); ++i) {
        // Try to free up the amount we are over plus some extra so that we don't have to
        // immediately GC again. Don't free all the memory since that can be unnecessarily
        // expensive.
        const int64_t EXTRA_BYTES_TO_FREE = 512L * 1024L * 1024L;
        int64_t bytes_to_free = curr_consumption - max_consumption + EXTRA_BYTES_TO_FREE;
        _gc_functions[i](bytes_to_free);
        if (_consumption_metric != NULL) RefreshConsumptionFromMetric();
        curr_consumption = consumption();
        if (max_consumption - curr_consumption <= EXTRA_BYTES_TO_FREE) break;
    }

    if (_bytes_freed_by_last_gc_metric != NULL) {
        _bytes_freed_by_last_gc_metric->set_value(pre_gc_consumption - curr_consumption);
    }
    return curr_consumption > max_consumption;
}

} // end namespace starrocks
