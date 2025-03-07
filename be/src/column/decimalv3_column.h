// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include <runtime/decimalv3.h>

#include "column/column.h"
#include "column/fixed_length_column_base.h"
#include "util/decimal_types.h"
#include "util/mysql_row_buffer.h"

namespace starrocks::vectorized {

template <typename T>
class DecimalV3Column final : public ColumnFactory<FixedLengthColumnBase<T>, DecimalV3Column<DecimalType<T>>, Column> {
public:
    DecimalV3Column() = default;
    DecimalV3Column(int precision, int scale);
    DecimalV3Column(int precision, int scale, size_t num_rows);

    DecimalV3Column(DecimalV3Column const&) = default;
    DecimalV3Column& operator=(DecimalV3Column const&) = default;

    bool is_decimal() const override;
    bool is_numeric() const override;
    void set_precision(int precision);
    void set_scale(int scale);
    int precision() const;
    int scale() const;

    MutableColumnPtr clone_empty() const override;

    void put_mysql_row_buffer(MysqlRowBuffer* buf, size_t idx) const;
    std::string debug_item(uint32_t idx) const override;
    void crc32_hash(uint32_t* hash, uint16_t from, uint16_t to) const override;

private:
    int _precision;
    int _scale;
};

} // namespace starrocks::vectorized
