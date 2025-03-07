// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/util/types.h

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

#include "common/compiler_util.h"

namespace starrocks {

// Because __int128 in memory is not aligned, but GCC7 will generate SSE instruction
// for __int128 load/store. This will cause segment fault.
struct PackedInt128 {
    PackedInt128() : value(0) {}

    PackedInt128(const __int128& value_) { value = value_; }

    PackedInt128& operator=(const __int128& value_) {
        DIAGNOSTIC_PUSH
        DIAGNOSTIC_IGNORE("-Warray-bounds")
        value = value_;
        DIAGNOSTIC_POP
        return *this;
    }
    PackedInt128& operator=(const PackedInt128& rhs) {
        value = rhs.value;
        return *this;
    }
    __int128 value;
} __attribute__((packed));

} // namespace starrocks
