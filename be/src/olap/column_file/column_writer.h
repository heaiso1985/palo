// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef BDG_PALO_BE_SRC_OLAP_COLUMN_FILE_COLUMN_WRITER_H
#define BDG_PALO_BE_SRC_OLAP_COLUMN_FILE_COLUMN_WRITER_H

#include <gen_cpp/column_data_file.pb.h>

#include <map>

#include "olap/column_file/bloom_filter.hpp"
#include "olap/column_file/bloom_filter_writer.h"
#include "olap/column_file/out_stream.h"
#include "olap/column_file/stream_index_writer.h"
#include "olap/field.h"
#include "olap/olap_common.h"
#include "olap/olap_define.h"
#include "olap/row_cursor.h"

namespace palo {
namespace column_file {

class OutStream;
class OutStreamFactory;
class ColumnStatistics;
class BitFieldWriter;
class RunLengthByteWriter;
class RunLengthIntegerWriter;

class ColumnWriter {
public:
    // 创建一个ColumnWriter, 创建后的对象生命期由调用者所有
    // 即调用者负责调用delete析构ColumnWriter
    // Args:
    //    column_id: 创建的列在columns中的位置
    //    columns: 表的所有的列信息
    //    stream_factory: 用于创建输出流的工厂对象, 该对象的生命期由调用者所有
    static ColumnWriter* create(
            uint32_t column_id,
            const std::vector<FieldInfo>& columns,
            OutStreamFactory* stream_factory,
            size_t num_rows_per_row_block,
            double bf_fpp);
    virtual ~ColumnWriter();
    virtual OLAPStatus init();
    virtual OLAPStatus write(RowCursor* row_cursor);
    // 将之前记录的block位置信息与当前的统计信息写入到一个新的索引项中
    OLAPStatus create_row_index_entry();
    // 估算当前缓存的内存大小, 不包括已经输出到OutStream的内存
    virtual uint64_t estimate_buffered_memory();
    virtual OLAPStatus flush();
    // 结束Segment, flush stream并更新header:
    //   * column_unique_id
    //   * column_type
    //   * column_encoding
    //   * column_statistics
    virtual OLAPStatus finalize(ColumnDataHeaderMessage* header);
    virtual void save_encoding(ColumnEncodingMessage* encoding);
    uint32_t column_id() const {
        return _column_id;
    }

    uint32_t unique_column_id() const {
        return _field_info.unique_id;
    }

    virtual void get_bloom_filter_info(bool* has_bf_column,
                                       uint32_t* bf_hash_function_num,
                                       uint32_t* bf_bit_num);

    ColumnStatistics* segment_statistics() {
        return &_segment_statistics;
    }

    ColumnStatistics* block_statistics() {
        return &_block_statistics;
    }
protected:
    ColumnWriter(uint32_t column_id,
            OutStreamFactory* stream_factory,
            const FieldInfo& field_info,
            size_t num_rows_per_row_block,
            double bf_fpp);

    OutStreamFactory* stream_factory() {
        return _stream_factory;
    }
    PositionEntryWriter* index_entry() {
        return &_index_entry;
    }
    StreamIndexWriter* index() {
        return &_index;
    }
    // 记录当前Stream的位置,用于生成索引项
    virtual void record_position();

protected:
    ColumnStatistics _block_statistics;
    ColumnStatistics _segment_statistics;

private:
    void _remove_is_present_positions();

    bool is_bf_column() {
        return _field_info.is_bf_column;
    }

    uint32_t _column_id;
    const FieldInfo& _field_info;
    OutStreamFactory* _stream_factory; // 该对象由外部调用者所有
    std::vector<ColumnWriter*> _sub_writers;   // 保存子列的writer
    PositionEntryWriter _index_entry;
    StreamIndexWriter _index;
    BitFieldWriter* _is_present;       // 对于允许NULL的列记录NULL Bits
    OutStream* _is_present_stream;
    OutStream* _index_stream;          // 注意对象的所有权是_stream_factory
    bool _is_found_nulls;
    BloomFilter* _bf;
    BloomFilterIndexWriter _bf_index;
    OutStream* _bf_index_stream;
    size_t _num_rows_per_row_block;
    double _bf_fpp;

    DISALLOW_COPY_AND_ASSIGN(ColumnWriter);
};

class ByteColumnWriter : public ColumnWriter {
public:
    ByteColumnWriter(uint32_t column_id, OutStreamFactory* stream_factory,
                     const FieldInfo& field_info, size_t num_rows_per_row_block,
                     double bf_fpp);
    virtual ~ByteColumnWriter();
    virtual OLAPStatus init();
    virtual OLAPStatus write(RowCursor* row_cursor);
    virtual OLAPStatus finalize(ColumnDataHeaderMessage* header);
    virtual void record_position();
    virtual OLAPStatus flush() {
        return OLAP_SUCCESS;
    }
private:
    RunLengthByteWriter* _writer;

    DISALLOW_COPY_AND_ASSIGN(ByteColumnWriter);
};

// 对于SHORT/INT/LONG类型的数据，统一使用int64作为存储的数据
class IntegerColumnWriter {
public:
    IntegerColumnWriter(
            uint32_t column_id,
            uint32_t unique_column_id,
            OutStreamFactory* stream_factory,
            bool is_singed);
    ~IntegerColumnWriter();
    OLAPStatus init();
    OLAPStatus write(int64_t data);
    OLAPStatus finalize(ColumnDataHeaderMessage* header);
    void record_position(PositionEntryWriter* index_entry);
    OLAPStatus flush();

private:
    uint32_t _column_id;
    uint32_t _unique_column_id;
    OutStreamFactory* _stream_factory;
    RunLengthIntegerWriter* _writer;
    bool _is_signed;

    DISALLOW_COPY_AND_ASSIGN(IntegerColumnWriter);
};

template<class T, bool is_singed>
class IntegerColumnWriterWrapper : public ColumnWriter {
public:
    IntegerColumnWriterWrapper(
            uint32_t column_id,
            OutStreamFactory* stream_factory,
            const FieldInfo& field_info,
            size_t num_rows_per_row_block,
            double bf_fpp) : 
            ColumnWriter(column_id, stream_factory, field_info, num_rows_per_row_block, bf_fpp),
            _writer(column_id, field_info.unique_id, stream_factory, is_singed) {}

    virtual ~IntegerColumnWriterWrapper() {}

    virtual OLAPStatus init() {
        OLAPStatus res = OLAP_SUCCESS;

        res = ColumnWriter::init();
        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("fail to init ColumnWriter. [res=%d]", res);
            return res;
        }

        res = _writer.init();
        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("fail to init IntegerColumnWriter. [res=%d]", res);
            return res;
        }

        record_position();
        return OLAP_SUCCESS;
    }

    virtual OLAPStatus write(RowCursor* row_cursor) {
        OLAPStatus res = ColumnWriter::write(row_cursor);

        if (OLAP_UNLIKELY(OLAP_SUCCESS != res)) {
            OLAP_LOG_WARNING("fail to write ColumnWriter. [res=%d]", res);
            return res;
        }

        bool is_null = row_cursor->is_null(column_id());
        const Field* field = row_cursor->get_field_by_index(column_id());
        char* buf = field->get_field_ptr(row_cursor->get_buf());
        _block_statistics.add(buf);
        if (!is_null) {
            T value = *reinterpret_cast<T*>(buf + 1);
            return _writer.write(static_cast<int64_t>(value));
        }

        return OLAP_SUCCESS;
    }

    virtual OLAPStatus flush() {
        OLAPStatus res = ColumnWriter::flush();

        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("fail to flush column_writer. [res=%d]", res);
            return res;
        }

        res = _writer.flush();

        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("fail to flush integer_writer. [res=%d]", res);
            return res;
        }

        return res;
    }

    virtual OLAPStatus finalize(ColumnDataHeaderMessage* header) {
        OLAPStatus res = ColumnWriter::finalize(header);

        if (OLAP_UNLIKELY(OLAP_SUCCESS != res)) {
            OLAP_LOG_WARNING("fail to finalize ColumnWriter. [res=%d]", res);
            return res;
        }

        return _writer.finalize(header);
    }

    virtual void record_position() {
        ColumnWriter::record_position();
        _writer.record_position(index_entry());
    }
private:
    IntegerColumnWriter _writer;

    DISALLOW_COPY_AND_ASSIGN(IntegerColumnWriterWrapper);
};

template<class T>
class DoubleColumnWriterBase: public ColumnWriter {
public:
    DoubleColumnWriterBase(
            uint32_t column_id, 
            OutStreamFactory* stream_factory,
            const FieldInfo& field_info,
            size_t num_rows_per_row_block,
            double bf_fpp) : 
            ColumnWriter(column_id, stream_factory, field_info, num_rows_per_row_block, bf_fpp),
            _stream(NULL) {}

    virtual ~DoubleColumnWriterBase() {}

    virtual OLAPStatus init() {
        OLAPStatus res = OLAP_SUCCESS;

        res = ColumnWriter::init();
        if (OLAP_SUCCESS != res) {
            return res;
        }

        OutStreamFactory* factory = stream_factory();
        _stream = factory->create_stream(
                unique_column_id(), StreamInfoMessage::DATA);

        if (NULL == _stream) {
            OLAP_LOG_WARNING("fail to allocate DATA STERAM");
            return OLAP_ERR_MALLOC_ERROR;
        }

        record_position();
        return OLAP_SUCCESS;
    }

    virtual OLAPStatus flush() {
        return OLAP_SUCCESS;
    }

    virtual OLAPStatus write(RowCursor* row_cursor) {
        OLAPStatus res = ColumnWriter::write(row_cursor);

        if (OLAP_UNLIKELY(OLAP_SUCCESS != res)) {
            OLAP_LOG_WARNING("fail to write ColumnWriter. [res=%d]", res);
            return res;
        }

        bool is_null = row_cursor->is_null(column_id());
        const Field* field = row_cursor->get_field_by_index(column_id());
        char* buf = field->get_field_ptr(row_cursor->get_buf());
        _block_statistics.add(buf);
        if (!is_null) {
            T* value = reinterpret_cast<T*>(buf + 1);
            return _stream->write(reinterpret_cast<char*>(value), sizeof(T));
        }

        return OLAP_SUCCESS;
    }

    virtual OLAPStatus finalize(ColumnDataHeaderMessage* header) {
        OLAPStatus res = OLAP_SUCCESS;

        res = ColumnWriter::finalize(header);
        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("fail to finalize ColumnWriter. [res=%d]", res);
            return res;
        }

        res = _stream->flush();
        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("fail to flush. [res=%d]", res);
            return res;
        }

        return OLAP_SUCCESS;
    }

    virtual void record_position() {
        ColumnWriter::record_position();
        _stream->get_position(index_entry());
    }

private:
    OutStream* _stream;

    DISALLOW_COPY_AND_ASSIGN(DoubleColumnWriterBase);
};

typedef DoubleColumnWriterBase<double> DoubleColumnWriter;
typedef DoubleColumnWriterBase<float> FloatColumnWriter;
typedef IntegerColumnWriterWrapper<int64_t, true> DiscreteDoubleColumnWriter;

// VarString和String都作为变长类型使用StringColumnWriter写入
class VarStringColumnWriter : public ColumnWriter {
public:
    VarStringColumnWriter(uint32_t column_id, OutStreamFactory* stream_factory,
                          const FieldInfo& field_info, size_t num_rows_per_row_block, 
                          double bf_fpp);
    virtual ~VarStringColumnWriter();
    virtual OLAPStatus init();
    virtual OLAPStatus write(RowCursor* row_cursor);
    virtual uint64_t estimate_buffered_memory();
    virtual OLAPStatus finalize(ColumnDataHeaderMessage* header);
    virtual void save_encoding(ColumnEncodingMessage* encoding);
    virtual void record_position();
    virtual OLAPStatus flush() {
        return OLAP_SUCCESS;
    }
protected:
    // 不使用cursor直接写入一条数据
    OLAPStatus write(const char* str, uint32_t length);
private:
    // 可以在map中使用引用做key
    class DictKey {
    public:
        explicit DictKey(const std::string& str_ref) : _str_ref(str_ref) {}
        bool operator < (const DictKey& other) const {
            return _str_ref < other._str_ref;
        }
        bool operator == (const DictKey& other) const {
            return _str_ref == other._str_ref;
        }
        const std::string& get() const {
            return _str_ref;
        }
    private:
        const std::string _str_ref;
    };
    typedef std::map<DictKey, uint32_t> StringDict;
private:
    OLAPStatus _finalize_dict_encoding();
    OLAPStatus _finalize_direct_encoding();
private:
    bool _use_dictionary_encoding;
    std::vector<uint32_t> _string_id;
    std::vector<std::string> _string_keys;
    StringDict _string_dict;
    uint64_t _dict_total_size;
    OutStream* _dict_stream;
    RunLengthIntegerWriter* _length_writer;
    OutStream* _data_stream;
    RunLengthIntegerWriter* _id_writer;
    std::vector<uint32_t> _block_row_count;

    DISALLOW_COPY_AND_ASSIGN(VarStringColumnWriter);
};

// 特例化一下VarStringColumnWriter, 在write的时候提取数据再写入
class FixLengthStringColumnWriter : public VarStringColumnWriter {
public:
    FixLengthStringColumnWriter(uint32_t column_id,
            OutStreamFactory* stream_factory,
            const FieldInfo& field_info,
            size_t num_rows_per_row_block,
            double bf_fpp);
    virtual ~FixLengthStringColumnWriter();
    virtual OLAPStatus write(RowCursor* row_cursor);
    virtual OLAPStatus flush() {
        return OLAP_SUCCESS;
    }
private:
    uint32_t _length;

    DISALLOW_COPY_AND_ASSIGN(FixLengthStringColumnWriter);
};

// Date是三字节整数
typedef IntegerColumnWriterWrapper<uint24_t, false> DateColumnWriter;

// DateTime是用int64实现的
typedef IntegerColumnWriterWrapper<uint64_t, false> DateTimeColumnWriter;

class DecimalColumnWriter : public ColumnWriter {
public:
    DecimalColumnWriter(uint32_t column_id, OutStreamFactory* stream_factory,
                        const FieldInfo& field_info, size_t num_rows_per_row_block,
                        double bf_fpp);
    virtual ~DecimalColumnWriter();
    virtual OLAPStatus init();
    virtual OLAPStatus write(RowCursor* row_cursor);
    virtual OLAPStatus finalize(ColumnDataHeaderMessage* header);
    virtual void record_position();
    virtual OLAPStatus flush() {
        return OLAP_SUCCESS;
    }
private:
    RunLengthIntegerWriter* _int_writer;
    RunLengthIntegerWriter* _frac_writer;

    DISALLOW_COPY_AND_ASSIGN(DecimalColumnWriter);
};

class LargeIntColumnWriter : public ColumnWriter {
public:
    LargeIntColumnWriter(uint32_t column_id, OutStreamFactory* stream_factory,
                         const FieldInfo& field_info, size_t num_rows_per_row_block,
                         double bf_fpp);
    virtual ~LargeIntColumnWriter();
    virtual OLAPStatus init();
    virtual OLAPStatus write(RowCursor* row_cursor);
    virtual OLAPStatus finalize(ColumnDataHeaderMessage* header);
    virtual void record_position();
    virtual OLAPStatus flush() {
        return OLAP_SUCCESS;
    }

private:
    RunLengthIntegerWriter* _high_writer;
    RunLengthIntegerWriter* _low_writer;

    DISALLOW_COPY_AND_ASSIGN(LargeIntColumnWriter);
};

}  // namespace column_file
}  // namespace palo
#endif // BDG_PALO_BE_SRC_OLAP_COLUMN_FILE_COLUMN_WRITER_H
