/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <filesystem>
#include <source_location>
#include <fmt/chrono.h>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/util/closeable.hh>

#include "compaction/compaction.hh"
#include "db/config.hh"
#include "db/large_data_handler.hh"
#include "gms/feature_service.hh"
#include "reader_concurrency_semaphore.hh"
#include "readers/combined.hh"
#include "readers/generating_v2.hh"
#include "schema_builder.hh"
#include "sstables/index_reader.hh"
#include "sstables/sstables_manager.hh"
#include "sstables/open_info.hh"
#include "types/user.hh"
#include "types/set.hh"
#include "types/map.hh"
#include "tools/schema_loader.hh"
#include "tools/utils.hh"
#include "utils/rjson.hh"
#include "locator/host_id.hh"

// has to be below the utils/rjson.hh include
#include <rapidjson/ostreamwrapper.h>

using namespace seastar;

namespace bpo = boost::program_options;

namespace {

const auto app_name = "scylla-sstable";

logging::logger sst_log(app_name);

db::nop_large_data_handler large_data_handler;

struct decorated_key_hash {
    std::size_t operator()(const dht::decorated_key& dk) const {
        return dht::token::to_int64(dk.token());
    }
};

struct decorated_key_equal {
    const schema& _s;
    explicit decorated_key_equal(const schema& s) : _s(s) {
    }
    bool operator()(const dht::decorated_key& a, const dht::decorated_key& b) const {
        return a.equal(_s, b);
    }
};

using partition_set = std::unordered_set<dht::decorated_key, decorated_key_hash, decorated_key_equal>;

template <typename T>
using partition_map = std::unordered_map<dht::decorated_key, T, decorated_key_hash, decorated_key_equal>;

partition_set get_partitions(schema_ptr schema, const bpo::variables_map& app_config) {
    partition_set partitions(app_config.count("partition"), {}, decorated_key_equal(*schema));
    auto pk_type = schema->partition_key_type();

    auto dk_from_hex = [&] (std::string_view hex) {
        auto pk = partition_key::from_exploded(pk_type->components(managed_bytes_view(from_hex(hex))));
        return dht::decorate_key(*schema, std::move(pk));
    };

    if (app_config.count("partition")) {
        for (const auto& pk_hex : app_config["partition"].as<std::vector<sstring>>()) {
            partitions.emplace(dk_from_hex(pk_hex));
        }
    }

    if (app_config.count("partitions-file")) {
        auto file = open_file_dma(app_config["partitions-file"].as<sstring>(), open_flags::ro).get();
        auto fstream = make_file_input_stream(file);

        temporary_buffer<char> pk_buf;
        while (auto buf = fstream.read().get()) {
            do {
                const auto it = std::find_if(buf.begin(), buf.end(), [] (char c) { return std::isspace(c); });
                const auto len = it - buf.begin();
                if (!len && !pk_buf) {
                    buf.trim_front(1); // discard extra leading whitespace
                    continue;
                }
                if (pk_buf) {
                    auto new_buf = temporary_buffer<char>(pk_buf.size() + len);
                    auto ot = new_buf.get_write();
                    ot = std::copy_n(pk_buf.begin(), pk_buf.size(), ot);
                    std::copy_n(buf.begin(), len, ot);
                    pk_buf = std::move(new_buf);
                } else {
                    pk_buf = buf.share(0, len);
                }
                buf.trim_front(len);
                if (it != buf.end()) {
                    partitions.emplace(dk_from_hex(std::string_view(pk_buf.begin(), pk_buf.size())));
                    pk_buf = {};
                    buf.trim_front(1); // remove the newline
                }
                thread::maybe_yield();
            } while (buf);
        }
        if (!pk_buf.empty()) { // last line might not have EOL
            partitions.emplace(dk_from_hex(std::string_view(pk_buf.begin(), pk_buf.size())));
        }
    }

    if (!partitions.empty()) {
        sst_log.info("filtering enabled, {} partition(s) to filter for", partitions.size());
    }

    return partitions;
}

const std::vector<sstables::shared_sstable> load_sstables(schema_ptr schema, sstables::sstables_manager& sst_man, const std::vector<sstring>& sstable_names) {
    std::vector<sstables::shared_sstable> sstables;

    parallel_for_each(sstable_names, [schema, &sst_man, &sstables] (const sstring& sst_name) -> future<> {
        const auto sst_path = std::filesystem::path(sst_name);

        if (const auto ftype_opt = co_await file_type(sst_path.c_str(), follow_symlink::yes)) {
            if (!ftype_opt) {
                throw std::invalid_argument(fmt::format("error: failed to determine type of file pointed to by provided sstable path {}", sst_path.c_str()));
            }
            if (*ftype_opt != directory_entry_type::regular) {
                throw std::invalid_argument(fmt::format("error: file pointed to by provided sstable path {} is not a regular file", sst_path.c_str()));
            }
        }

        const auto dir_path = std::filesystem::path(sst_path).remove_filename();
        const auto sst_filename = sst_path.filename();

        auto ed = sstables::entry_descriptor::make_descriptor(dir_path.c_str(), sst_filename.c_str(), schema->ks_name(), schema->cf_name());
        auto sst = sst_man.make_sstable(schema, dir_path.c_str(), ed.generation, ed.version, ed.format);

        co_await sst->load();

        sstables.push_back(std::move(sst));
    }).get();

    return sstables;
}

// stop_iteration::no -> continue consuming sstable content
class sstable_consumer {
public:
    virtual ~sstable_consumer() = default;
    // called at the very start
    virtual future<> on_start_of_stream() = 0;
    // stop_iteration::yes -> on_end_of_sstable() - skip sstable content
    // sstable parameter is nullptr when merging multiple sstables
    virtual future<stop_iteration> on_new_sstable(const sstables::sstable* const) = 0;
    // stop_iteration::yes -> consume(partition_end) - skip partition content
    virtual future<stop_iteration> consume(partition_start&&) = 0;
    // stop_iteration::yes -> consume(partition_end) - skip remaining partition content
    virtual future<stop_iteration> consume(static_row&&) = 0;
    // stop_iteration::yes -> consume(partition_end) - skip remaining partition content
    virtual future<stop_iteration> consume(clustering_row&&) = 0;
    // stop_iteration::yes -> consume(partition_end) - skip remaining partition content
    virtual future<stop_iteration> consume(range_tombstone_change&&) = 0;
    // stop_iteration::yes -> on_end_of_sstable() - skip remaining partitions in sstable
    virtual future<stop_iteration> consume(partition_end&&) = 0;
    // stop_iteration::yes -> full stop - skip remaining sstables
    virtual future<stop_iteration> on_end_of_sstable() = 0;
    // called at the very end
    virtual future<> on_end_of_stream() = 0;
};

class consumer_wrapper {
public:
    using filter_type = std::function<bool(const dht::decorated_key&)>;
private:
    sstable_consumer& _consumer;
    filter_type _filter;
public:
    consumer_wrapper(sstable_consumer& consumer, filter_type filter)
        : _consumer(consumer), _filter(std::move(filter)) {
    }
    future<stop_iteration> operator()(mutation_fragment_v2&& mf) {
        sst_log.trace("consume {}", mf.mutation_fragment_kind());
        if (mf.is_partition_start() && _filter && !_filter(mf.as_partition_start().key())) {
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        }
        return std::move(mf).consume(_consumer);
    }
};

class json_writer {
    using stream = rapidjson::BasicOStreamWrapper<std::ostream>;
    using writer = rapidjson::Writer<stream, rjson::encoding, rjson::encoding, rjson::allocator>;

    stream _stream;
    writer _writer;

public:
    json_writer() : _stream(std::cout), _writer(_stream)
    { }

    // following the rapidjson method names here
    bool Null() { return _writer.Null(); }
    bool Bool(bool b) { return _writer.Bool(b); }
    bool Int(int i) { return _writer.Int(i); }
    bool Uint(unsigned i) { return _writer.Uint(i); }
    bool Int64(int64_t i) { return _writer.Int64(i); }
    bool Uint64(uint64_t i) { return _writer.Uint64(i); }
    bool Double(double d) { return _writer.Double(d); }
    bool RawNumber(std::string_view str) { return _writer.RawNumber(str.data(), str.size(), false); }
    bool String(std::string_view str) { return _writer.String(str.data(), str.size(), false); }
    bool StartObject() { return _writer.StartObject(); }
    bool Key(std::string_view str) { return _writer.Key(str.data(), str.size(), false); }
    bool EndObject(rapidjson::SizeType memberCount = 0) { return _writer.EndObject(memberCount); }
    bool StartArray() { return _writer.StartArray(); }
    bool EndArray(rapidjson::SizeType elementCount = 0) { return _writer.EndArray(elementCount); }

    // scylla-specific extensions (still following rapidjson naming scheme for consistency)
    template <typename T>
    void AsString(const T& obj) {
        String(fmt::format("{}", obj));
    }
    void PartitionKey(const schema& schema, const partition_key& pkey, std::optional<dht::token> token = {}) {
        StartObject();
        if (token) {
            Key("token");
            AsString(*token);
        }
        Key("raw");
        String(to_hex(pkey.representation()));
        Key("value");
        AsString(pkey.with_schema(schema));
        EndObject();
    }
    void StartStream() {
        StartObject();
        Key("sstables");
        StartObject();
    }
    void EndStream() {
        EndObject();
        EndObject();
    }
    void SstableKey(const sstables::sstable& sst) {
        Key(sst.get_filename());
    }
    void SstableKey(const sstables::sstable* const sst) {
        if (sst) {
            SstableKey(*sst);
        } else {
            Key("anonymous");
        }
    }
};

enum class output_format {
    text, json
};

output_format get_output_format_from_options(const bpo::variables_map& opts, output_format default_format) {
    if (auto it = opts.find("output-format"); it != opts.end()) {
        const auto& value = it->second.as<std::string>();
        if (value == "text") {
            return output_format::text;
        } else if (value == "json") {
            return output_format::json;
        } else {
            throw std::invalid_argument(fmt::format("error: invalid value for dump option output-format: {}", value));
        }
    }
    return default_format;
}

class dumping_consumer : public sstable_consumer {
    class text_dumper : public sstable_consumer {
        const schema& _schema;
    public:
        explicit text_dumper(const schema& s) : _schema(s) { }
        virtual future<> on_start_of_stream() override {
            fmt::print("{{stream_start}}\n");
            return make_ready_future<>();
        }
        virtual future<stop_iteration> on_new_sstable(const sstables::sstable* const sst) override {
            fmt::print("{{sstable_start{}}}\n", sst ? fmt::format(": filename {}", sst->get_filename()) : "");
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(partition_start&& ps) override {
            fmt::print("{}\n", ps);
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(static_row&& sr) override {
            fmt::print("{}\n", static_row::printer(_schema, sr));
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(clustering_row&& cr) override {
            fmt::print("{}\n", clustering_row::printer(_schema, cr));
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(range_tombstone_change&& rtc) override {
            fmt::print("{}\n", rtc);
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(partition_end&& pe) override {
            fmt::print("{{partition_end}}\n");
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> on_end_of_sstable() override {
            fmt::print("{{sstable_end}}\n");
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<> on_end_of_stream() override {
            fmt::print("{{stream_end}}\n");
            return make_ready_future<>();
        }
    };
    class json_dumper : public sstable_consumer {
        const schema& _schema;
        json_writer _writer;
        bool _clustering_array_created;
    private:
        sstring to_string(gc_clock::time_point tp) {
            return fmt::format("{:%F %T}z", fmt::gmtime(gc_clock::to_time_t(tp)));
        }
        void write(gc_clock::duration ttl, gc_clock::time_point expiry) {
            _writer.Key("ttl");
            _writer.AsString(ttl);
            _writer.Key("expiry");
            _writer.String(to_string(expiry));
        }
        template <typename Key>
        void write_key(const Key& key) {
            _writer.StartObject();
            _writer.Key("raw");
            _writer.String(to_hex(key.representation()));
            _writer.Key("value");
            _writer.AsString(key.with_schema(_schema));
            _writer.EndObject();
        }
        void write(const tombstone& t) {
            _writer.StartObject();
            if (t) {
                _writer.Key("timestamp");
                _writer.Int64(t.timestamp);
                _writer.Key("deletion_time");
                _writer.String(to_string(t.deletion_time));
            }
            _writer.EndObject();
        }
        void write(const row_marker& m) {
            _writer.StartObject();
            _writer.Key("timestamp");
            _writer.Int64(m.timestamp());
            if (m.is_live() && m.is_expiring()) {
                write(m.ttl(), m.expiry());
            }
            _writer.EndObject();
        }
        void write(counter_cell_view cv) {
            _writer.StartArray();
            for (const auto& shard : cv.shards()) {
                _writer.StartObject();
                _writer.Key("id");
                _writer.AsString(shard.id());
                _writer.Key("value");
                _writer.Int64(shard.value());
                _writer.Key("clock");
                _writer.Int64(shard.logical_clock());
                _writer.EndObject();
            }
            _writer.EndArray();
        }
        void write(const atomic_cell_view& cell, data_type type) {
            _writer.StartObject();
            _writer.Key("is_live");
            _writer.Bool(cell.is_live());
            _writer.Key("timestamp");
            _writer.Int64(cell.timestamp());
            if (type->is_counter()) {
                if (cell.is_counter_update()) {
                    _writer.Key("value");
                    _writer.Int64(cell.counter_update_value());
                } else {
                    _writer.Key("shards");
                    write(counter_cell_view(cell));
                }
            } else {
                if (cell.is_live_and_has_ttl()) {
                    write(cell.ttl(), cell.expiry());
                }
                if (cell.is_live()) {
                    _writer.Key("value");
                    _writer.String(type->to_string(cell.value().linearize()));
                } else {
                    _writer.Key("deletion_time");
                    _writer.String(to_string(cell.deletion_time()));
                }
            }
            _writer.EndObject();
        }
        void write(const collection_mutation_view_description& mv, data_type type) {
            _writer.StartObject();

            if (mv.tomb) {
                _writer.Key("tombstone");
                write(mv.tomb);
            }

            _writer.Key("cells");

            std::function<void(size_t, bytes_view)> write_key;
            std::function<void(size_t, atomic_cell_view)> write_value;
            if (auto t = dynamic_cast<const collection_type_impl*>(type.get())) {
                write_key = [this, t] (size_t, bytes_view k) { _writer.Key(t->name_comparator()->to_string(k)); };
                write_value = [this, t] (size_t, atomic_cell_view v) { write(v, t->value_comparator()); };
            } else if (auto t = dynamic_cast<const tuple_type_impl*>(type.get())) {
                write_key = [this] (size_t i, bytes_view) { _writer.Key(format("{}", i)); };
                write_value = [this, t] (size_t i, atomic_cell_view v) { write(v, t->type(i)); };
            }

            if (write_key && write_value) {
                _writer.StartObject();
                for (size_t i = 0; i < mv.cells.size(); ++i) {
                    write_key(i, mv.cells[i].first);
                    write_value(i, mv.cells[i].second);
                }
                _writer.EndObject();
            } else {
                _writer.String("<unknown>");
            }

            _writer.EndObject();
        }
        void write(const atomic_cell_or_collection& cell, const column_definition& cdef) {
            if (cdef.is_atomic()) {
                write(cell.as_atomic_cell(cdef), cdef.type);
            } else if (cdef.type->is_collection() || cdef.type->is_user_type()) {
                cell.as_collection_mutation().with_deserialized(*cdef.type, [&, this] (collection_mutation_view_description mv) {
                    write(mv, cdef.type);
                });
            } else {
                _writer.String("<unknown>");
            }
        }
        void write(const row& r, column_kind kind) {
            _writer.StartObject();
            r.for_each_cell([this, kind] (column_id id, const atomic_cell_or_collection& cell) {
                auto cdef = _schema.column_at(kind, id);
                _writer.Key(cdef.name_as_text());
                write(cell, cdef);
            });
            _writer.EndObject();
        }
        void write(const clustering_row& cr) {
            _writer.StartObject();
            _writer.Key("type");
            _writer.String("clustering-row");
            _writer.Key("key");
            write_key(cr.key());
            if (cr.tomb()) {
                _writer.Key("tombstone");
                write(cr.tomb().regular());
                _writer.Key("shadowable_tombstone");
                write(cr.tomb().shadowable().tomb());
            }
            if (!cr.marker().is_missing()) {
                _writer.Key("marker");
                write(cr.marker());
            }
            _writer.Key("columns");
            write(cr.cells(), column_kind::regular_column);
            _writer.EndObject();
        }
        void write(const range_tombstone_change& rtc) {
            _writer.StartObject();
            _writer.Key("type");
            _writer.String("range-tombstone-change");
            const auto pos = rtc.position();
            if (pos.has_key()) {
                _writer.Key("key");
                write_key(pos.key());
            }
            _writer.Key("weight");
            _writer.Int(static_cast<int>(pos.get_bound_weight()));
            _writer.Key("tombstone");
            write(rtc.tombstone());
            _writer.EndObject();
        }
    public:
        explicit json_dumper(const schema& s) : _schema(s) {}
        virtual future<> on_start_of_stream() override {
            _writer.StartStream();
            return make_ready_future<>();
        }
        virtual future<stop_iteration> on_new_sstable(const sstables::sstable* const sst) override {
            _writer.SstableKey(sst);
            _writer.StartArray();
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(partition_start&& ps) override {
            const auto& dk = ps.key();
            _clustering_array_created = false;

            _writer.StartObject();

            _writer.Key("key");
            _writer.PartitionKey(_schema, dk.key(), dk.token());

            if (ps.partition_tombstone()) {
                _writer.Key("tombstone");
                write(ps.partition_tombstone());
            }

            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(static_row&& sr) override {
            _writer.Key("static_row");
            write(sr.cells(), column_kind::static_column);
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(clustering_row&& cr) override {
            if (!_clustering_array_created) {
                _writer.Key("clustering_elements");
                _writer.StartArray();
                _clustering_array_created = true;
            }
            write(cr);
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(range_tombstone_change&& rtc) override {
            if (!_clustering_array_created) {
                _writer.Key("clustering_elements");
                _writer.StartArray();
                _clustering_array_created = true;
            }
            write(rtc);
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> consume(partition_end&& pe) override {
            if (_clustering_array_created) {
                _writer.EndArray();
            }
            _writer.EndObject();
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<stop_iteration> on_end_of_sstable() override {
            _writer.EndArray();
            return make_ready_future<stop_iteration>(stop_iteration::no);
        }
        virtual future<> on_end_of_stream() override {
            _writer.EndStream();
            return make_ready_future<>();
        }
    };

private:
    schema_ptr _schema;
    std::unique_ptr<sstable_consumer> _consumer;

public:
    explicit dumping_consumer(schema_ptr s, reader_permit, const bpo::variables_map& opts) : _schema(std::move(s)) {
        _consumer = std::make_unique<text_dumper>(*_schema);
        switch (get_output_format_from_options(opts, output_format::text)) {
            case output_format::text:
                _consumer = std::make_unique<text_dumper>(*_schema);
                break;
            case output_format::json:
                _consumer = std::make_unique<json_dumper>(*_schema);
                break;
        }
    }
    virtual future<> on_start_of_stream() override { return _consumer->on_start_of_stream(); }
    virtual future<stop_iteration> on_new_sstable(const sstables::sstable* const sst) override { return _consumer->on_new_sstable(sst); }
    virtual future<stop_iteration> consume(partition_start&& ps) override { return _consumer->consume(std::move(ps)); }
    virtual future<stop_iteration> consume(static_row&& sr) override { return _consumer->consume(std::move(sr)); }
    virtual future<stop_iteration> consume(clustering_row&& cr) override { return _consumer->consume(std::move(cr)); }
    virtual future<stop_iteration> consume(range_tombstone_change&& rtc) override { return _consumer->consume(std::move(rtc)); }
    virtual future<stop_iteration> consume(partition_end&& pe) override { return _consumer->consume(std::move(pe)); }
    virtual future<stop_iteration> on_end_of_sstable() override { return _consumer->on_end_of_sstable(); }
    virtual future<> on_end_of_stream() override { return _consumer->on_end_of_stream(); }
};

class writetime_histogram_collecting_consumer : public sstable_consumer {
private:
    enum class bucket {
        years,
        months,
        weeks,
        days,
        hours,
    };

public:
    schema_ptr _schema;
    bucket _bucket = bucket::months;
    std::map<api::timestamp_type, uint64_t> _histogram;
    uint64_t _partitions = 0;
    uint64_t _rows = 0;
    uint64_t _cells = 0;
    uint64_t _timestamps = 0;

private:
    api::timestamp_type timestamp_bucket(api::timestamp_type ts) {
        using namespace std::chrono;
        switch (_bucket) {
            case bucket::years:
                return duration_cast<microseconds>(duration_cast<years>(microseconds(ts))).count();
            case bucket::months:
                return duration_cast<microseconds>(duration_cast<months>(microseconds(ts))).count();
            case bucket::weeks:
                return duration_cast<microseconds>(duration_cast<weeks>(microseconds(ts))).count();
            case bucket::days:
                return duration_cast<microseconds>(duration_cast<days>(microseconds(ts))).count();
            case bucket::hours:
                return duration_cast<microseconds>(duration_cast<hours>(microseconds(ts))).count();
        }
        std::abort();
    }
    void collect_timestamp(api::timestamp_type ts) {
        ts = timestamp_bucket(ts);

        ++_timestamps;
        auto it = _histogram.find(ts);
        if (it == _histogram.end()) {
            it = _histogram.emplace(ts, 0).first;
        }
        ++it->second;
    }
    void collect_column(const atomic_cell_or_collection& cell, const column_definition& cdef) {
        if (cdef.is_atomic()) {
            ++_cells;
            collect_timestamp(cell.as_atomic_cell(cdef).timestamp());
        } else if (cdef.type->is_collection() || cdef.type->is_user_type()) {
            cell.as_collection_mutation().with_deserialized(*cdef.type, [&, this] (collection_mutation_view_description mv) {
                if (mv.tomb) {
                    collect_timestamp(mv.tomb.timestamp);
                }
                for (auto&& c : mv.cells) {
                    ++_cells;
                    collect_timestamp(c.second.timestamp());
                }
            });
        } else {
            throw std::runtime_error(fmt::format("Cannot collect timestamp of cell (column {} of uknown type {})", cdef.name_as_text(), cdef.type->name()));
        }
    }

    void collect_row(const row& r, column_kind kind) {
        ++_rows;
        r.for_each_cell([this, kind] (column_id id, const atomic_cell_or_collection& cell) {
            collect_column(cell, _schema->column_at(kind, id));
        });
    }

    void collect_static_row(const static_row& sr) {
        collect_row(sr.cells(), column_kind::static_column);
    }

    void collect_clustering_row(const clustering_row& cr) {
        if (!cr.marker().is_missing()) {
            collect_timestamp(cr.marker().timestamp());
        }
        if (cr.tomb() != row_tombstone{}) {
            collect_timestamp(cr.tomb().tomb().timestamp);
        }

        collect_row(cr.cells(), column_kind::regular_column);
    }

public:
    explicit writetime_histogram_collecting_consumer(schema_ptr s, reader_permit, const bpo::variables_map& vm) : _schema(std::move(s)) {
        auto it = vm.find("bucket");
        if (it != vm.end()) {
            auto value = it->second.as<std::string>();
            if (value == "years") {
                _bucket = bucket::years;
            } else if (value == "months") {
                _bucket = bucket::months;
            } else if (value == "weeks") {
                _bucket = bucket::weeks;
            } else if (value == "days") {
                _bucket = bucket::days;
            } else if (value == "hours") {
                _bucket = bucket::hours;
            } else {
                throw std::invalid_argument(fmt::format("error: invalid value for writetime-histogram option bucket: {}", value));
            }
        }
    }
    virtual future<> on_start_of_stream() override {
        return make_ready_future<>();
    }
    virtual future<stop_iteration> on_new_sstable(const sstables::sstable* const sst) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(partition_start&& ps) override {
        ++_partitions;
        if (auto tomb = ps.partition_tombstone()) {
            collect_timestamp(tomb.timestamp);
        }
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(static_row&& sr) override {
        collect_static_row(sr);
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(clustering_row&& cr) override {
        collect_clustering_row(cr);
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(range_tombstone_change&& rtc) override {
        collect_timestamp(rtc.tombstone().timestamp);
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(partition_end&& pe) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> on_end_of_sstable() override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<> on_end_of_stream() override {
        if (_histogram.empty()) {
            sst_log.info("Histogram empty, no data to write");
            co_return;
        }
        sst_log.info("Histogram has {} entries, collected from {} partitions, {} rows, {} cells: {} timestamps total", _histogram.size(), _partitions, _rows, _cells, _timestamps);

        const auto filename = "histogram.json";

        auto file = co_await open_file_dma(filename, open_flags::wo | open_flags::create);
        auto fstream = co_await make_file_output_stream(file);

        co_await fstream.write("{");

        co_await fstream.write("\n\"buckets\": [");
        auto it = _histogram.begin();
        co_await fstream.write(format("\n  {}", it->first));
        for (++it; it != _histogram.end(); ++it) {
            co_await fstream.write(format(",\n  {}", it->first));
        }
        co_await fstream.write("\n]");

        co_await fstream.write(",\n\"counts\": [");
        it = _histogram.begin();
        co_await fstream.write(format("\n  {}", it->second));
        for (++it; it != _histogram.end(); ++it) {
            co_await fstream.write(format(",\n  {}", it->second));
        }
        co_await fstream.write("\n]");
        co_await fstream.write("\n}");

        co_await fstream.close();

        sst_log.info("Histogram written to {}", filename);

        co_return;
    }
};

// scribble here, then call with --operation=custom
class custom_consumer : public sstable_consumer {
    schema_ptr _schema;
    reader_permit _permit;
public:
    explicit custom_consumer(schema_ptr s, reader_permit p, const bpo::variables_map&)
        : _schema(std::move(s)), _permit(std::move(p))
    { }
    virtual future<> on_start_of_stream() override {
        return make_ready_future<>();
    }
    virtual future<stop_iteration> on_new_sstable(const sstables::sstable* const sst) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(partition_start&& ps) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(static_row&& sr) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(clustering_row&& cr) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(range_tombstone_change&& rtc) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> consume(partition_end&& pe) override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<stop_iteration> on_end_of_sstable() override {
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }
    virtual future<> on_end_of_stream() override {
        return make_ready_future<>();
    }
};

stop_iteration consume_reader(flat_mutation_reader_v2 rd, sstable_consumer& consumer, sstables::sstable* sst, const partition_set& partitions, bool no_skips) {
    auto close_rd = deferred_close(rd);
    if (consumer.on_new_sstable(sst).get() == stop_iteration::yes) {
        return consumer.on_end_of_sstable().get();
    }
    bool skip_partition = false;
    consumer_wrapper::filter_type filter;
    if (!partitions.empty()) {
        filter = [&] (const dht::decorated_key& key) {
            const auto pass = partitions.find(key) != partitions.end();
            sst_log.trace("filter({})={}", key, pass);
            skip_partition = !pass;
            return pass;
        };
    }
    while (!rd.is_end_of_stream()) {
        skip_partition = false;
        rd.consume_pausable(consumer_wrapper(consumer, filter)).get();
        sst_log.trace("consumer paused, skip_partition={}", skip_partition);
        if (!rd.is_end_of_stream() && !skip_partition) {
            if (auto* mfp = rd.peek().get(); mfp && !mfp->is_partition_start()) {
                sst_log.trace("consumer returned stop_iteration::yes for partition end, stopping");
                break;
            }
            if (consumer.consume(partition_end{}).get() == stop_iteration::yes) {
                sst_log.trace("consumer returned stop_iteration::yes for synthetic partition end, stopping");
                break;
            }
            skip_partition = true;
        }
        if (skip_partition) {
            if (no_skips) {
                mutation_fragment_v2_opt mfopt;
                while ((mfopt = rd().get()) && !mfopt->is_end_of_partition());
            } else {
                rd.next_partition().get();
            }
        }
    }
    return consumer.on_end_of_sstable().get();
}

void consume_sstables(schema_ptr schema, reader_permit permit, std::vector<sstables::shared_sstable> sstables, bool merge, bool use_crawling_reader,
        std::function<stop_iteration(flat_mutation_reader_v2&, sstables::sstable*)> reader_consumer) {
    sst_log.trace("consume_sstables(): {} sstables, merge={}, use_crawling_reader={}", sstables.size(), merge, use_crawling_reader);
    if (merge) {
        std::vector<flat_mutation_reader_v2> readers;
        readers.reserve(sstables.size());
        for (const auto& sst : sstables) {
            if (use_crawling_reader) {
                readers.emplace_back(sst->make_crawling_reader(schema, permit));
            } else {
                readers.emplace_back(sst->make_reader(schema, permit, query::full_partition_range, schema->full_slice()));
            }
        }
        auto rd = make_combined_reader(schema, permit, std::move(readers));

        reader_consumer(rd, nullptr);
    } else {
        for (const auto& sst : sstables) {
            auto rd = use_crawling_reader
                ? sst->make_crawling_reader(schema, permit)
                : sst->make_reader(schema, permit, query::full_partition_range, schema->full_slice());

            if (reader_consumer(rd, sst.get()) == stop_iteration::yes) {
                break;
            }
        }
    }
}

using operation_func = void(*)(schema_ptr, reader_permit, const std::vector<sstables::shared_sstable>&, sstables::sstables_manager&, const bpo::variables_map&);

class operation {
    std::string _name;
    std::string _summary;
    std::string _description;
    std::vector<std::string> _available_options;
    operation_func _func;

public:
    operation(std::string name, std::string summary, std::string description, operation_func func)
        : _name(std::move(name)), _summary(std::move(summary)), _description(std::move(description)), _func(func) {
    }
    operation(std::string name, std::string summary, std::string description, std::vector<std::string> available_options, operation_func func)
        : _name(std::move(name)), _summary(std::move(summary)), _description(std::move(description)), _available_options(std::move(available_options)), _func(func) {
    }

    const std::string& name() const { return _name; }
    const std::string& summary() const { return _summary; }
    const std::string& description() const { return _description; }
    const std::vector<std::string>& available_options() const { return _available_options; }

    void operator()(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
            sstables::sstables_manager& sst_man, const bpo::variables_map& vm) const {
        _func(std::move(schema), std::move(permit), sstables, sst_man, vm);
    }
};

void validate_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map& vm) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }
    const auto merge = vm.count("merge");
    sstables::compaction_data info;
    consume_sstables(schema, permit, sstables, merge, true, [&info] (flat_mutation_reader_v2& rd, sstables::sstable* sst) {
        if (sst) {
            sst_log.info("validating {}", sst->get_filename());
        }
        const auto errors = sstables::scrub_validate_mode_validate_reader(std::move(rd), info).get();
        sst_log.info("validated {}: {}", sst ? sst->get_filename() : "the stream", errors == 0 ? "valid" : "invalid");
        return stop_iteration::no;
    });
}

void dump_index_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map&) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }

    json_writer writer;
    writer.StartStream();
    for (auto& sst : sstables) {
        sstables::index_reader idx_reader(sst, permit, default_priority_class(), {}, sstables::use_caching::yes);
        auto close_idx_reader = deferred_close(idx_reader);

        writer.SstableKey(*sst);
        writer.StartArray();

        while (!idx_reader.eof()) {
            idx_reader.read_partition_data().get();
            auto pos = idx_reader.get_data_file_position();
            auto pkey = idx_reader.get_partition_key();

            writer.StartObject();
            writer.Key("key");
            writer.PartitionKey(*schema, pkey);
            writer.Key("pos");
            writer.Uint64(pos);
            writer.EndObject();

            idx_reader.advance_to_next_partition().get();
        }
        writer.EndArray();
    }
    writer.EndStream();
}

template <typename Integer>
sstring disk_string_to_string(const sstables::disk_string<Integer>& ds) {
    return sstring(ds.value.begin(), ds.value.end());
}

void dump_compression_info_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map&) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }

    json_writer writer;
    writer.StartStream();

    for (auto& sst : sstables) {
        const auto& compression = sst->get_compression();

        writer.SstableKey(*sst);
        writer.StartObject();
        writer.Key("name");
        writer.String(disk_string_to_string(compression.name));
        writer.Key("options");
        writer.StartObject();
        for (const auto& opt : compression.options.elements) {
            writer.Key(disk_string_to_string(opt.key));
            writer.String(disk_string_to_string(opt.value));
        }
        writer.EndObject();
        writer.Key("chunk_len");
        writer.Uint(compression.chunk_len);
        writer.Key("data_len");
        writer.Uint64(compression.data_len);
        writer.Key("offsets");
        writer.StartArray();
        for (const auto& offset : compression.offsets) {
            writer.Uint64(offset);
        }
        writer.EndArray();
        writer.EndObject();
    }
    writer.EndStream();
}

void dump_summary_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map&) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }

    json_writer writer;
    writer.StartStream();

    for (auto& sst : sstables) {
        auto& summary = sst->get_summary();

        writer.SstableKey(*sst);
        writer.StartObject();

        writer.Key("header");
        writer.StartObject();
        writer.Key("min_index_interval");
        writer.Uint64(summary.header.min_index_interval);
        writer.Key("size");
        writer.Uint64(summary.header.size);
        writer.Key("memory_size");
        writer.Uint64(summary.header.memory_size);
        writer.Key("sampling_level");
        writer.Uint64(summary.header.sampling_level);
        writer.Key("size_at_full_sampling");
        writer.Uint64(summary.header.size_at_full_sampling);
        writer.EndObject();

        writer.Key("positions");
        writer.StartArray();
        for (const auto& pos : summary.positions) {
            writer.Uint64(pos);
        }
        writer.EndArray();

        writer.Key("entries");
        writer.StartArray();
        for (const auto& e : summary.entries) {
            writer.StartObject();

            auto pkey = e.get_key().to_partition_key(*schema);
            writer.Key("key");
            writer.PartitionKey(*schema, pkey, e.token);
            writer.Key("position");
            writer.Uint64(e.position);

            writer.EndObject();
        }
        writer.EndArray();

        auto first_key = sstables::key_view(summary.first_key.value).to_partition_key(*schema);
        writer.Key("first_key");
        writer.PartitionKey(*schema, first_key);

        auto last_key = sstables::key_view(summary.last_key.value).to_partition_key(*schema);
        writer.Key("last_key");
        writer.PartitionKey(*schema, last_key);

        writer.EndObject();
    }
    writer.EndStream();
}

class json_dumper {
    json_writer& _writer;
    sstables::sstable_version_types _version;
    std::function<std::string_view(const void* const)> _name_resolver;

private:
    void visit(int8_t val) { _writer.Int(val); }
    void visit(uint8_t val) { _writer.Uint(val); }
    void visit(int val) { _writer.Int(val); }
    void visit(unsigned val) { _writer.Uint(val); }
    void visit(int64_t val) { _writer.Int64(val); }
    void visit(uint64_t val) { _writer.Uint64(val); }
    void visit(double val) {
        if (std::isnan(val)) {
            _writer.String("NaN");
        } else {
            _writer.Double(val);
        }
    }

    template <typename Integer>
    void visit(const sstables::disk_string<Integer>& val) {
        _writer.String(disk_string_to_string(val));
    }

    template <typename Contents>
    void visit(const std::optional<Contents>& val) {
        if (bool(val)) {
            visit(*val);
        } else {
            _writer.Null();
        }
    }

    template <typename Integer, typename T>
    void visit(const sstables::disk_array<Integer, T>& val) {
        _writer.StartArray();
        for (const auto& elem : val.elements) {
            visit(elem);
        }
        _writer.EndArray();
    }

    void visit(const sstables::disk_string_vint_size& val) {
        _writer.String(sstring(val.value.begin(), val.value.end()));
    }

    template <typename T>
    void visit(const sstables::disk_array_vint_size<T>& val) {
        _writer.StartArray();
        for (const auto& elem : val.elements) {
            visit(elem);
        }
        _writer.EndArray();
    }

    void visit(const utils::estimated_histogram& val) {
        _writer.StartArray();
        for (size_t i = 0; i < val.buckets.size(); i++) {
            _writer.StartObject();
            _writer.Key("offset");
            _writer.Int64(val.bucket_offsets[i == 0 ? 0 : i - 1]);
            _writer.Key("value");
            _writer.Int64(val.buckets[i]);
            _writer.EndObject();
        }
        _writer.EndArray();
    }

    void visit(const utils::streaming_histogram& val) {
        _writer.StartObject();
        for (const auto& [k, v] : val.bin) {
            _writer.Key(format("{}", k));
            _writer.Uint64(v);
        }
        _writer.EndObject();
    }

    void visit(const db::replay_position& val) {
        _writer.StartObject();
        _writer.Key("id");
        _writer.Uint64(val.id);
        _writer.Key("pos");
        _writer.Uint(val.pos);
        _writer.EndObject();
    }

    void visit(const sstables::commitlog_interval& val) {
        _writer.StartObject();
        _writer.Key("start");
        visit(val.start);
        _writer.Key("end");
        visit(val.end);
        _writer.EndObject();
    }

    void visit(const utils::UUID& uuid) {
        _writer.String(uuid.to_sstring());
    }

    template <typename Tag>
    void visit(const utils::tagged_uuid<Tag>& id) {
        visit(id.uuid());
    }

    template <typename Integer>
    void visit(const sstables::vint<Integer>& val) {
        visit(val.value);
    }

    void visit(const sstables::serialization_header::column_desc& val) {
        auto prev_name_resolver = std::exchange(_name_resolver, [&val] (const void* const field) {
            if (field == &val.name) { return "name"; }
            else if (field == &val.type_name) { return "type_name"; }
            else { throw std::invalid_argument("invalid field offset"); }
        });

        const_cast<sstables::serialization_header::column_desc&>(val).describe_type(_version, std::ref(*this));

        _name_resolver = std::move(prev_name_resolver);
    }

    json_dumper(json_writer& writer, sstables::sstable_version_types version, std::function<std::string_view(const void* const)> name_resolver)
        : _writer(writer), _version(version), _name_resolver(std::move(name_resolver)) {
    }

public:
    template <typename Arg1>
    void operator()(Arg1& arg1) {
        _writer.Key(_name_resolver(&arg1));
        visit(arg1);
    }

    template <typename Arg1, typename... Arg>
    void operator()(Arg1& arg1, Arg&... arg) {
        _writer.Key(_name_resolver(&arg1));
        visit(arg1);
        (*this)(arg...);
    }

    template <typename T>
    static void dump(json_writer& writer, sstables::sstable_version_types version, const T& obj, std::string_view name,
            std::function<std::string_view(const void* const)> name_resolver) {
        json_dumper dumper(writer, version, std::move(name_resolver));
        writer.Key(name);
        writer.StartObject();
        const_cast<T&>(obj).describe_type(version, std::ref(dumper));
        writer.EndObject();
    }
};

void dump_validation_metadata(json_writer& writer, sstables::sstable_version_types version, const sstables::validation_metadata& metadata) {
    json_dumper::dump(writer, version, metadata, "validation", [&metadata] (const void* const field) {
        if (field == &metadata.partitioner) { return "partitioner"; }
        else if (field == &metadata.filter_chance) { return "filter_chance"; }
        else { throw std::invalid_argument("invalid field offset"); }
    });
}

void dump_compaction_metadata(json_writer& writer, sstables::sstable_version_types version, const sstables::compaction_metadata& metadata) {
    json_dumper::dump(writer, version, metadata, "compaction", [&metadata] (const void* const field) {
        if (field == &metadata.ancestors) { return "ancestors"; }
        else if (field == &metadata.cardinality) { return "cardinality"; }
        else { throw std::invalid_argument("invalid field offset"); }
    });
}

void dump_stats_metadata(json_writer& writer, sstables::sstable_version_types version, const sstables::stats_metadata& metadata) {
    json_dumper::dump(writer, version, metadata, "stats", [&metadata] (const void* const field) {
        if (field == &metadata.estimated_partition_size) { return "estimated_partition_size"; }
        else if (field == &metadata.estimated_cells_count) { return "estimated_cells_count"; }
        else if (field == &metadata.position) { return "position"; }
        else if (field == &metadata.min_timestamp) { return "min_timestamp"; }
        else if (field == &metadata.max_timestamp) { return "max_timestamp"; }
        else if (field == &metadata.min_local_deletion_time) { return "min_local_deletion_time"; }
        else if (field == &metadata.max_local_deletion_time) { return "max_local_deletion_time"; }
        else if (field == &metadata.min_ttl) { return "min_ttl"; }
        else if (field == &metadata.max_ttl) { return "max_ttl"; }
        else if (field == &metadata.compression_ratio) { return "compression_ratio"; }
        else if (field == &metadata.estimated_tombstone_drop_time) { return "estimated_tombstone_drop_time"; }
        else if (field == &metadata.sstable_level) { return "sstable_level"; }
        else if (field == &metadata.repaired_at) { return "repaired_at"; }
        else if (field == &metadata.min_column_names) { return "min_column_names"; }
        else if (field == &metadata.max_column_names) { return "max_column_names"; }
        else if (field == &metadata.has_legacy_counter_shards) { return "has_legacy_counter_shards"; }
        else if (field == &metadata.columns_count) { return "columns_count"; }
        else if (field == &metadata.rows_count) { return "rows_count"; }
        else if (field == &metadata.commitlog_lower_bound) { return "commitlog_lower_bound"; }
        else if (field == &metadata.commitlog_intervals) { return "commitlog_intervals"; }
        else if (field == &metadata.originating_host_id) { return "originating_host_id"; }
        else { throw std::invalid_argument("invalid field offset"); }
    });
}

void dump_serialization_header(json_writer& writer, sstables::sstable_version_types version, const sstables::serialization_header& metadata) {
    json_dumper::dump(writer, version, metadata, "serialization_header", [&metadata] (const void* const field) {
        if (field == &metadata.min_timestamp_base) { return "min_timestamp_base"; }
        else if (field == &metadata.min_local_deletion_time_base) { return "min_local_deletion_time_base"; }
        else if (field == &metadata.min_ttl_base) { return "min_ttl_base"; }
        else if (field == &metadata.pk_type_name) { return "pk_type_name"; }
        else if (field == &metadata.clustering_key_types_names) { return "clustering_key_types_names"; }
        else if (field == &metadata.static_columns) { return "static_columns"; }
        else if (field == &metadata.regular_columns) { return "regular_columns"; }
        else { throw std::invalid_argument("invalid field offset"); }
    });
}

void dump_statistics_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map&) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }

    auto to_string = [] (sstables::metadata_type t) {
        switch (t) {
            case sstables::metadata_type::Validation: return "validation";
            case sstables::metadata_type::Compaction: return "compaction";
            case sstables::metadata_type::Stats: return "stats";
            case sstables::metadata_type::Serialization: return "serialization";
        }
        std::abort();
    };

    json_writer writer;
    writer.StartStream();
    for (auto& sst : sstables) {
        auto& statistics = sst->get_statistics();

        writer.SstableKey(*sst);
        writer.StartObject();

        writer.Key("offsets");
        writer.StartObject();
        for (const auto& [k, v] : statistics.offsets.elements) {
            writer.Key(to_string(k));
            writer.Uint(v);
        }
        writer.EndObject();

        const auto version = sst->get_version();
        for (const auto& [type, _] : statistics.offsets.elements) {
            const auto& metadata_ptr = statistics.contents.at(type);
            switch (type) {
                case sstables::metadata_type::Validation:
                    dump_validation_metadata(writer, version, *dynamic_cast<const sstables::validation_metadata*>(metadata_ptr.get()));
                    break;
                case sstables::metadata_type::Compaction:
                    dump_compaction_metadata(writer, version, *dynamic_cast<const sstables::compaction_metadata*>(metadata_ptr.get()));
                    break;
                case sstables::metadata_type::Stats:
                    dump_stats_metadata(writer, version, *dynamic_cast<const sstables::stats_metadata*>(metadata_ptr.get()));
                    break;
                case sstables::metadata_type::Serialization:
                    dump_serialization_header(writer, version, *dynamic_cast<const sstables::serialization_header*>(metadata_ptr.get()));
                    break;
            }
        }

        writer.EndObject();
    }
    writer.EndStream();
}

const char* to_string(sstables::scylla_metadata_type t) {
    switch (t) {
        case sstables::scylla_metadata_type::Sharding: return "sharding";
        case sstables::scylla_metadata_type::Features: return "features";
        case sstables::scylla_metadata_type::ExtensionAttributes: return "extension_attributes";
        case sstables::scylla_metadata_type::RunIdentifier: return "run_identifier";
        case sstables::scylla_metadata_type::LargeDataStats: return "large_data_stats";
        case sstables::scylla_metadata_type::SSTableOrigin: return "sstable_origin";
        case sstables::scylla_metadata_type::ScyllaVersion: return "scylla_version";
        case sstables::scylla_metadata_type::ScyllaBuildId: return "scylla_build_id";
    }
    std::abort();
}

const char* to_string(sstables::large_data_type t) {
    switch (t) {
        case sstables::large_data_type::partition_size: return "partition_size";
        case sstables::large_data_type::row_size: return "row_size";
        case sstables::large_data_type::cell_size: return "cell_size";
        case sstables::large_data_type::rows_in_partition: return "rows_in_partition";
        case sstables::large_data_type::elements_in_collection: return "elements_in_collection";
    }
    std::abort();
}

class scylla_metadata_visitor : public boost::static_visitor<> {
    json_writer& _writer;

public:
    scylla_metadata_visitor(json_writer& writer) : _writer(writer) { }

    void operator()(const sstables::sharding_metadata& val) const {
        _writer.StartArray();
        for (const auto& e : val.token_ranges.elements) {
            _writer.StartObject();

            _writer.Key("left");
            _writer.StartObject();
            _writer.Key("exclusive");
            _writer.Bool(e.left.exclusive);
            _writer.Key("token");
            _writer.String(disk_string_to_string(e.left.token));
            _writer.EndObject();

            _writer.Key("right");
            _writer.StartObject();
            _writer.Key("exclusive");
            _writer.Bool(e.right.exclusive);
            _writer.Key("token");
            _writer.String(disk_string_to_string(e.right.token));
            _writer.EndObject();

            _writer.EndObject();
        }
        _writer.EndArray();
    }
    void operator()(const sstables::sstable_enabled_features& val) const {
        std::pair<sstables::sstable_feature, const char*> all_features[] = {
                {sstables::sstable_feature::NonCompoundPIEntries, "NonCompoundPIEntries"},
                {sstables::sstable_feature::NonCompoundRangeTombstones, "NonCompoundRangeTombstones"},
                {sstables::sstable_feature::ShadowableTombstones, "ShadowableTombstones"},
                {sstables::sstable_feature::CorrectStaticCompact, "CorrectStaticCompact"},
                {sstables::sstable_feature::CorrectEmptyCounters, "CorrectEmptyCounters"},
                {sstables::sstable_feature::CorrectUDTsInCollections, "CorrectUDTsInCollections"},
        };
        _writer.StartObject();
        _writer.Key("mask");
        _writer.Uint64(val.enabled_features);
        _writer.Key("features");
        _writer.StartArray();
        for (const auto& [mask, name] : all_features) {
            if (mask & val.enabled_features) {
                _writer.String(name);
            }
        }
        _writer.EndArray();
        _writer.EndObject();
    }
    void operator()(const sstables::scylla_metadata::extension_attributes& val) const {
        _writer.StartObject();
        for (const auto& [k, v] : val.map) {
            _writer.Key(disk_string_to_string(k));
            _writer.String(disk_string_to_string(v));
        }
        _writer.EndObject();
    }
    void operator()(const sstables::run_identifier& val) const {
        _writer.AsString(val.id.uuid());
    }
    void operator()(const sstables::scylla_metadata::large_data_stats& val) const {
        _writer.StartObject();
        for (const auto& [k, v] : val.map) {
            _writer.Key(to_string(k));
            _writer.StartObject();
            _writer.Key("max_value");
            _writer.Uint64(v.max_value);
            _writer.Key("threshold");
            _writer.Uint64(v.threshold);
            _writer.Key("above_threshold");
            _writer.Uint(v.above_threshold);
            _writer.EndObject();
        }
        _writer.EndObject();
    }
    template <typename Size>
    void operator()(const sstables::disk_string<Size>& val) const {
        _writer.String(disk_string_to_string(val));
    }

    template <sstables::scylla_metadata_type E, typename T>
    void operator()(const sstables::disk_tagged_union_member<sstables::scylla_metadata_type, E, T>& m) const {
        _writer.Key(to_string(E));
        (*this)(m.value);
    }
};

void dump_scylla_metadata_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map&) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }

    json_writer writer;
    writer.StartStream();
    for (auto& sst : sstables) {
        writer.SstableKey(*sst);
        writer.StartObject();
        auto m = sst->get_scylla_metadata();
        if (!m) {
            writer.EndObject();
            continue;
        }
        for (const auto& [k, v] : m->data.data) {
            boost::apply_visitor(scylla_metadata_visitor(writer), v);
        }
        writer.EndObject();
    }
    writer.EndStream();
}

void validate_checksums_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map&) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }

    for (auto& sst : sstables) {
        const auto valid = sstables::validate_checksums(sst, permit, default_priority_class()).get();
        sst_log.info("validated the checksums of {}: {}", sst->get_filename(), valid ? "valid" : "invalid");
    }
}

void decompress_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map& vm) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }

    for (const auto& sst : sstables) {
        if (!sst->get_compression()) {
            sst_log.info("Sstable {} is not compressed, nothing to do", sst->get_filename());
            continue;
        }

        auto output_filename = sst->get_filename();
        output_filename += ".decompressed";

        auto ofile = open_file_dma(output_filename, open_flags::wo | open_flags::create).get();
        file_output_stream_options options;
        options.buffer_size = 4096;
        auto ostream = make_file_output_stream(std::move(ofile), options).get();
        auto close_ostream = defer([&ostream] { ostream.close().get(); });

        auto istream = sst->data_stream(0, sst->data_size(), default_priority_class(), permit, nullptr, nullptr);
        auto close_istream = defer([&istream] { istream.close().get(); });

        istream.consume([&] (temporary_buffer<char> buf) {
            return ostream.write(buf.get(), buf.size()).then([] {
                return consumption_result<char>(continue_consuming{});
            });
        }).get();
        ostream.flush().get();

        sst_log.info("Sstable {} decompressed into {}", sst->get_filename(), output_filename);
    }
}

class json_mutation_stream_parser {
    using reader = rapidjson::GenericReader<rjson::encoding, rjson::encoding, rjson::allocator>;
    class stream {
    public:
        using Ch = char;
    private:
        input_stream<Ch> _is;
        temporary_buffer<Ch> _current;
        size_t _pos = 0;
        size_t _line = 1;
        size_t _last_lf_pos = 0;
    private:
        void maybe_read_some() {
            if (!_current.empty()) {
                return;
            }
            _current = _is.read().get();
            // EOS is encoded as null char
            if (_current.empty()) {
                _current = temporary_buffer<Ch>("\0", 1);
            }
        }
    public:
        stream(input_stream<Ch> is) : _is(std::move(is)) {
            maybe_read_some();
        }
        stream(stream&&) = default;
        ~stream() {
            _is.close().get();
        }
        Ch Peek() const {
            return *_current.get();
        }
        Ch Take() {
            auto c = Peek();
            if (c == '\n') {
                ++_line;
                ++_last_lf_pos = _pos;
            }
            ++_pos;
            _current.trim_front(1);
            maybe_read_some();
            return c;
        }
        size_t Tell() {
            return _pos;
        }
        // ostream methods, unused but need a definition
        Ch* PutBegin() { return nullptr; }
        void Put(Ch c) { }
        void Flush() { }
        size_t PutEnd(Ch* begin) { return 0; }
        // own methods
        size_t line() const {
            return _line;
        }
        size_t last_line_feed_pos() const {
            return _last_lf_pos;
        }
    };
    class handler {
    public:
        using Ch = char;
    private:
        enum class state {
            start,
            before_partition,
            in_partition,
            before_key,
            in_key,
            before_tombstone,
            in_tombstone,
            before_static_columns,
            before_clustering_elements,
            before_clustering_element,
            in_clustering_element,
            in_range_tombstone_change,
            in_clustering_row,
            before_marker,
            in_marker,
            before_clustering_columns,
            before_column_key,
            before_column,
            in_column,
            before_ignored_value,
            before_integer,
            before_string,
            before_bool,
        };
        struct column {
            const column_definition* def = nullptr;
            std::optional<bool> is_live;
            std::optional<api::timestamp_type> timestamp;
            std::optional<bytes> value;
            std::optional<gc_clock::time_point> deletion_time;

            explicit column(const column_definition* def) : def(def) { }
        };
        struct tombstone {
            std::optional<api::timestamp_type> timestamp;
            std::optional<gc_clock::time_point> deletion_time;
        };
    private:
        schema_ptr _schema;
        reader_permit _permit;
        queue<mutation_fragment_v2_opt>& _queue;
        circular_buffer<state> _state_stack;
        std::string _key; // last seen key
        bool _partition_start_emited = false;
        bool _is_shadowable = false; // currently processed tombstone is a shadowable one
        std::optional<bool> _bool;
        std::optional<int64_t> _integer;
        std::optional<std::string_view> _string;
        std::optional<partition_key> _pkey;
        std::optional<tombstone> _tombstone;
        std::optional<clustering_key> _ckey;
        std::optional<bound_weight> _bound_weight;
        std::optional<row_marker> _row_marker;
        std::optional<row_tombstone> _row_tombstone;
        std::optional<row> _row;
        std::optional<column> _column;
        std::optional<gc_clock::duration> _ttl;
        std::optional<gc_clock::time_point> _expiry;
    private:
        static std::string_view to_string(state s) {
            switch (s) {
                case state::start: return "start";
                case state::before_partition: return "before_partition";
                case state::in_partition: return "in_partition";
                case state::before_key: return "before_key";
                case state::in_key: return "in_key";
                case state::before_tombstone: return "before_tombstone";
                case state::in_tombstone: return "in_tombstone";
                case state::before_static_columns: return "before_static_columns";
                case state::before_clustering_elements: return "before_clustering_elements";
                case state::before_clustering_element: return "before_clustering_element";
                case state::in_clustering_element: return "in_clustering_element";
                case state::in_range_tombstone_change: return "in_range_tombstone_change";
                case state::in_clustering_row: return "in_clustering_row";
                case state::before_marker: return "before_marker";
                case state::in_marker: return "in_marker";
                case state::before_clustering_columns: return "before_clustering_columns";
                case state::before_column_key: return "before_column_key";
                case state::before_column: return "before_column";
                case state::in_column: return "in_column";
                case state::before_ignored_value: return "before_ignored_value";
                case state::before_integer: return "before_integer";
                case state::before_string: return "before_string";
                case state::before_bool: return "before_bool";
            }
            std::abort();
        }

        std::string stack_to_string() const {
            return boost::algorithm::join(_state_stack | boost::adaptors::transformed([] (state s) { return std::string(to_string(s)); }), "|");
        }

        bool error(const char* msg, auto&&... args) {
#if FMT_VERSION >= 80000
            auto parse_error = fmt::format(fmt::runtime(msg), std::forward<decltype(args)>(args)...);
#else
            auto parse_error = fmt::format(msg, std::forward<decltype(args)>(args)...);
#endif
            sst_log.trace(parse_error.c_str());
            _queue.abort(std::make_exception_ptr(std::runtime_error(parse_error)));
            return false;
        }

        bool emit(mutation_fragment_v2 mf) {
            sst_log.trace("emit({})", mf.mutation_fragment_kind());
            _queue.push_eventually(std::move(mf)).get();
            return true;
        }

        bool parse_partition_key() {
            try {
                auto raw = from_hex(*_string);
                _pkey.emplace(partition_key::from_bytes(raw));
            } catch (...) {
                return error("failed to parse partition key from raw string: {}", std::current_exception());
            }
            return true;
        }

        bool parse_clustering_key() {
            try {
                auto raw = from_hex(*_string);
                _ckey.emplace(clustering_key::from_bytes(raw));
            } catch (...) {
                return error("failed to parse clustering key from raw string: {}", std::current_exception());
            }
            return true;
        }

        bool parse_bound_weight() {
            switch (*_integer) {
                case -1:
                    _bound_weight.emplace(bound_weight::before_all_prefixed);
                    return true;
                case 0:
                    _bound_weight.emplace(bound_weight::equal);
                    return true;
                case 1:
                    _bound_weight.emplace(bound_weight::after_all_prefixed);
                    return true;
                default:
                    return error("failed to parse bound weight: {} is not a valid bound weight value", *_integer);
            }
        }

        bool parse_deletion_time() {
            try {
                auto dt = gc_clock::time_point(gc_clock::duration(timestamp_from_string(*_string) / 1000));
                if (top(1) == state::in_column) {
                    _column->deletion_time = dt;
                } else {
                    _tombstone->deletion_time = dt;
                }
                return true;
            } catch (...) {
                return error("failed to parse deletion_time: {}", std::current_exception());
            }
        }

        bool parse_ttl() {
            auto e = _string->end();
            if (*std::prev(e) == 's') {
                --e;
            }
            uint64_t ttl;
            std::stringstream ss(std::string(_string->begin(), e));
            ss >> ttl;
            if (ss.fail()) {
                return error("failed to parse ttl value of {}", _string);
            }
            _ttl = gc_clock::duration(ttl);
            return true;
        }

        bool parse_expiry() {
            try {
                _expiry = gc_clock::time_point(gc_clock::duration(timestamp_from_string(*_string) / 1000));
            } catch (...) {
                return error("failed to parse expiry: {}", std::current_exception());
            }
            return true;
        }

        std::optional<::tombstone> get_tombstone() {
            if (bool(_tombstone->timestamp) != bool(_tombstone->deletion_time)) {
                error("incomplete tombstone: timestamp or deletion-time have to be either both present or missing");
                return {};
            }
            if (!_tombstone->timestamp) {
                _tombstone.reset();
                return ::tombstone{};
            }
            auto tomb = ::tombstone(*_tombstone->timestamp, *_tombstone->deletion_time);
            _tombstone.reset();
            return tomb;
        }

        bool finalize_partition_start(::tombstone tomb = {}) {
            auto pkey = std::exchange(_pkey, {});
            if (!pkey) {
                return error("failed to finalize partition start: no partition key");
            }
            partition_start ps(dht::decorate_key(*_schema, *pkey), tomb);
            _partition_start_emited = true;
            return emit(mutation_fragment_v2(*_schema, _permit, std::move(ps)));
        }

        bool finalize_static_row() {
            if (!_row) {
                return error("failed to finalize clustering row: row is not initialized yet");
            }
            auto row = std::exchange(_row, {});
            auto sr = static_row(std::move(*row));
            return emit(mutation_fragment_v2(*_schema, _permit, std::move(sr)));
        }

        bool finalize_range_tombstone_change() {
            if (!_bound_weight) {
                return error("failed to finalize range tombstone change: missing bound weight");
            }
            if (*_bound_weight == bound_weight::equal) {
                return error("failed to finalize range tombstone change: bound_weight::equal is not valid for range tombstones changes");
            }
            if (!_row_tombstone) {
                return error("failed to finalize range tombstone change: missing tombstone");
            }
            clustering_key ckey = clustering_key::make_empty();
            if (_ckey) {
                ckey = std::move(*std::exchange(_ckey, {}));
            }
            auto pos = position_in_partition(partition_region::clustered, *std::exchange(_bound_weight, {}), std::move(ckey));
            auto tomb = std::exchange(_row_tombstone, {})->tomb();
            auto rtc = range_tombstone_change(std::move(pos), std::move(tomb));
            return emit(mutation_fragment_v2(*_schema, _permit, std::move(rtc)));
        }

        bool finalize_row_marker() {
            if (!_row_marker) {
                return error("failed to finalize row marker: it has no timestamp");
            }
            if (bool(_expiry) != bool(_ttl)) {
                return error("failed to finalize row marker: ttl and expiry must either be both present or both missing");
            }
            if (!_expiry && !_ttl) {
                return true;
            }
            _row_marker->apply(row_marker(_row_marker->timestamp(), *std::exchange(_ttl, {}), *std::exchange(_expiry, {})));
            return true;
        }

        bool parse_column_value() {
            try {
                _column->value.emplace(_column->def->type->from_string(*_string));
            } catch (...) {
                return error("failed to parse cell value: {}", std::current_exception());
            }
            return true;
        }

        bool finalize_column() {
            if (!_row) {
                return error("failed to finalize cell: row not initialized yet");
            }
            if (!_column->is_live || !_column->timestamp) {
                return error("failed to finalize cell: required fields is_live and/or timestamp missing");
            }
            if (*_column->is_live && !_column->value) {
                return error("failed to finalize cell: live cell doesn't have data");
            }
            if (!*_column->is_live && !_column->deletion_time) {
                return error("failed to finalize cell: dead cell doesn't have deletion time");
            }
            if (bool(_expiry) != bool(_ttl)) {
                return error("failed to finalize cell: ttl and expiry must either be both present or both missing");
            }
            if (*_column->is_live) {
                if (_ttl) {
                    _row->apply(*_column->def, ::atomic_cell::make_live(*_column->def->type, *_column->timestamp, *_column->value,
                            *std::exchange(_expiry, {}), *std::exchange(_ttl, {})));
                } else {
                    _row->apply(*_column->def, ::atomic_cell::make_live(*_column->def->type, *_column->timestamp, *_column->value));
                }
            } else {
                _row->apply(*_column->def, ::atomic_cell::make_dead(*_column->timestamp, *_column->deletion_time));
            }
            _column.reset();
            return true;
        }

        bool finalize_clustering_row() {
            if (!_ckey) {
                return error("failed to finalize clustering row: missing clustering key");
            }
            if (!_row) {
                return error("failed to finalize clustering row: row is not initialized yet");
            }
            auto row = std::exchange(_row, {});
            auto tomb = std::exchange(_row_tombstone, {});
            auto marker = std::exchange(_row_marker, {});
            auto cr = clustering_row(
                    std::move(*_ckey),
                    tomb.value_or(row_tombstone{}),
                    marker.value_or(row_marker{}),
                    std::move(*row));
            return emit(mutation_fragment_v2(*_schema, _permit, std::move(cr)));
        }

        bool finalize_partition() {
            _partition_start_emited = false;
            return emit(mutation_fragment_v2(*_schema, _permit, partition_end{}));
        }

        struct retire_state_result {
            bool ok = true;
            unsigned pop_states = 1;
            std::optional<state> next_state;
        };
        retire_state_result handle_retire_state() {
            sst_log.trace("handle_retire_state(): stack={}", stack_to_string());
            retire_state_result ret;
            switch (top()) {
                case state::before_partition:
                    // EOS
                    _queue.push_eventually({}).get();
                    break;
                case state::in_partition:
                    ret.ok = finalize_partition();
                    break;
                case state::in_key:
                    ret.pop_states = 2;
                    break;
                case state::in_tombstone:
                    ret.pop_states = 2;
                    {
                        auto is_shadowable = std::exchange(_is_shadowable, false);
                        auto tomb = get_tombstone();
                        if (!tomb) {
                            ret.ok = false;
                            break;
                        }
                        if (top(2) == state::in_partition) {
                            ret.ok = finalize_partition_start(*tomb);
                        } else if (top(2) == state::in_range_tombstone_change) {
                            _row_tombstone.emplace(*tomb);
                        } else if (top(2) == state::in_clustering_row) {
                            if (is_shadowable) {
                                if (!_row_tombstone) {
                                    ret.ok = error("cannot apply shadowable tombstone, row tombstone not initialized yet");
                                    break;
                                }
                                _row_tombstone->apply(shadowable_tombstone(*tomb), {});
                            } else {
                                _row_tombstone.emplace(*tomb);
                            }
                        } else {
                            ret.ok = error("retiring in_tombstone state in invalid context: {}", stack_to_string());
                        }
                    }
                    break;
                case state::in_marker:
                    ret.pop_states = 2;
                    ret.ok = finalize_row_marker();
                    break;
                case state::in_column:
                    ret.pop_states = 2;
                    ret.ok = finalize_column();
                    break;
                case state::before_column_key:
                    if (top(1) == state::before_static_columns) {
                        ret.ok = finalize_static_row();
                    }
                    ret.pop_states = 2;
                    break;
                case state::before_clustering_element:
                    ret.pop_states = 2;
                    break;
                case state::in_range_tombstone_change:
                    ret.pop_states = 2;
                    ret.ok = finalize_range_tombstone_change();
                    break;
                case state::in_clustering_row:
                    ret.pop_states = 2;
                    ret.ok = finalize_clustering_row();
                    break;
                case state::before_ignored_value:
                    break;
                case state::before_bool:
                    if (top(1) == state::in_column) {
                        _column->is_live = _bool;
                    }
                    _bool.reset();
                    break;
                case state::before_integer:
                    if (top(1) == state::in_tombstone) {
                        _tombstone->timestamp = _integer.value();
                    }
                    if (top(1) == state::in_range_tombstone_change) {
                        ret.ok = parse_bound_weight();
                    }
                    if (top(1) == state::in_column) {
                        _column->timestamp = _integer;
                    }
                    if (top(1) == state::in_marker) {
                        _row_marker.emplace(_integer.value());
                    }
                    _integer.reset();
                    break;
                case state::before_string:
                    if (top(1) == state::in_key) {
                        if (top(3) == state::in_partition) {
                            ret.ok = parse_partition_key();
                        } else if (top(3) == state::in_clustering_row || top(3) == state::in_range_tombstone_change) {
                            ret.ok = parse_clustering_key();
                        }
                    } else if (top(1) == state::in_tombstone) {
                        ret.ok = parse_deletion_time();
                    } else if (top(1) == state::in_marker) {
                        if (_key == "ttl") {
                            ret.ok = parse_ttl();
                        } else {
                            ret.ok = parse_expiry();
                        }
                    } else if (top(1) == state::in_clustering_element) {
                        if (*_string == "clustering-row") {
                            ret.next_state = state::in_clustering_row;
                        } else if (*_string == "range-tombstone-change") {
                            ret.next_state = state::in_range_tombstone_change;
                        } else {
                            ret.ok = error("invalid clustering element type: {}, expected clustering-row or range-tombstone-change", *_string);
                        }
                    } else if (top(1) == state::in_column) {
                        if (_key == "ttl") {
                            ret.ok = parse_ttl();
                        } else if (_key == "expiry") {
                            ret.ok = parse_expiry();
                        } else if (_key == "deletion_time") {
                            ret.ok = parse_deletion_time();
                        } else {
                            ret.ok = parse_column_value();
                        }
                    }
                    _string.reset();
                    break;
                default:
                    ret.ok =  error("attempted to retire unexpected state {} ({})", to_string(top()), stack_to_string());
                    break;
            }
            return ret;
        }
        state top(size_t i = 0) const {
            return _state_stack[i];
        }
        bool push(state s) {
            sst_log.trace("push({})", to_string(s));
            _state_stack.push_front(s);
            return true;
        }
        bool pop() {
            auto res = handle_retire_state();
            sst_log.trace("pop({})", res.ok ? res.pop_states : 0);
            if (!res.ok) {
                return false;
            }
            while (res.pop_states--) {
                _state_stack.pop_front();
            }
            if (res.next_state) {
                push(*res.next_state);
            }
            return true;
        }
        bool unexpected(std::experimental::source_location sl = std::experimental::source_location::current()) {
            return error("unexpected json event {} in state {}", sl.function_name(), stack_to_string());
        }
        bool unexpected(std::string_view key, std::experimental::source_location sl = std::experimental::source_location::current()) {
            return error("unexpected json event {}({}) in state {}", sl.function_name(), key, stack_to_string());
        }
    public:
        explicit handler(schema_ptr schema, reader_permit permit, queue<mutation_fragment_v2_opt>& queue)
            : _schema(std::move(schema))
            , _permit(std::move(permit))
            , _queue(queue)
        {
            push(state::start);
        }
        handler(handler&&) = default;
        bool Null() {
            sst_log.trace("Null()");
            switch (top()) {
                case state::before_ignored_value:
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool Bool(bool b) {
            sst_log.trace("Bool({})", b);
            switch (top()) {
                case state::before_bool:
                    _bool.emplace(b);
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool Int(int i) {
            sst_log.trace("Int({})", i);
            switch (top()) {
                case state::before_ignored_value:
                    return pop();
                case state::before_integer:
                    _integer.emplace(i);
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool Uint(unsigned i) {
            sst_log.trace("Uint({})", i);
            switch (top()) {
                case state::before_ignored_value:
                    return pop();
                case state::before_integer:
                    _integer.emplace(i);
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool Int64(int64_t i) {
            sst_log.trace("Int64({})", i);
            switch (top()) {
                case state::before_ignored_value:
                    return pop();
                case state::before_integer:
                    _integer.emplace(i);
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool Uint64(uint64_t i) {
            sst_log.trace("Uint64({})", i);
            switch (top()) {
                case state::before_ignored_value:
                    return pop();
                case state::before_integer:
                    _integer.emplace(i);
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool Double(double d) {
            sst_log.trace("Double({})", d);
            switch (top()) {
                case state::before_ignored_value:
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool RawNumber(const Ch* str, rapidjson::SizeType length, bool copy) {
            sst_log.trace("RawNumber({})", std::string_view(str, length));
            return unexpected();
        }
        bool String(const Ch* str, rapidjson::SizeType length, bool copy) {
            sst_log.trace("String({})", std::string_view(str, length));
            switch (top()) {
                case state::before_ignored_value:
                    return pop();
                case state::before_string:
                    _string.emplace(str, length);
                    return pop();
                default:
                    return unexpected();
            }
            return true;
        }
        bool StartObject() {
            sst_log.trace("StartObject()");
            switch (top()) {
                case state::before_partition:
                    return push(state::in_partition);
                case state::before_key:
                    return push(state::in_key);
                case state::before_tombstone:
                    _tombstone.emplace();
                    return push(state::in_tombstone);
                case state::before_static_columns:
                    _row.emplace();
                    return push(state::before_column_key);
                case state::before_clustering_element:
                    _row.emplace();
                    return push(state::in_clustering_element);
                case state::before_marker:
                    return push(state::in_marker);
                case state::before_clustering_columns:
                    return push(state::before_column_key);
                case state::before_column:
                    return push(state::in_column);
                default:
                    return unexpected();
            }
        }
        bool Key(const Ch* str, rapidjson::SizeType length, bool copy) {
            _key = std::string(str, length);
            sst_log.trace("Key({})", _key);
            switch (top()) {
                case state::in_partition:
                    if (_key == "key") {
                        return push(state::before_key);
                    }
                    if (_key == "tombstone") {
                        return push(state::before_tombstone);
                    }
                    if (_key == "static_row" || _key == "clustering_elements") {
                        if (!_partition_start_emited && !finalize_partition_start()) {
                            return false;
                        }
                        if (_key == "static_row") {
                            return push(state::before_static_columns);
                        } else {
                            return push(state::before_clustering_elements);
                        }
                    }
                    return unexpected(_key);
                case state::in_key:
                    if (_key == "value" || (top(2) == state::in_partition && _key == "token")) {
                        return push(state::before_ignored_value);
                    }
                    if (_key == "raw") {
                        return push(state::before_string);
                    }
                    return unexpected(_key);
                case state::in_tombstone:
                    if (_key == "timestamp") {
                        return push(state::before_integer);
                    }
                    if (_key == "deletion_time") {
                        return push(state::before_string);
                    }
                    return unexpected(_key);
                case state::in_marker:
                    if (_key == "timestamp") {
                        return push(state::before_integer);
                    }
                    if (_key == "ttl" || _key == "expiry") {
                        return push(state::before_string);
                    }
                    return unexpected(_key);
                case state::in_clustering_element:
                    if (_key == "type") {
                        return push(state::before_string);
                    }
                    return unexpected(_key);
                case state::in_range_tombstone_change:
                    if (_key == "key") {
                        return push(state::before_key);
                    }
                    if (_key == "weight") {
                        return push(state::before_integer);
                    }
                    if (_key == "tombstone") {
                        return push(state::before_tombstone);
                    }
                    return unexpected(_key);
                case state::in_clustering_row:
                    if (_key == "key") {
                        return push(state::before_key);
                    }
                    if (_key == "marker") {
                        return push(state::before_marker);
                    }
                    if (_key == "tombstone") {
                        return push(state::before_tombstone);
                    }
                    if (_key == "shadowable_tombstone") {
                        _is_shadowable = true;
                        return push(state::before_tombstone);
                    }
                    if (_key == "columns") {
                        return push(state::before_clustering_columns);
                    }
                    return unexpected(_key);
                case state::before_column_key:
                    _column.emplace(_schema->get_column_definition(bytes(reinterpret_cast<bytes::const_pointer>(_key.data()), _key.size())));
                    if (!_column->def) {
                        return error("failed to look-up column name {}", _key);
                    }
                    if (top(1) == state::before_static_columns && _column->def->kind != column_kind::static_column) {
                        return error("cannot add column {} of kind {} to static row", _key, to_sstring(_column->def->kind));
                    }
                    if (top(1) == state::before_clustering_columns && _column->def->kind != column_kind::regular_column) {
                        return error("cannot add column {} of kind {} to regular row", _key, to_sstring(_column->def->kind));
                    }
                    if (!_column->def->is_atomic()) {
                        return error("failed to initialize column {}: non-atomic columns are not supported yet", _key);
                    }
                    return push(state::before_column);
                case state::in_column:
                    if (_key == "is_live") {
                        return push(state::before_bool);
                    }
                    if (_key == "timestamp") {
                        return push(state::before_integer);
                    }
                    if (_key == "ttl" || _key == "expiry" || _key == "value" || _key == "deletion_time") {
                        return push(state::before_string);
                    }
                    return unexpected(_key);
                default:
                    return unexpected(_key);
            }
        }
        bool EndObject(rapidjson::SizeType memberCount) {
            sst_log.trace("EndObject()");
            switch (top()) {
                case state::in_partition:
                case state::in_key:
                case state::in_tombstone:
                case state::in_range_tombstone_change:
                case state::in_clustering_row:
                case state::before_column_key:
                case state::in_marker:
                case state::in_column:
                    return pop();
                default:
                    return unexpected();
            }
        }
        bool StartArray() {
            sst_log.trace("StartArray()");
            switch (top()) {
                case state::start:
                    return push(state::before_partition);
                case state::before_clustering_elements:
                    return push(state::before_clustering_element);
                default:
                    return unexpected();
            }
        }
        bool EndArray(rapidjson::SizeType elementCount) {
            sst_log.trace("EndArray({})", elementCount);
            switch (top()) {
                case state::before_clustering_element:
                case state::before_partition:
                    return pop();
                default:
                    return unexpected();
            }
        }
    };

private:
    class impl {
        queue<mutation_fragment_v2_opt> _queue;
        stream _stream;
        handler _handler;
        reader _reader;
        thread _thread;

    public:
        impl(schema_ptr schema, reader_permit permit, input_stream<char> istream)
            : _queue(1)
            , _stream(std::move(istream))
            , _handler(std::move(schema), std::move(permit), _queue)
            , _thread([this] { _reader.Parse(_stream, _handler); })
        { }
        impl(impl&&) = default;
        ~impl() {
            _thread.join().get();
        }
        future<mutation_fragment_v2_opt> operator()() {
            return _queue.pop_eventually().handle_exception([this] (std::exception_ptr e) -> mutation_fragment_v2_opt {
                auto err_off = _reader.GetErrorOffset();
                throw std::runtime_error(fmt::format("parsing input failed at line {}, offset {}: {}", _stream.line(), err_off - _stream.last_line_feed_pos(), e));
            });
        }
    };
    std::unique_ptr<impl> _impl;

public:
    explicit json_mutation_stream_parser(schema_ptr schema, reader_permit permit, input_stream<char> istream)
        : _impl(std::make_unique<impl>(std::move(schema), std::move(permit), std::move(istream)))
    { }
    future<mutation_fragment_v2_opt> operator()() { return (*_impl)(); }
};

void write_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& manager, const bpo::variables_map& vm) {
    static const std::vector<std::pair<std::string, mutation_fragment_stream_validation_level>> valid_validation_levels{
        {"partition_region", mutation_fragment_stream_validation_level::partition_region},
        {"token", mutation_fragment_stream_validation_level::token},
        {"partition_key", mutation_fragment_stream_validation_level::partition_key},
        {"clustering_key", mutation_fragment_stream_validation_level::clustering_key},
    };
    if (!sstables.empty()) {
        throw std::invalid_argument("error: write operation does not operate on input sstables");
    }
    if (!vm.count("input-file")) {
        throw std::invalid_argument("error: missing required option '--input-file'");
    }
    mutation_fragment_stream_validation_level validation_level;
    {
        const auto vl_name = vm["validation-level"].as<std::string>();
        auto vl_it = boost::find_if(valid_validation_levels, [&vl_name] (const std::pair<std::string, mutation_fragment_stream_validation_level>& v) {
            return v.first == vl_name;
        });
        if (vl_it == valid_validation_levels.end()) {
            throw std::invalid_argument(fmt::format("error: invalid validation-level {}", vl_name));
        }
        validation_level = vl_it->second;
    }
    auto input_file = vm["input-file"].as<std::string>();
    auto output_dir = vm["output-dir"].as<std::string>();
    if (!vm.count("generation")) {
        throw std::invalid_argument("error: missing required option '--generation'");
    }
    auto generation = sstables::generation_type(vm["generation"].as<int64_t>());
    auto format = sstables::sstable_format_types::big;
    auto version = sstables::get_highest_sstable_version();

    {
        auto sst_name = sstables::sstable::filename(output_dir, schema->ks_name(), schema->cf_name(), version, generation, format, component_type::Data);
        if (file_exists(sst_name).get()) {
            throw std::runtime_error(fmt::format("error: cannot create output sstable {}, file already exists", sst_name));
        }
    }

    auto ifile = open_file_dma(input_file, open_flags::ro).get();
    auto istream = make_file_input_stream(std::move(ifile));
    auto parser = json_mutation_stream_parser{schema, permit, std::move(istream)};
    auto reader = make_generating_reader_v2(schema, permit, std::move(parser));
    auto writer_cfg = manager.configure_writer("scylla-sstable");
    writer_cfg.validation_level = validation_level;
    auto sst = manager.make_sstable(schema, output_dir, generation, version, format);

    sst->write_components(std::move(reader), 1, schema, writer_cfg, encoding_stats{}).get();
}

template <typename SstableConsumer>
void sstable_consumer_operation(schema_ptr schema, reader_permit permit, const std::vector<sstables::shared_sstable>& sstables,
        sstables::sstables_manager& sst_man, const bpo::variables_map& vm) {
    if (sstables.empty()) {
        throw std::runtime_error("error: no sstables specified on the command line");
    }
    const auto merge = vm.count("merge");
    const auto no_skips = vm.count("no-skips");
    const auto partitions = get_partitions(schema, vm);
    const auto use_crawling_reader = no_skips || partitions.empty();
    auto consumer = std::make_unique<SstableConsumer>(schema, permit, vm);
    consumer->on_start_of_stream().get();
    consume_sstables(schema, permit, sstables, merge, use_crawling_reader, [&, &consumer = *consumer] (flat_mutation_reader_v2& rd, sstables::sstable* sst) {
        return consume_reader(std::move(rd), consumer, sst, partitions, no_skips);
    });
    consumer->on_end_of_stream().get();
}

class basic_option {
public:
    const char* name;
    const char* description;

public:
    basic_option(const char* name, const char* description) : name(name), description(description) { }

    virtual void add_option(bpo::options_description& opts) const = 0;
};

template <typename T = std::monostate>
class typed_option : public basic_option {
    std::optional<T> _default_value;

    virtual void add_option(bpo::options_description& opts) const override {
        if (_default_value) {
            opts.add_options()(name, bpo::value<T>()->default_value(*_default_value), description);
        } else {
            opts.add_options()(name, bpo::value<T>(), description);
        }
    }

public:
    typed_option(const char* name, const char* description) : basic_option(name, description) { }
    typed_option(const char* name, T default_value, const char* description) : basic_option(name, description), _default_value(std::move(default_value)) { }
};

template <>
class typed_option<std::monostate> : public basic_option {
    virtual void add_option(bpo::options_description& opts) const override {
        opts.add_options()(name, description);
    }
public:
    typed_option(const char* name, const char* description) : basic_option(name, description) { }
};

class option {
    shared_ptr<basic_option> _opt; // need copy to support convenient range declaration of std::vector<option>

public:
    template <typename T>
    option(typed_option<T> opt) : _opt(make_shared<typed_option<T>>(std::move(opt))) { }

    const char* name() const { return _opt->name; }
    const char* description() const { return _opt->description; }
    void add_option(bpo::options_description& opts) const { _opt->add_option(opts); }
};

const std::vector<option> all_options {
    typed_option<std::vector<sstring>>("partition", "partition(s) to filter for, partitions are expected to be in the hex format"),
    typed_option<sstring>("partitions-file", "file containing partition(s) to filter for, partitions are expected to be in the hex format"),
    typed_option<>("merge", "merge all sstables into a single mutation fragment stream (use a combining reader over all sstable readers)"),
    typed_option<>("no-skips", "don't use skips to skip to next partition when the partition filter rejects one, this is slower but works with corrupt index"),
    typed_option<std::string>("bucket", "months", "the unit of time to use as bucket, one of (years, months, weeks, days, hours)"),
    typed_option<std::string>("output-format", "json", "the output-format, one of (text, json)"),
    typed_option<std::string>("input-file", "the file containing the input"),
    typed_option<std::string>("output-dir", ".", "directory to place the output files to"),
    typed_option<int64_t>("generation", "generation of generated sstable"),
    typed_option<std::string>("validation-level", "clustering_key", "degree of validation on the output, one of (partition_region, token, partition_key, clustering_key)"),
};

const std::vector<operation> operations{
/* dump-data */
    {"dump-data",
            "Dump content of sstable(s)",
R"(
Dump the content of the data component. This component contains the data-proper
of the sstable. This might produce a huge amount of output. In general the
human-readable output will be larger than the binary file.
For more information about the sstable components and the format itself, visit
https://docs.scylladb.com/architecture/sstable/.

It is possible to filter the data to print via the --partitions or
--partitions-file options. Both expect partition key values in the hexdump
format.

Supports both a text and JSON output. The text output uses the built-in scylla
printers, which are also used when logging mutation-related data structures.

The schema of the JSON output is the following:

$ROOT := $NON_MERGED_ROOT | $MERGED_ROOT

$NON_MERGED_ROOT := { "$sstable_path": $SSTABLE, ... } // without --merge

$MERGED_ROOT := { "anonymous": $SSTABLE } // with --merge

$SSTABLE := [$PARTITION, ...]

$PARTITION := {
    "key": {
        "token": String,
        "raw": String, // hexadecimal representation of the raw binary
        "value": String
    },
    "tombstone: $TOMBSTONE, // optional
    "static_row": $COLUMNS, // optional
    "clustering_fragments": [
        $CLUSTERING_ROW | $RANGE_TOMBSTONE_CHANGE,
        ...
    ]
}

$TOMBSTONE := {
    "timestamp": Int64,
    "deletion_time": String // YYYY-MM-DD HH:MM:SS
}

$COLUMNS := {
    "$column_name": $REGULAR_CELL | $COUNTER_CELL | $COLLECTION,
    ...
}

$REGULAR_CELL := $REGULAR_LIVE_CELL | $REGULAR_DEAD_CELL

$REGULAR_LIVE_CELL := {
    "is_live": true,
    "timestamp": Int64,
    "ttl": String, // gc_clock::duration - optional
    "expiry": String, // YYYY-MM-DD HH:MM:SS - optional
    "value": String
}

$REGULAR_DEAD_CELL := {
    "is_live": false,
    "timestamp": Int64,
    "deletion_time": String // YYYY-MM-DD HH:MM:SS
}

$COUNTER_CELL := {
    "is_live": true,
    "timestamp": Int64,
    "shards": [$COUNTER_SHARD, ...]
}

$COUNTER_SHARD := {
    "id": String, // UUID
    "value": Int64,
    "clock": Int64
}

$COLLECTION := {
    "tombstone": $TOMBSTONE, // optional
    "cells": {
        "$key": $REGULAR_CELL,
        ...
    }
}

$CLUSTERING_ROW := {
    "type": "clustering-row",
    "key": {
        "raw": String, // hexadecimal representation of the raw binary
        "value": String
    },
    "tombstone": $TOMBSTONE, // optional
    "shadowable_tombstone": $TOMBSTONE, // optional
    "marker": { // optional
        "timestamp": Int64,
        "ttl": String, // gc_clock::duration
        "expiry": String // YYYY-MM-DD HH:MM:SS
    },
    "columns": $COLUMNS
}

$RANGE_TOMBSTONE_CHANGE := {
    "type": "range-tombstone-change",
    "key": { // optional
        "raw": String, // hexadecimal representation of the raw binary
        "value": String
    },
    "weight": Int, // -1 or 1
    "tombstone": $TOMBSTONE
}
)",
            {"partition", "partitions-file", "merge", "no-skips", "output-format"},
            sstable_consumer_operation<dumping_consumer>},
/* dump-index */
    {"dump-index",
            "Dump content of sstable index(es)",
R"(
Dump the content of the index component. Contains the partition-index of the data
component. This is effectively a list of all the partitions in the sstable, with
their starting position in the data component and optionally a promoted index,
which contains a sampled index of the clustering rows in the partition.
Positions (both that of partition and that of rows) is valid for uncompressed
data.
For more information about the sstable components and the format itself, visit
https://docs.scylladb.com/architecture/sstable/.

The content is dumped in JSON, using the following schema:

$ROOT := { "$sstable_path": $SSTABLE, ... }

$SSTABLE := [$INDEX_ENTRY, ...]

$INDEX_ENTRY := {
    "key": {
        "raw": String, // hexadecimal representation of the raw binary
        "value": String
    },
    "pos": Uint64
}
)",
            dump_index_operation},
/* dump-compression-info */
    {"dump-compression-info",
            "Dump content of sstable compression info(s)",
R"(
Dump the content of the compression-info component. Contains compression
parameters and maps positions into the uncompressed data to that into compressed
data. Note that compression happens over chunks with configurable size, so to
get data at a position in the middle of a compressed chunk, the entire chunk has
to be decompressed.
For more information about the sstable components and the format itself, visit
https://docs.scylladb.com/architecture/sstable/.

The content is dumped in JSON, using the following schema:

$ROOT := { "$sstable_path": $SSTABLE, ... }

$SSTABLE := {
    "name": String,
    "options": {
        "$option_name": String,
        ...
    },
    "chunk_len": Uint,
    "data_len": Uint64,
    "offsets": [Uint64, ...]
}
)",
            dump_compression_info_operation},
/* dump-summary */
    {"dump-summary",
            "Dump content of sstable summary(es)",
R"(
Dump the content of the summary component. The summary is a sampled index of the
content of the index-component. An index of the index. Sampling rate is chosen
such that this file is small enough to be kept in memory even for very large
sstables.
For more information about the sstable components and the format itself, visit
https://docs.scylladb.com/architecture/sstable/.

The content is dumped in JSON, using the following schema:

$ROOT := { "$sstable_path": $SSTABLE, ... }

$SSTABLE := {
    "header": {
        "min_index_interval": Uint64,
        "size": Uint64,
        "memory_size": Uint64,
        "sampling_level": Uint64,
        "size_at_full_sampling": Uint64
    },
    "positions": [Uint64, ...],
    "entries": [$SUMMARY_ENTRY, ...],
    "first_key": $KEY,
    "last_key": $KEY
}

$SUMMARY_ENTRY := {
    "key": $DECORATED_KEY,
    "position": Uint64
}

$DECORATED_KEY := {
    "token": String,
    "raw": String, // hexadecimal representation of the raw binary
    "value": String
}

$KEY := {
    "raw": String, // hexadecimal representation of the raw binary
    "value": String
}
)",
            dump_summary_operation},
/* dump-statistics */
    {"dump-statistics",
            "Dump content of sstable statistics(s)",
R"(
Dump the content of the statistics component. Contains various metadata about the
data component. In the sstable 3 format, this component is critical for parsing
the data component.
For more information about the sstable components and the format itself, visit
https://docs.scylladb.com/architecture/sstable/.

The content is dumped in JSON, using the following schema:

$ROOT := { "$sstable_path": $SSTABLE, ... }

$SSTABLE := {
    "offsets": {
        "$metadata": Uint,
        ...
    },
    "validation": $VALIDATION_METADATA,
    "compaction": $COMPACTION_METADATA,
    "stats": $STATS_METADATA,
    "serialization_header": $SERIALIZATION_HEADER // >= MC only
}

$VALIDATION_METADATA := {
    "partitioner": String,
    "filter_chance": Double
}

$COMPACTION_METADATA := {
    "ancestors": [Uint, ...], // < MC only
    "cardinality": [Uint, ...]
}

$STATS_METADATA := {
    "estimated_partition_size": $ESTIMATED_HISTOGRAM,
    "estimated_cells_count": $ESTIMATED_HISTOGRAM,
    "position": $REPLAY_POSITION,
    "min_timestamp": Int64,
    "max_timestamp": Int64,
    "min_local_deletion_time": Int64, // >= MC only
    "max_local_deletion_time": Int64,
    "min_ttl": Int64, // >= MC only
    "max_ttl": Int64, // >= MC only
    "compression_ratio": Double,
    "estimated_tombstone_drop_time": $STREAMING_HISTOGRAM,
    "sstable_level": Uint,
    "repaired_at": Uint64,
    "min_column_names": [Uint, ...],
    "max_column_names": [Uint, ...],
    "has_legacy_counter_shards": Bool,
    "columns_count": Int64, // >= MC only
    "rows_count": Int64, // >= MC only
    "commitlog_lower_bound": $REPLAY_POSITION, // >= MC only
    "commitlog_intervals": [$COMMITLOG_INTERVAL, ...] // >= MC only
}

$ESTIMATED_HISTOGRAM := [$ESTIMATED_HISTOGRAM_BUCKET, ...]

$ESTIMATED_HISTOGRAM_BUCKET := {
    "offset": Int64,
    "value": Int64
}

$STREAMING_HISTOGRAM := {
    "$key": Uint64,
    ...
}

$REPLAY_POSITION := {
    "id": Uint64,
    "pos": Uint
}

$COMMITLOG_INTERVAL := {
    "start": $REPLAY_POSITION,
    "end": $REPLAY_POSITION
}

$SERIALIZATION_HEADER_METADATA := {
    "min_timestamp_base": Uint64,
    "min_local_deletion_time_base": Uint64,
    "min_ttl_base": Uint64",
    "pk_type_name": String,
    "clustering_key_types_names": [String, ...],
    "static_columns": [$COLUMN_DESC, ...],
    "regular_columns": [$COLUMN_DESC, ...],
}

$COLUMN_DESC := {
    "name": String,
    "type_name": String
}
)",
            dump_statistics_operation},
/* dump-scylla-metadata */
    {"dump-scylla-metadata",
            "Dump content of sstable scylla metadata(s)",
R"(
Dump the content of the scylla-metadata component. Contains scylla-specific
metadata about the data component. This component won't be present in sstables
produced by Apache Cassandra.
For more information about the sstable components and the format itself, visit
https://docs.scylladb.com/architecture/sstable/.

The content is dumped in JSON, using the following schema:

$ROOT := { "$sstable_path": $SSTABLE, ... }

$SSTABLE := {
    "sharding": [$SHARDING_METADATA, ...],
    "features": $FEATURES_METADATA,
    "extension_attributes": { "$key": String, ...}
    "run_identifier": String, // UUID
    "large_data_stats": {"$key": $LARGE_DATA_STATS_METADATA, ...}
    "sstable_origin": String
}

$SHARDING_METADATA := {
    "left": {
        "exclusive": Bool,
        "token": String
    },
    "right": {
        "exclusive": Bool,
        "token": String
    }
}

$FEATURES_METADATA := {
    "mask": Uint64,
    "features": [String, ...]
}

$LARGE_DATA_STATS_METADATA := {
    "max_value": Uint64,
    "threshold": Uint64,
    "above_threshold": Uint
}
)",
            dump_scylla_metadata_operation},
/* writetime-histogram */
    {"writetime-histogram",
            "Generate a histogram of all the timestamps (writetime)",
R"(
Crawl over all timestamps in the data component and add them to a histogram. The
bucket size by default is a month, tunable with the --bucket option.
The timestamp of all objects that have one are added to the histogram:
* cells (atomic and collection cells)
* tombstones (partition-tombstone, range-tombstone, row-tombstone,
  shadowable-tombstone, cell-tombstone, collection-tombstone, cell-tombstone)
* row-marker

This allows determining when the data was written, provided the writer of the
data didn't mangle with the timestamps.
This produces a json file `histogram.json` whose content can be plotted with the
following example python script:

     import datetime
     import json
     import matplotlib.pyplot as plt # requires the matplotlib python package

     with open('histogram.json', 'r') as f:
         data = json.load(f)

     x = data['buckets']
     y = data['counts']

     max_y = max(y)

     x = [datetime.date.fromtimestamp(i / 1000000).strftime('%Y.%m') for i in x]
     y = [i / max_y for i in y]

     fig, ax = plt.subplots()

     ax.set_xlabel('Timestamp')
     ax.set_ylabel('Normalized cell count')
     ax.set_title('Histogram of data write-time')
     ax.bar(x, y)

     plt.show()
)",
            {"bucket"},
            sstable_consumer_operation<writetime_histogram_collecting_consumer>},
/* custom */
    {"custom",
            "Hackable custom operation for expert users, until scripting support is implemented",
R"(
Poor man's scripting support. Aimed at developers as it requires editing C++
source code and re-building the binary. Will be replaced by proper scripting
support soon (don't quote me on that).
)",
            sstable_consumer_operation<custom_consumer>},
/* validate */
    {"validate",
            "Validate the sstable(s), same as scrub in validate mode",
R"(
On a conceptual level, the data in sstables is represented by objects called
mutation fragments. We have the following kinds of fragments:
* partition-start (1)
* static-row (0-1)
* clustering-row (0-N)
* range-tombstone/range-tombstone-change (0-N)
* partition-end (1)

Data from the sstable is parsed into these fragments. We use these fragments to
stream data because it allows us to represent as little as part of a partition
or as many as the entire content of an sstable.

This operation validates data on the mutation-fragment level. Any parsing errors
will also be detected, but after successful parsing the validation will happen
on the fragment level. The following things are validated:
* Partitions are ordered in strictly monotonic ascending order [1].
* Fragments are correctly ordered. Fragments must follow the order defined in the
  listing above also respecting the occurrence numbers within a partition. Note
  that clustering rows and range tombstone [change] fragments can be intermingled.
* Clustering elements are ordered according in a strictly increasing clustering
  order as defined by the schema. Range tombstones (but not range tombstone
  changes) are allowed to have weakly monotonically increasing positions.
* The stream ends with a partition-end fragment.

[1] Although partitions are said to be unordered, this is only true w.r.t. the
data type of the key components. Partitions are ordered according to their tokens
(hashes), so partitions are unordered in the sense that a hash-table is
unordered: they have a random order as perceived by they user but they have a
well defined internal order.
)",
            {"merge"},
            validate_operation},
    {"validate-checksums",
            "Validate the checksums of the sstable(s)",
R"(
There are two kinds of checksums for sstable data files:
* The digest (full checksum), stored in the Digest.crc32 file. This is calculated
  over the entire content of Data.db.
* The per-chunk checksum. For uncompressed sstables, this is stored in CRC.db,
  for compressed sstables it is stored inline after each compressed chunk in
  Data.db.

During normal reads Scylla validates the per-chunk checksum for compressed
sstables. The digest and the per-chunk checksum of uncompressed sstables are not
checked on any code-paths currently.

This operation reads the entire Data.db and validates both kind of checksums
against the data. Errors found are logged to stderr. The output just contains a
bool for each sstable that is true if the sstable matches all checksums.

The content is dumped in JSON, using the following schema:

$ROOT := { "$sstable_path": Bool, ... }

)",
            validate_checksums_operation},
    {"decompress",
            "Decompress sstable(s)",
R"(
Decompress Data.db if compressed. Noop if not compressed. The decompressed data
is written to Data.db.decompressed. E.g. for an sstable:

    md-12311-big-Data.db

the output will be:

    md-12311-big-Data.db.decompressed
)",
            decompress_operation},
    {"write",
            "Write an sstable",
R"(
Write an sstable based on a JSON representation of the content. The JSON
representation has to have the same schema as that of a single sstable
from the output of the dump-data operation (corresponding to the $SSTABLE
symbol). See the help of dump-data more details on the json schema.
The easiest way to get started with writing your own sstable is to dump
an existing sstable, modify the json then invoke this operation with the
result. You can feed the output of dump-data to write by filtering the
output of the former with `jq .sstables[]`:

    $ scylla sstable dump-data --system-schema system_schema.columns /path/to/me-14-big-Data.db | jq .sstables[] > input.json
    $ scylla sstable write --system-schema system_schema.columns --input-file ./input.json --generation 0
    $ scylla sstable dump-data --system-schema system_schema.columns ./me-0-big-Data.db | jq .sstables[] > dump.json

At the end of the above, `input.json` and `dump.json` will have the same
content.

Note that "write" doesn't yet support all the features of the scylladb
storage engine. The following is not supported:
* Counters.
* Non-strictly atomic cells, this includes frozen multi-cell types like
  collections, tuples and UDTs.

Parsing uses a streaming json parser, it is safe to pass in input-files
of any size.

The output sstable will use the BIG format, the highest supported sstable
format and the specified generation (--generation). By default it is
placed in the local directory, can be changed with --output-dir. If the
output sstable clashes with an existing sstable, the write will fail.

The output is validated before being written to the disk. The validation
done here is similar to that done by the validate operation. The level
of validation can be changed with the --validation-level flag.
Possible validation-levels are:
* partition_region - only check fragment types, e.g. that a
  partition-end is followed by partition-start or EOS.
* token - also check token order of partitions.
* partition_key - full check on partition-ordering.
* clustering_key - also check clustering element ordering.

Note that levels are cumulative, each contains all the checks of the
previous levels too. By default the strictest level is used. This can
be relaxed if e.g. one wants to produce intentionally corrupt sstables
for tests.
)",
            {"input-file", "output-dir", "generation", "validation-level"},
            write_operation},
};

} // anonymous namespace

namespace tools {

int scylla_sstable_main(int argc, char** argv) {
    const operation* found_op = nullptr;
    if (std::strcmp(argv[1], "--help") != 0 && std::strcmp(argv[1], "-h") != 0) {
        found_op = &tools::utils::get_selected_operation(argc, argv, operations, "operation");
    }

    app_template::seastar_options app_cfg;
    app_cfg.name = app_name;

    const auto description_template =
R"(scylla-sstable - a multifunctional command-line tool to examine the content of sstables.

Usage: scylla sstable {{operation}} [--option1] [--option2] ... [{{sstable_path1}}] [{{sstable_path2}}] ...

Contains various tools (operations) to examine or produce sstables.

# Operations

The operation to execute is the mandatory, first positional argument.
Operations write their output to stdout, or file(s). Logs are written to
stderr, with a logger called {}.

The supported operations are:
{}

For more details on an operation, run: scylla sstable {{operation}} --help

# Sstables

Operations that read sstables, take the sstables to-be-examined
as positional command line arguments. Sstables will be processed by the
selected operation one-by-one. Any number of sstables can be passed but
mind the open file limits and the memory consumption. Always pass the
path to the data component of the sstables (*-Data.db) even if you want
to examine another component.
NOTE: currently you have to prefix dir local paths with `./`.

# Schema

To be able to interpret the sstables, their schema is required. There
are multiple ways to obtain the schema:
* system schema
* schema file

## System schema

If the examined sstables belong to a system table, whose schema is
hardcoded in scylla (and thus known), it is enough to provide just
the name of said table in the `keyspace.table` notation, via the
`--system-schema` command line option. The table has to be from one of
the following system keyspaces:
* system
* system_schema
* system_distributed
* system_distributed_everywhere

## Schema file

The schema to read the sstables is read from a schema.cql file. This
should contain the keyspace and table definitions, any UDTs used and
dropped columns in the form of relevant CQL statements. The keyspace
definition is allowed to be missing, in which case one will be
auto-generated. Dropped columns should be present in the form of insert
statements into the system_schema.dropped_columns table.
Example scylla.cql:

    CREATE KEYSPACE ks WITH replication = {{'class': 'SimpleStrategy', 'replication_factor': 1}};

    CREATE TYPE ks.type1 (f1 int, f2 text);

    CREATE TABLE ks.cf (pk int PRIMARY KEY, v frozen<type1>);

    INSERT
    INTO system_schema.dropped_columns (keyspace_name, table_name, column_name, dropped_time, type)
    VALUES ('ks', 'cf', 'v2', 1631011979170675, 'int');

In general you should be able to use the output of `DESCRIBE TABLE` or
the relevant parts of `DESCRIBE KEYSPACE` of `cqlsh` as well as the
`schema.cql` produced by snapshots.

# Examples

Dump the content of the sstable:
$ scylla sstable dump-data /path/to/md-123456-big-Data.db

Dump the content of the two sstable(s) as a unified stream:
$ scylla sstable dump-data --merge /path/to/md-123456-big-Data.db /path/to/md-123457-big-Data.db

Generate a joint histogram for the specified partition:
$ scylla sstable writetime-histogram --partition={{myhexpartitionkey}} /path/to/md-123456-big-Data.db

Validate the specified sstables:
$ scylla sstable validate /path/to/md-123456-big-Data.db /path/to/md-123457-big-Data.db


)";

    if (found_op) {
        app_cfg.description = format("{}\n\n{}\n", found_op->summary(), found_op->description());
    } else  {
        app_cfg.description = format(description_template, app_name, boost::algorithm::join(operations | boost::adaptors::transformed([] (const auto& op) {
            return format("* {}: {}", op.name(), op.summary());
        }), "\n"));
    }

    tools::utils::configure_tool_mode(app_cfg, sst_log.name());

    app_template app(std::move(app_cfg));

    app.add_options()
        ("schema-file", bpo::value<sstring>()->default_value("schema.cql"), "file containing the schema description")
        ("system-schema", bpo::value<sstring>(), "table has to be a system table, name has to be in `keyspace.table` notation")
        ;
    app.add_positional_options({
        {"sstables", bpo::value<std::vector<sstring>>(), "sstable(s) to process for operations that have sstable inputs, can also be provided as positional arguments", -1},
    });

    if (found_op) {
        bpo::options_description op_desc(found_op->name());
        for (const auto& opt_name : found_op->available_options()) {
            auto it = std::find_if(all_options.begin(), all_options.end(), [&] (const option& opt) { return opt.name() == opt_name; });
            assert(it != all_options.end());
            it->add_option(op_desc);
        }
        if (!found_op->available_options().empty()) {
            app.get_options_description().add(op_desc);
        }
    }

    return app.run(argc, argv, [&app, found_op] {
        return async([&app, found_op] {
            logalloc::use_standard_allocator_segment_pool_backend(100 * 1024 * 1024).get();

            auto& app_config = app.configuration();

            const auto& operation = *found_op;

            schema_ptr schema;
            std::string schema_source_opt;
            try {
                if (auto it = app_config.find("system-schema"); it != app_config.end()) {
                    schema_source_opt = "system-schema";
                    std::vector<sstring> comps;
                    boost::split(comps, app_config["system-schema"].as<sstring>(), boost::is_any_of("."));
                    schema = tools::load_system_schema(comps.at(0), comps.at(1));
                } else {
                    schema_source_opt = "schema-file";
                    schema = tools::load_one_schema_from_file(std::filesystem::path(app_config["schema-file"].as<sstring>())).get();
                }
            } catch (...) {
                fmt::print(std::cerr, "error: could not load {} '{}': {}\n", schema_source_opt, app_config[schema_source_opt].as<sstring>(), std::current_exception());
                return 1;
            }

            db::config dbcfg;
            gms::feature_service feature_service(gms::feature_config_from_db_config(dbcfg));
            cache_tracker tracker;
            dbcfg.host_id = locator::host_id::create_random_id();
            sstables::sstables_manager sst_man(large_data_handler, dbcfg, feature_service, tracker, memory::stats().total_memory());
            auto close_sst_man = deferred_close(sst_man);

            std::vector<sstables::shared_sstable> sstables;
            if (app_config.count("sstables")) {
                sstables = load_sstables(schema, sst_man, app_config["sstables"].as<std::vector<sstring>>());
            }

            reader_concurrency_semaphore rcs_sem(reader_concurrency_semaphore::no_limits{}, app_name);
            auto stop_semaphore = deferred_stop(rcs_sem);

            const auto permit = rcs_sem.make_tracking_only_permit(schema.get(), app_name, db::no_timeout);

            operation(schema, permit, sstables, sst_man, app_config);

            return 0;
        });
    });
}

} // namespace tools
