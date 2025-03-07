// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/tablet.h

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

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/statusor.h"
#include "gen_cpp/AgentService_types.h"
#include "gen_cpp/MasterService_types.h"
#include "gen_cpp/olap_file.pb.h"
#include "storage/base_tablet.h"
#include "storage/data_dir.h"
#include "storage/olap_define.h"
#include "storage/rowset/rowset.h"
#include "storage/rowset/rowset_reader.h"
#include "storage/tablet_meta.h"
#include "storage/tuple.h"
#include "storage/utils.h"
#include "storage/version_graph.h"
#include "util/once.h"

namespace starrocks {

class DataDir;
class Tablet;
class TabletMeta;
class TabletUpdateState;
class TabletUpdates;

using TabletSharedPtr = std::shared_ptr<Tablet>;

namespace vectorized {
class ChunkIterator;
class RowsetReadOptions;
} // namespace vectorized

using ChunkIteratorPtr = std::shared_ptr<vectorized::ChunkIterator>;

class Tablet : public BaseTablet {
public:
    static TabletSharedPtr create_tablet_from_meta(MemTracker* mem_tracker, TabletMetaSharedPtr tablet_meta,
                                                   DataDir* data_dir = nullptr);

    Tablet(MemTracker* mem_tracker, TabletMetaSharedPtr tablet_meta, DataDir* data_dir);

    ~Tablet();

    OLAPStatus init();
    inline bool init_succeeded();

    bool is_used();

    void register_tablet_into_dir();
    void deregister_tablet_from_dir();

    void save_meta();
    // Used in clone task, to update local meta when finishing a clone job
    OLAPStatus revise_tablet_meta(const std::vector<RowsetMetaSharedPtr>& rowsets_to_clone,
                                  const std::vector<Version>& versions_to_delete);

    inline const int64_t cumulative_layer_point() const;
    inline void set_cumulative_layer_point(int64_t new_point);

    size_t tablet_footprint(); // disk space occupied by tablet
    size_t num_rows();
    int version_count() const;
    Version max_version() const;

    // propreties encapsulated in TabletSchema
    inline KeysType keys_type() const;
    inline size_t num_columns() const;
    inline size_t num_key_columns() const;
    inline size_t num_short_key_columns() const;
    inline size_t num_rows_per_row_block() const;
    inline CompressKind compress_kind() const;
    inline size_t next_unique_id() const;
    inline size_t row_size() const;
    inline size_t field_index(const string& field_name) const;

    // operation in rowsets
    OLAPStatus add_rowset(RowsetSharedPtr rowset, bool need_persist = true);
    void modify_rowsets(const vector<RowsetSharedPtr>& to_add, const vector<RowsetSharedPtr>& to_delete);

    // _rs_version_map and _inc_rs_version_map should be protected by _meta_lock
    // The caller must call hold _meta_lock when call this two function.
    const RowsetSharedPtr get_rowset_by_version(const Version& version) const;
    const RowsetSharedPtr get_inc_rowset_by_version(const Version& version) const;

    const RowsetSharedPtr rowset_with_max_version() const;

    OLAPStatus add_inc_rowset(const RowsetSharedPtr& rowset);
    void delete_expired_inc_rowsets();
    /// Delete stale rowset by timing. This delete policy uses now() munis
    /// config::tablet_rowset_expired_stale_sweep_time_sec to compute the deadline of expired rowset
    /// to delete.  When rowset is deleted, it will be added to StorageEngine unused map and record
    /// need to delete flag.
    void delete_expired_stale_rowset();

    OLAPStatus capture_consistent_versions(const Version& spec_version, vector<Version>* version_path) const;
    OLAPStatus check_version_integrity(const Version& version);
    bool check_version_exist(const Version& version) const;
    void list_versions(std::vector<Version>* versions) const;

    OLAPStatus capture_consistent_rowsets(const Version& spec_version, vector<RowsetSharedPtr>* rowsets) const;
    OLAPStatus capture_rs_readers(const Version& spec_version, vector<RowsetReaderSharedPtr>* rs_readers) const;
    OLAPStatus capture_rs_readers(const vector<Version>& version_path, vector<RowsetReaderSharedPtr>* rs_readers) const;

    using IteratorList = std::vector<ChunkIteratorPtr>;

    // Get the segment iterators for the specified version |spec_version|.
    StatusOr<IteratorList> capture_segment_iterators(const Version& spec_version, const vectorized::Schema& schema,
                                                     const vectorized::RowsetReadOptions& options) const;

    const DelPredicateArray& delete_predicates() const { return _tablet_meta->delete_predicates(); }
    void add_delete_predicate(const DeletePredicatePB& delete_predicate, int64_t version);
    bool version_for_delete_predicate(const Version& version);

    // message for alter task
    AlterTabletTaskSharedPtr alter_task();
    void add_alter_task(int64_t related_tablet_id, int32_t related_schema_hash,
                        const vector<Version>& versions_to_alter, const AlterTabletType alter_type);
    void delete_alter_task();
    OLAPStatus set_alter_state(AlterTabletState state);

    // meta lock
    inline void obtain_header_rdlock() { _meta_lock.lock_shared(); }
    inline void obtain_header_wrlock() { _meta_lock.lock(); }
    inline void release_header_lock() { _meta_lock.unlock(); }
    inline std::shared_mutex& get_header_lock() { return _meta_lock; }

    // ingest lock
    inline void obtain_push_lock() { _ingest_lock.lock(); }
    inline void release_push_lock() { _ingest_lock.unlock(); }
    inline std::mutex& get_push_lock() { return _ingest_lock; }

    // base lock
    inline void obtain_base_compaction_lock() { _base_lock.lock(); }
    inline void release_base_compaction_lock() { _base_lock.unlock(); }
    inline std::mutex& get_base_lock() { return _base_lock; }

    // cumulative lock
    inline void obtain_cumulative_lock() { _cumulative_lock.lock(); }
    inline void release_cumulative_lock() { _cumulative_lock.unlock(); }
    inline std::mutex& get_cumulative_lock() { return _cumulative_lock; }

    inline std::shared_mutex& get_migration_lock() { return _migration_lock; }

    // operation for compaction
    bool can_do_compaction();
    const uint32_t calc_cumulative_compaction_score() const;
    const uint32_t calc_base_compaction_score() const;
    static void compute_version_hash_from_rowsets(const std::vector<RowsetSharedPtr>& rowsets,
                                                  VersionHash* version_hash);

    // operation for clone
    void calc_missed_versions(int64_t spec_version, vector<Version>* missed_versions);
    void calc_missed_versions_unlocked(int64_t spec_version, vector<Version>* missed_versions) const;

    // This function to find max continuous version from the beginning.
    // For example: If there are 1, 2, 3, 5, 6, 7 versions belongs tablet, then 3 is target.
    Version max_continuous_version_from_beginning() const;

    // operation for query
    OLAPStatus split_range(const OlapTuple& start_key_strings, const OlapTuple& end_key_strings,
                           uint64_t request_block_row_count, vector<OlapTuple>* ranges);

    int64_t last_cumu_compaction_failure_time() { return _last_cumu_compaction_failure_millis; }
    void set_last_cumu_compaction_failure_time(int64_t millis) { _last_cumu_compaction_failure_millis = millis; }

    int64_t last_base_compaction_failure_time() { return _last_base_compaction_failure_millis; }
    void set_last_base_compaction_failure_time(int64_t millis) { _last_base_compaction_failure_millis = millis; }

    int64_t last_cumu_compaction_success_time() { return _last_cumu_compaction_success_millis; }
    void set_last_cumu_compaction_success_time(int64_t millis) { _last_cumu_compaction_success_millis = millis; }

    int64_t last_base_compaction_success_time() { return _last_base_compaction_success_millis; }
    void set_last_base_compaction_success_time(int64_t millis) { _last_base_compaction_success_millis = millis; }

    void delete_all_files();

    bool check_rowset_id(const RowsetId& rowset_id);

    OLAPStatus set_partition_id(int64_t partition_id);

    TabletInfo get_tablet_info() const;

    void pick_candicate_rowsets_to_cumulative_compaction(int64_t skip_window_sec,
                                                         std::vector<RowsetSharedPtr>* candidate_rowsets);
    void pick_candicate_rowsets_to_base_compaction(std::vector<RowsetSharedPtr>* candidate_rowsets);

    void calculate_cumulative_point();
    // TODO(ygl):
    inline bool is_primary_replica() { return false; }

    // TODO(ygl):
    // eco mode means power saving in new energy car
    // eco mode also means save money in starrocks
    inline bool in_eco_mode() { return false; }

    void do_tablet_meta_checkpoint();

    bool rowset_meta_is_useful(RowsetMetaSharedPtr rowset_meta);

    void build_tablet_report_info(TTabletInfo* tablet_info);

    void generate_tablet_meta_copy(TabletMetaSharedPtr new_tablet_meta) const;
    // caller should hold the _meta_lock before calling this method
    void generate_tablet_meta_copy_unlocked(TabletMetaSharedPtr new_tablet_meta) const;

    // return a json string to show the compaction status of this tablet
    void get_compaction_status(std::string* json_result);

    // updatable tablet specific operations
    TabletUpdates* updates() { return _updates.get(); }
    Status rowset_commit(int64_t version, const RowsetSharedPtr& rowset);

protected:
    void on_shutdown() override;

private:
    OLAPStatus _init_once_action();
    void _print_missed_versions(const std::vector<Version>& missed_versions) const;
    bool _contains_rowset(const RowsetId rowset_id);
    OLAPStatus _contains_version(const Version& version);
    Version _max_continuous_version_from_beginning_unlocked() const;
    RowsetSharedPtr _rowset_with_largest_size();
    void _delete_inc_rowset_by_version(const Version& version, const VersionHash& version_hash);
    /// Delete stale rowset by version. This method not only delete the version in expired rowset map,
    /// but also delete the version in rowset meta vector.
    void _delete_stale_rowset_by_version(const Version& version);
    OLAPStatus _capture_consistent_rowsets_unlocked(const vector<Version>& version_path,
                                                    vector<RowsetSharedPtr>* rowsets) const;

private:
    friend class TabletUpdates;
    static const int64_t kInvalidCumulativePoint = -1;

    TimestampedVersionTracker _timestamped_version_tracker;

    StarRocksCallOnce<OLAPStatus> _init_once;
    // meta store lock is used for prevent 2 threads do checkpoint concurrently
    // it will be used in econ-mode in the future
    std::shared_mutex _meta_store_lock;
    std::mutex _ingest_lock;
    std::mutex _base_lock;
    std::mutex _cumulative_lock;
    std::shared_mutex _migration_lock;

    // TODO(lingbin): There is a _meta_lock TabletMeta too, there should be a comment to
    // explain how these two locks work together.
    mutable std::shared_mutex _meta_lock;
    // A new load job will produce a new rowset, which will be inserted into both _rs_version_map
    // and _inc_rs_version_map. Only the most recent rowsets are kept in _inc_rs_version_map to
    // reduce the amount of data that needs to be copied during the clone task.
    // NOTE: Not all incremental-rowsets are in _rs_version_map. Because after some rowsets
    // are compacted, they will be remove from _rs_version_map, but it may not be deleted from
    // _inc_rs_version_map.
    // Which rowsets should be deleted from _inc_rs_version_map is affected by
    // inc_rowset_expired_sec conf. In addition, the deletion is triggered periodically,
    // So at a certain time point (such as just after a base compaction), some rowsets in
    // _inc_rs_version_map may do not exist in _rs_version_map.
    std::unordered_map<Version, RowsetSharedPtr, HashOfVersion> _rs_version_map;
    std::unordered_map<Version, RowsetSharedPtr, HashOfVersion> _inc_rs_version_map;
    // This variable _stale_rs_version_map is used to record these rowsets which are be compacted.
    // These _stale rowsets are been removed when rowsets' pathVersion is expired,
    // this policy is judged and computed by TimestampedVersionTracker.
    std::unordered_map<Version, RowsetSharedPtr, HashOfVersion> _stale_rs_version_map;

    // States used for updatable tablets only
    std::unique_ptr<TabletUpdates> _updates;

    // if this tablet is broken, set to true. default is false
    // timestamp of last cumu compaction failure
    std::atomic<int64_t> _last_cumu_compaction_failure_millis{0};
    // timestamp of last base compaction failure
    std::atomic<int64_t> _last_base_compaction_failure_millis{0};
    // timestamp of last cumu compaction success
    std::atomic<int64_t> _last_cumu_compaction_success_millis{0};
    // timestamp of last base compaction success
    std::atomic<int64_t> _last_base_compaction_success_millis{0};

    std::atomic<int64_t> _cumulative_point{0};
    std::atomic<int32_t> _newly_created_rowset_num{0};
    std::atomic<int64_t> _last_checkpoint_time{0};

    DISALLOW_COPY_AND_ASSIGN(Tablet);
};

inline bool Tablet::init_succeeded() {
    return _init_once.has_called() && _init_once.stored_result() == OLAP_SUCCESS;
}

inline bool Tablet::is_used() {
    return _data_dir->is_used();
}

inline void Tablet::register_tablet_into_dir() {
    _data_dir->register_tablet(this);
}

inline void Tablet::deregister_tablet_from_dir() {
    _data_dir->deregister_tablet(this);
}

inline const int64_t Tablet::cumulative_layer_point() const {
    return _cumulative_point;
}

inline void Tablet::set_cumulative_layer_point(int64_t new_point) {
    _cumulative_point = new_point;
}

inline KeysType Tablet::keys_type() const {
    return _tablet_meta->tablet_schema().keys_type();
}

inline size_t Tablet::num_columns() const {
    return _tablet_meta->tablet_schema().num_columns();
}

inline size_t Tablet::num_key_columns() const {
    return _tablet_meta->tablet_schema().num_key_columns();
}

inline size_t Tablet::num_short_key_columns() const {
    return _tablet_meta->tablet_schema().num_short_key_columns();
}

inline size_t Tablet::num_rows_per_row_block() const {
    return _tablet_meta->tablet_schema().num_rows_per_row_block();
}

inline CompressKind Tablet::compress_kind() const {
    return _tablet_meta->tablet_schema().compress_kind();
}

inline size_t Tablet::next_unique_id() const {
    return _tablet_meta->tablet_schema().next_column_unique_id();
}

inline size_t Tablet::field_index(const string& field_name) const {
    return _tablet_meta->tablet_schema().field_index(field_name);
}

inline size_t Tablet::row_size() const {
    return _tablet_meta->tablet_schema().row_size();
}

} // namespace starrocks
