// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/decimalv2_value.h

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

#ifndef STARROCKS_BE_SRC_RUNTIME_DECIMALV2_VALUE_H
#define STARROCKS_BE_SRC_RUNTIME_DECIMALV2_VALUE_H

#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "common/logging.h"
#include "runtime/decimal_value.h"
#include "storage/decimal12.h"
#include "udf/udf.h"
#include "util/hash_util.hpp"

namespace starrocks {

typedef __int128 int128_t;
typedef unsigned __int128 uint128_t;

class DecimalV2Value {
public:
    friend DecimalV2Value operator+(const DecimalV2Value& v1, const DecimalV2Value& v2);
    friend DecimalV2Value operator-(const DecimalV2Value& v1, const DecimalV2Value& v2);
    friend DecimalV2Value operator*(const DecimalV2Value& v1, const DecimalV2Value& v2);
    friend DecimalV2Value operator/(const DecimalV2Value& v1, const DecimalV2Value& v2);
    friend std::istream& operator>>(std::istream& ism, DecimalV2Value& decimal_value);
    friend DecimalV2Value operator-(const DecimalV2Value& v);

    static const int32_t PRECISION = 27;
    static const int32_t SCALE = 9;
    static const uint32_t ONE_BILLION = 1000000000;
    static const int64_t MAX_INT_VALUE = 999999999999999999ll;
    static const int32_t MAX_FRAC_VALUE = 999999999l;
    static const int64_t MAX_INT64 = 9223372036854775807ll;

    static const int128_t MAX_DECIMAL_VALUE = static_cast<int128_t>(MAX_INT64) * ONE_BILLION + MAX_FRAC_VALUE;
    static const int128_t MIN_DECIMAL_VALUE = -MAX_DECIMAL_VALUE;

    DecimalV2Value() : _value(0) {}
    inline const int128_t& value() const { return _value; }
    inline int128_t& value() { return _value; }

    explicit DecimalV2Value(const std::string& decimal_str) { parse_from_str(decimal_str.c_str(), decimal_str.size()); }

    explicit DecimalV2Value(const decimal12_t& decimal12) { from_olap_decimal(decimal12.integer, decimal12.fraction); }

    // Construct from olap engine
    DecimalV2Value(int64_t int_value, int64_t frac_value) { from_olap_decimal(int_value, frac_value); }

    inline bool from_olap_decimal(int64_t int_value, int64_t frac_value) {
        bool success = true;
        bool is_negtive = (int_value < 0 || frac_value < 0);
        if (is_negtive) {
            int_value = std::abs(int_value);
            frac_value = std::abs(frac_value);
        }

        //if (int_value > MAX_INT_VALUE) {
        //    int_value = MAX_INT_VALUE;
        //    success = false;
        //}

        if (frac_value > MAX_FRAC_VALUE) {
            frac_value = MAX_FRAC_VALUE;
            success = false;
        }

        _value = static_cast<int128_t>(int_value) * ONE_BILLION + frac_value;
        if (is_negtive) _value = -_value;

        return success;
    }

    explicit DecimalV2Value(int128_t int_value) { _value = int_value; }

    void set_value(int128_t value) { _value = value; }

    DecimalV2Value& assign_from_float(const float float_value) {
        _value = static_cast<int128_t>(float_value * ONE_BILLION);
        return *this;
    }

    DecimalV2Value& assign_from_double(const double double_value) {
        _value = static_cast<int128_t>(double_value * ONE_BILLION);
        return *this;
    }

    // These cast functions are needed in expressions
    // Discard the scale part
    // ATTN: invoker must make sure no OVERFLOW
    operator int64_t() const { return static_cast<int64_t>(_value / ONE_BILLION); }

    operator int128_t() const { return static_cast<int128_t>(_value / ONE_BILLION); }

    operator bool() const { return _value != 0; }

    operator int8_t() const { return static_cast<char>(operator int64_t()); }

    operator int16_t() const { return static_cast<int16_t>(operator int64_t()); }

    operator int32_t() const { return static_cast<int32_t>(operator int64_t()); }

    operator size_t() const { return static_cast<size_t>(operator int64_t()); }

    operator float() const { return (float)operator double(); }

    operator double() const { return static_cast<double>(_value) / ONE_BILLION; }

    DecimalV2Value& operator+=(const DecimalV2Value& other);

    // To be Compatible with OLAP
    // ATTN: NO-OVERFLOW should be guaranteed.
    int64_t int_value() const { return operator int64_t(); }

    // To be Compatible with OLAP
    // NOTE: return a negative value if decimal is negative.
    // ATTN: the max length of fraction part in OLAP is 9, so the 'big digits' except the first one
    // will be truncated.
    int32_t frac_value() const { return static_cast<int64_t>(_value % ONE_BILLION); }

    bool operator==(const DecimalV2Value& other) const { return _value == other.value(); }

    bool operator!=(const DecimalV2Value& other) const { return _value != other.value(); }

    bool operator<=(const DecimalV2Value& other) const { return _value <= other.value(); }

    bool operator>=(const DecimalV2Value& other) const { return _value >= other.value(); }

    bool operator<(const DecimalV2Value& other) const { return _value < other.value(); }

    bool operator>(const DecimalV2Value& other) const { return _value > other.value(); }

    // change to maximum value for given precision and scale
    // precision/scale - see decimal_bin_size() below
    // to              - decimal where where the result will be stored
    void to_max_decimal(int precision, int frac);
    void to_min_decimal(int precision, int frac) {
        to_max_decimal(precision, frac);
        if (_value > 0) _value = -_value;
    }

    // The maximum of fraction part is "scale".
    // If the length of fraction part is less than "scale", '0' will be filled.
    std::string to_string(int scale) const;
    // Output actual "scale", remove ending zeroes.
    std::string to_string() const;

    // No padding zero.
    // The size of |buff| must not less than 64.
    // Returns the number of bytes copied to |buff|.
    int to_string(char* buff) const;

    // Convert string to decimal
    // @param from - value to convert. Doesn't have to be \0 terminated!
    //               will stop at the fist non-digit char(nor '.' 'e' 'E'),
    //               or reaches the length
    // @param length - maximum lengnth
    // @return error number.
    //
    // E_DEC_OK/E_DEC_TRUNCATED/E_DEC_OVERFLOW/E_DEC_BAD_NUM/E_DEC_OOM
    // In case of E_DEC_FATAL_ERROR *to is set to decimal zero
    // (to make error handling easier)
    //
    // e.g. "1.2"  ".2"  "1.2e-3"  "1.2e3"
    int parse_from_str(const char* decimal_str, int32_t length);

    std::string get_debug_info() const { return to_string(); }

    static DecimalV2Value get_min_decimal() { return DecimalV2Value(-MAX_INT_VALUE, MAX_FRAC_VALUE); }

    static DecimalV2Value get_max_decimal() { return DecimalV2Value(MAX_INT_VALUE, MAX_FRAC_VALUE); }

    static DecimalV2Value from_decimal_val(const DecimalV2Val& val) { return DecimalV2Value(val.value()); }

    void to_decimal_val(DecimalV2Val* value) const { value->val = _value; }

    // set DecimalV2Value to zero
    void set_to_zero() { _value = 0; }

    void to_abs_value() {
        if (_value < 0) _value = -_value;
    }

    uint32_t hash(uint32_t seed) const { return HashUtil::hash(&_value, sizeof(_value), seed); }

    int32_t precision() const { return PRECISION; }

    int32_t scale() const { return SCALE; }

    bool greater_than_scale(int scale);

    int round(DecimalV2Value* to, int scale, DecimalRoundMode mode);

    inline static int128_t get_scale_base(int scale) {
        static const int128_t values[] = {static_cast<int128_t>(1ll),
                                          static_cast<int128_t>(10ll),
                                          static_cast<int128_t>(100ll),
                                          static_cast<int128_t>(1000ll),
                                          static_cast<int128_t>(10000ll),
                                          static_cast<int128_t>(100000ll),
                                          static_cast<int128_t>(1000000ll),
                                          static_cast<int128_t>(10000000ll),
                                          static_cast<int128_t>(100000000ll),
                                          static_cast<int128_t>(1000000000ll),
                                          static_cast<int128_t>(10000000000ll),
                                          static_cast<int128_t>(100000000000ll),
                                          static_cast<int128_t>(1000000000000ll),
                                          static_cast<int128_t>(10000000000000ll),
                                          static_cast<int128_t>(100000000000000ll),
                                          static_cast<int128_t>(1000000000000000ll),
                                          static_cast<int128_t>(10000000000000000ll),
                                          static_cast<int128_t>(100000000000000000ll),
                                          static_cast<int128_t>(1000000000000000000ll),
                                          static_cast<int128_t>(1000000000000000000ll) * 10ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 1000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 10000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 1000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 10000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 1000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 10000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 1000000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 10000000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 1000000000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 10000000000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000000000000000ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000000000000000ll * 10ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000000000000000ll * 100ll,
                                          static_cast<int128_t>(1000000000000000000ll) * 100000000000000000ll * 1000ll};
        if (scale >= 0 && scale < 38) return values[scale];
        return -1; // Overflow
    }

    bool is_zero() const { return _value == 0; }

public:
    static DecimalV2Value ZERO;
    static DecimalV2Value ONE;

private:
    int128_t _value;
};

DecimalV2Value operator+(const DecimalV2Value& v1, const DecimalV2Value& v2);
DecimalV2Value operator-(const DecimalV2Value& v1, const DecimalV2Value& v2);
DecimalV2Value operator*(const DecimalV2Value& v1, const DecimalV2Value& v2);
DecimalV2Value operator/(const DecimalV2Value& v1, const DecimalV2Value& v2);
DecimalV2Value operator%(const DecimalV2Value& v1, const DecimalV2Value& v2);

DecimalV2Value operator-(const DecimalV2Value& v);

std::ostream& operator<<(std::ostream& os, DecimalV2Value const& decimal_value);
std::istream& operator>>(std::istream& ism, DecimalV2Value& decimal_value);

std::size_t hash_value(DecimalV2Value const& value);

} // end namespace starrocks

namespace std {
template <>
struct hash<starrocks::DecimalV2Value> {
    size_t operator()(const starrocks::DecimalV2Value& v) const { return starrocks::hash_value(v); }
};
} // namespace std

#endif // STARROCKS_BE_SRC_RUNTIME_DECIMALV2_VALUE_H
