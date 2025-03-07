// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/tablet_meta_manager.h

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

#ifndef STARROCKS_BE_SRC_OLAP_TABLET_META_MANAGER_H
#define STARROCKS_BE_SRC_OLAP_TABLET_META_MANAGER_H

#include <string>

#include "storage/data_dir.h"
#include "storage/olap_define.h"
#include "storage/olap_meta.h"
#include "storage/tablet_meta.h"

namespace starrocks {

class DelVector;
using DelVectorPtr = std::shared_ptr<DelVector>;
class EditVersion;
class EditVersionMetaPB;
class RowsetMetaPB;
class TabletMetaPB;

struct TabletMetaStats {
    TTabletId tablet_id = 0;
    TTableId table_id = 0;
    size_t meta_bytes = 0;
    // updatable related
    size_t log_size = 0;
    size_t log_bytes = 0;
    size_t rowset_size = 0;
    size_t rowset_bytes = 0;
    size_t pending_rowset_size = 0;
    size_t pending_rowset_bytes = 0;
    size_t delvec_size = 0;
    size_t delvec_bytes = 0;
};

struct MetaStoreStats {
    size_t tablet_size = 0;
    size_t tablet_bytes = 0;
    size_t rst_size = 0;
    size_t rst_bytes = 0;
    size_t update_tablet_size = 0;
    size_t update_tablet_bytes = 0;
    size_t log_size = 0;
    size_t log_bytes = 0;
    size_t delvec_size = 0;
    size_t delvec_bytes = 0;
    size_t rowset_size = 0;
    size_t rowset_bytes = 0;
    size_t pending_rowset_size = 0;
    size_t pending_rowset_bytes = 0;
    size_t total_size = 0;
    size_t total_bytes = 0;
    size_t error_size = 0;
    std::map<TTabletId, TabletMetaStats> tablets;
};

// Helper Class for managing tablet headers of one root path.
class TabletMetaManager {
public:
    static Status get_primary_meta(OlapMeta* meta, TTabletId tablet_id, TabletMetaPB& tablet_meta_pb,
                                   string* json_meta);

    static Status get_tablet_meta(DataDir* store, TTabletId tablet_id, TSchemaHash schema_hash,
                                  TabletMetaSharedPtr tablet_meta);

    static Status get_json_meta(DataDir* store, TTabletId tablet_id, TSchemaHash schema_hash, std::string* json_meta);

    static Status build_primary_meta(DataDir* store, rapidjson::Document& doc, rocksdb::ColumnFamilyHandle* cf,
                                     rocksdb::WriteBatch& batch);

    static Status save(DataDir* store, TTabletId tablet_id, TSchemaHash schema_hash, TabletMetaSharedPtr tablet_meta);

    static Status save(DataDir* store, TTabletId tablet_id, TSchemaHash schema_hash, const TabletMetaPB& meta_pb);

    static Status remove(DataDir* store, TTabletId tablet_id, TSchemaHash schema_hash);

    static Status traverse_headers(OlapMeta* meta, std::function<bool(long, long, const std::string&)> const& func);

    static Status load_json_meta(DataDir* store, const std::string& meta_path);

    static Status get_json_meta(DataDir* store, TTabletId tablet_id, std::string* json_meta);

    static Status get_stats(DataDir* store, MetaStoreStats* stats, bool detail = false);

    static Status remove(DataDir* store, TTabletId tablet_id);

    //
    // Updatable tablet meta operations
    //

    // commit a rowset into tablet
    static Status rowset_commit(DataDir* store, TTabletId tablet_id, int64_t logid, EditVersionMetaPB* edit,
                                const RowsetMetaPB& rowset, const string& rowset_meta_key);

    using RowsetIterateFunc = std::function<bool(RowsetMetaSharedPtr rowset_meta)>;
    static Status rowset_iterate(DataDir* store, TTabletId tablet_id, const RowsetIterateFunc& func);

    // methods for operating pending commits
    static Status pending_rowset_commit(DataDir* store, TTabletId tablet_id, int64_t version,
                                        const RowsetMetaPB& rowset, const string& rowset_meta_key);

    using PendingRowsetIterateFunc = std::function<bool(int64_t version, const std::string_view& rowset_meta_data)>;
    static Status pending_rowset_iterate(DataDir* store, TTabletId tablet_id, const PendingRowsetIterateFunc& func);

    // On success, store a pointer to `RowsetMeta` in |*meta| and return OK status.
    // On failure, store a nullptr in |*meta| and return a non-OK status.
    // Return NotFound if the rowset does not exist.
    static Status rowset_get(DataDir* store, TTabletId tablet_id, uint32_t rowset_id, RowsetMetaSharedPtr* meta);

    // For updatable tablet's rowset only.
    // Remove rowset meta from |store|, leave tablet meta unchanged.
    // |rowset_id| is the value returned from `RowsetMeta::get_rowset_seg_id`.
    // |segments| is the number of segments in the rowset, i.e, `Rowset::num_segments`.
    // All delete vectors that associated with this rowset will be deleted too.
    static Status rowset_delete(DataDir* store, TTabletId tablet_id, uint32_t rowset_id, uint32_t segments);

    // update meta after state of a rowset commit is applied
    static Status apply_rowset_commit(DataDir* store, TTabletId tablet_id, int64_t logid, const EditVersion& version,
                                      std::vector<std::pair<uint32_t, DelVectorPtr>>& delvecs);

    // traverse all the op logs for a tablet
    static Status traverse_meta_logs(DataDir* store, TTabletId tablet_id,
                                     const std::function<bool(uint64_t, const TabletMetaLogPB&)>& func);

    // TODO: rename parameter |segment_id|, it's different from `Segment::id()`
    static Status set_del_vector(OlapMeta* meta, TTabletId tablet_id, uint32_t segment_id, const DelVector& delvec);

    // TODO: rename parameter |segment_id|, it's different from `Segment::id()`
    static Status get_del_vector(OlapMeta* meta, TTabletId tablet_id, uint32_t segment_id, int64_t version,
                                 DelVector* delvec, int64_t* latest_version);

    // The first element of pair is segment id, the second element of pair is version.
    using DeleteVectorList = std::vector<std::pair<uint32_t, int64_t>>;

    static StatusOr<DeleteVectorList> list_del_vector(OlapMeta* meta, TTabletId tablet_id, int64_t max_version);

    static Status delete_del_vector_range(OlapMeta* meta, TTabletId tablet_id, uint32_t segment_id,
                                          int64_t start_version, int64_t end_version);

    static Status put_rowset_meta(DataDir* store, WriteBatch* batch, TTabletId tablet_id,
                                  const RowsetMetaPB& rowset_meta);

    static Status put_del_vector(DataDir* store, WriteBatch* batch, TTabletId tablet_id, uint32_t segment_id,
                                 const DelVector& delvec);

    static Status put_tablet_meta(DataDir* store, WriteBatch* batch, const TabletMetaPB& tablet_meta);

    static Status delete_pending_rowset(DataDir* store, WriteBatch* batch, TTabletId tablet_id, int64_t version);

    // Unlike `rowset_delete`, this method will NOT clear delete vectors.
    static Status clear_rowset(DataDir* store, WriteBatch* batch, TTabletId tablet_id);

    static Status clear_pending_rowset(DataDir* store, WriteBatch* batch, TTabletId tablet_id);

    static Status clear_log(DataDir* store, WriteBatch* batch, TTabletId tablet_id);

    static Status clear_del_vector(DataDir* store, WriteBatch* batch, TTabletId tablet_id);

    static Status remove_tablet_meta(DataDir* store, WriteBatch* batch, TTabletId tablet_id, TSchemaHash schema_hash);
};

} // namespace starrocks

#endif // STARROCKS_BE_SRC_OLAP_TABLET_META_MANAGER_H
