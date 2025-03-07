// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/exec/schema_scanner/schema_helper.h

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

#include "common/status.h"
#include "gen_cpp/FrontendService_types.h"

namespace starrocks {

// this class is a helper for getting schema info from FE
class SchemaHelper {
public:
    static Status get_db_names(const std::string& ip, const int32_t port, const TGetDbsParams& db_params,
                               TGetDbsResult* db_result);

    static Status get_table_names(const std::string& ip, const int32_t port, const TGetTablesParams& table_params,
                                  TGetTablesResult* table_result);

    static Status list_table_status(const std::string& ip, const int32_t port, const TGetTablesParams& table_params,
                                    TListTableStatusResult* table_result);

    static Status describe_table(const std::string& ip, const int32_t port, const TDescribeTableParams& desc_params,
                                 TDescribeTableResult* desc_result);

    static Status show_varialbes(const std::string& ip, const int32_t port, const TShowVariableRequest& var_params,
                                 TShowVariableResult* var_result);

    static std::string extract_db_name(const std::string& full_name);

    static Status get_user_privs(const std::string& ip, const int32_t port, const TGetUserPrivsParams& var_params,
                                 TGetUserPrivsResult* var_result);

    static Status get_db_privs(const std::string& ip, const int32_t port, const TGetDBPrivsParams& var_params,
                               TGetDBPrivsResult* var_result);

    static Status get_table_privs(const std::string& ip, const int32_t port, const TGetTablePrivsParams& var_params,
                                  TGetTablePrivsResult* var_result);
};

} // namespace starrocks
