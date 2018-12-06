/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "log.hh"
#include <vector>
#include <typeinfo>
#include <limits>
#include "core/future.hh"
#include "core/future-util.hh"
#include "core/sstring.hh"
#include "core/fstream.hh"
#include "core/shared_ptr.hh"
#include "core/do_with.hh"
#include "core/thread.hh"
#include <seastar/core/shared_future.hh>
#include <seastar/core/byteorder.hh>
#include <iterator>

#include "types.hh"
#include "m_format_write_helpers.hh"
#include "m_format_read_helpers.hh"
#include "sstables.hh"
#include "progress_monitor.hh"
#include "compress.hh"
#include "unimplemented.hh"
#include "index_reader.hh"
#include "remove.hh"
#include "memtable.hh"
#include "range.hh"
#include "downsampling.hh"
#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm_ext/insert.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/range/algorithm/set_algorithm.hpp>
#include <boost/range/algorithm_ext/is_sorted.hpp>
#include <regex>
#include <core/align.hh>
#include "range_tombstone_list.hh"
#include "counters.hh"
#include "binary_search.hh"
#include "utils/bloom_filter.hh"
#include "utils/memory_data_sink.hh"

#include "checked-file-impl.hh"
#include "integrity_checked_file_impl.hh"
#include "service/storage_service.hh"
#include "db/extensions.hh"
#include "unimplemented.hh"
#include "vint-serialization.hh"
#include "db/large_partition_handler.hh"
#include "sstables/random_access_reader.hh"
#include <boost/algorithm/string/predicate.hpp>

thread_local disk_error_signal_type sstable_read_error;
thread_local disk_error_signal_type sstable_write_error;

namespace sstables {

logging::logger sstlog("sstable");

static const db::config& get_config();

// Because this is a noop and won't hold any state, it is better to use a global than a
// thread_local. It will be faster, specially on non-x86.
static noop_write_monitor default_noop_write_monitor;
write_monitor& default_write_monitor() {
    return default_noop_write_monitor;
}

static noop_read_monitor default_noop_read_monitor;
read_monitor& default_read_monitor() {
    return default_noop_read_monitor;
}

static no_read_monitoring noop_read_monitor_generator;
read_monitor_generator& default_read_monitor_generator() {
    return noop_read_monitor_generator;
}

static future<file> open_sstable_component_file(const io_error_handler& error_handler, sstring name, open_flags flags,
        file_open_options options) {
    if (flags != open_flags::ro && get_config().enable_sstable_data_integrity_check()) {
        return open_integrity_checked_file_dma(name, flags, options).then([&error_handler] (auto f) {
            return make_checked_file(error_handler, std::move(f));
        });
    }
    return open_checked_file_dma(error_handler, name, flags, options);
}

static future<file> open_sstable_component_file_non_checked(sstring name, open_flags flags, file_open_options options) {
    if (flags != open_flags::ro && get_config().enable_sstable_data_integrity_check()) {
        return open_integrity_checked_file_dma(name, flags, options);
    }
    return open_file_dma(name, flags, options);
}

future<file> new_sstable_component_file(const io_error_handler& error_handler, sstring name, open_flags flags,
        file_open_options options = {}) {
    return open_sstable_component_file(error_handler, name, flags, options).handle_exception([name] (auto ep) {
        sstlog.error("Could not create SSTable component {}. Found exception: {}", name, ep);
        return make_exception_future<file>(ep);
    });
}

future<file> new_sstable_component_file_non_checked(sstring name, open_flags flags, file_open_options options = {}) {
    return open_sstable_component_file_non_checked(name, flags, options).handle_exception([name] (auto ep) {
        sstlog.error("Could not create SSTable component {}. Found exception: {}", name, ep);
        return make_exception_future<file>(ep);
    });
}

utils::phased_barrier& background_jobs() {
    static thread_local utils::phased_barrier gate;
    return gate;
}

future<> await_background_jobs() {
    sstlog.debug("Waiting for background jobs");
    return background_jobs().advance_and_await().finally([] {
        sstlog.debug("Waiting done");
    });
}

future<> await_background_jobs_on_all_shards() {
    return smp::invoke_on_all([] {
        return await_background_jobs();
    });
}

shared_sstable
make_sstable(schema_ptr schema, sstring dir, int64_t generation, sstable_version_types v, sstable_format_types f, gc_clock::time_point now,
            io_error_handler_gen error_handler_gen, size_t buffer_size) {
    return make_lw_shared<sstable>(std::move(schema), std::move(dir), generation, v, f, now, std::move(error_handler_gen), buffer_size);
}

std::unordered_map<sstable::version_types, sstring, enum_hash<sstable::version_types>> sstable::_version_string = {
    { sstable::version_types::ka , "ka" },
    { sstable::version_types::la , "la" },
    { sstable::version_types::mc , "mc" },
};

std::unordered_map<sstable::format_types, sstring, enum_hash<sstable::format_types>> sstable::_format_string = {
    { sstable::format_types::big , "big" }
};

// This assumes that the mappings are small enough, and called unfrequent
// enough.  If that changes, it would be adviseable to create a full static
// reverse mapping, even if it is done at runtime.
template <typename Map>
static typename Map::key_type reverse_map(const typename Map::mapped_type& value, Map& map) {
    for (auto& pair: map) {
        if (pair.second == value) {
            return pair.first;
        }
    }
    throw std::out_of_range("unable to reverse map");
}

// This should be used every time we use read_exactly directly.
//
// read_exactly is a lot more convenient of an interface to use, because we'll
// be parsing known quantities.
//
// However, anything other than the size we have asked for, is certainly a bug,
// and we need to do something about it.
static void check_buf_size(temporary_buffer<char>& buf, size_t expected) {
    if (buf.size() < expected) {
        throw bufsize_mismatch_exception(buf.size(), expected);
    }
}

template <typename T, typename U>
static void check_truncate_and_assign(T& to, const U from) {
    static_assert(std::is_integral<T>::value && std::is_integral<U>::value, "T and U must be integral");
    to = from;
    if (to != from) {
        throw std::overflow_error("assigning U to T caused an overflow");
    }
}

// Base parser, parses an integer type
template <typename T>
typename std::enable_if_t<std::is_integral<T>::value, void>
read_integer(temporary_buffer<char>& buf, T& i) {
    auto *nr = reinterpret_cast<const net::packed<T> *>(buf.get());
    i = net::ntoh(*nr);
}

template <typename T>
typename std::enable_if_t<std::is_integral<T>::value, future<>>
parse(sstable_version_types v, random_access_reader& in, T& i) {
    return in.read_exactly(sizeof(T)).then([&i] (auto buf) {
        check_buf_size(buf, sizeof(T));

        read_integer(buf, i);
        return make_ready_future<>();
    });
}

template <typename T>
inline typename std::enable_if_t<std::is_integral<T>::value, void>
write(sstable_version_types v, file_writer& out, T i) {
    auto *nr = reinterpret_cast<const net::packed<T> *>(&i);
    i = net::hton(*nr);
    auto p = reinterpret_cast<const char*>(&i);
    out.write(p, sizeof(T)).get();
}

template <typename T>
typename std::enable_if_t<std::is_enum<T>::value, future<>>
parse(sstable_version_types v, random_access_reader& in, T& i) {
    return parse(v, in, reinterpret_cast<typename std::underlying_type<T>::type&>(i));
}

template <typename T>
inline typename std::enable_if_t<std::is_enum<T>::value, void>
write(sstable_version_types v, file_writer& out, T i) {
    write(v, out, static_cast<typename std::underlying_type<T>::type>(i));
}

future<> parse(sstable_version_types v, random_access_reader& in, bool& i) {
    return parse(v, in, reinterpret_cast<uint8_t&>(i));
}

inline void write(sstable_version_types v, file_writer& out, bool i) {
    write(v, out, static_cast<uint8_t>(i));
}

template <typename To, typename From>
static inline To convert(From f) {
    static_assert(sizeof(To) == sizeof(From), "Sizes must match");
    union {
        To to;
        From from;
    } conv;

    conv.from = f;
    return conv.to;
}

future<> parse(sstable_version_types, random_access_reader& in, double& d) {
    return in.read_exactly(sizeof(double)).then([&d] (auto buf) {
        check_buf_size(buf, sizeof(double));

        auto *nr = reinterpret_cast<const net::packed<unsigned long> *>(buf.get());
        d = convert<double>(net::ntoh(*nr));
        return make_ready_future<>();
    });
}

inline void write(sstable_version_types v, file_writer& out, double d) {
    auto *nr = reinterpret_cast<const net::packed<unsigned long> *>(&d);
    auto tmp = net::hton(*nr);
    auto p = reinterpret_cast<const char*>(&tmp);
    out.write(p, sizeof(unsigned long)).get();
}

template <typename T>
future<> parse(sstable_version_types, random_access_reader& in, T& len, bytes& s) {
    return in.read_exactly(len).then([&s, len] (auto buf) {
        check_buf_size(buf, len);
        // Likely a different type of char. Most bufs are unsigned, whereas the bytes type is signed.
        s = bytes(reinterpret_cast<const bytes::value_type *>(buf.get()), len);
    });
}

inline void write(sstable_version_types v, file_writer& out, const bytes& s) {
    out.write(s).get();
}

inline void write(sstable_version_types v, file_writer& out, bytes_view s) {
    out.write(reinterpret_cast<const char*>(s.data()), s.size()).get();
}

inline void write(sstable_version_types v, file_writer& out, bytes_ostream s) {
    for (bytes_view fragment : s) {
        write(v, out, fragment);
    }
}

// All composite parsers must come after this
template<typename First, typename... Rest>
future<> parse(sstable_version_types v, random_access_reader& in, First& first, Rest&&... rest) {
    return parse(v, in, first).then([v, &in, &rest...] {
        return parse(v, in, std::forward<Rest>(rest)...);
    });
}

template<typename First, typename... Rest>
inline void write(sstable_version_types v, file_writer& out, const First& first, Rest&&... rest) {
    write(v, out, first);
    write(v, out, std::forward<Rest>(rest)...);
}

// Intended to be used for a type that describes itself through describe_type().
template <class T>
typename std::enable_if_t<!std::is_integral<T>::value && !std::is_enum<T>::value, future<>>
parse(sstable_version_types v, random_access_reader& in, T& t) {
    return t.describe_type(v, [v, &in] (auto&&... what) -> future<> {
        return parse(v, in, what...);
    });
}

template <class T>
inline void write(sstable_version_types v, file_writer& out, const vint<T>& t) {
    write_vint(out, t.value);
}

template <class T>
future<> parse(sstable_version_types v, random_access_reader& in, vint<T>& t) {
    return read_vint(in, t.value);
}

template <class T>
inline typename std::enable_if_t<!std::is_integral<T>::value && !std::is_enum<T>::value, void>
write(sstable_version_types v, file_writer& out, const T& t) {
    // describe_type() is not const correct, so cheat here:
    const_cast<T&>(t).describe_type(v, [v, &out] (auto&&... what) -> void {
        write(v, out, std::forward<decltype(what)>(what)...);
    });
}

// For all types that take a size, we provide a template that takes the type
// alone, and another, separate one, that takes a size parameter as well, of
// type Size. This is because although most of the time the size and the data
// are contiguous, it is not always the case. So we want to have the
// flexibility of parsing them separately.
template <typename Size>
future<> parse(sstable_version_types v, random_access_reader& in, disk_string<Size>& s) {
    auto len = std::make_unique<Size>();
    auto f = parse(v, in, *len);
    return f.then([v, &in, &s, len = std::move(len)] {
        return parse(v, in, *len, s.value);
    });
}

future<> parse(sstable_version_types v, random_access_reader& in, disk_string_vint_size& s) {
    auto len = std::make_unique<uint64_t>();
    auto f = read_vint(in, *len);
    return f.then([v, &in, &s, len = std::move(len)] {
        return parse(v, in, *len, s.value);
    });
}

template <typename Members>
future<> parse(sstable_version_types v, random_access_reader& in, disk_array_vint_size<Members>& arr) {
    auto len = std::make_unique<uint64_t>();
    auto f = read_vint(in, *len);
    return f.then([v, &in, &arr, len = std::move(len)] {
        return parse(v, in, *len, arr.elements);
    });
}

template <typename Size>
inline void write(sstable_version_types v, file_writer& out, const disk_string<Size>& s) {
    Size len = 0;
    check_truncate_and_assign(len, s.value.size());
    write(v, out, len);
    write(v, out, s.value);
}

inline void write(sstable_version_types v, file_writer& out, const disk_string_vint_size& s) {
    uint64_t len = 0;
    check_truncate_and_assign(len, s.value.size());
    write_vint(out, len);
    write(v, out, s.value);
}

template <typename Size>
inline void write(sstable_version_types v, file_writer& out, const disk_string_view<Size>& s) {
    Size len;
    check_truncate_and_assign(len, s.value.size());
    write(v, out, len, s.value);
}

template<typename SizeType>
inline void write(sstable_version_types ver, file_writer& out, const disk_data_value_view<SizeType>& v) {
    SizeType length;
    check_truncate_and_assign(length, v.value.size_bytes());
    write(ver, out, length);
    using boost::range::for_each;
    for_each(v.value, [&] (bytes_view fragment) { write(ver, out, fragment); });
}

// We cannot simply read the whole array at once, because we don't know its
// full size. We know the number of elements, but if we are talking about
// disk_strings, for instance, we have no idea how much of the stream each
// element will take.
//
// Sometimes we do know the size, like the case of integers. There, all we have
// to do is to convert each member because they are all stored big endian.
// We'll offer a specialization for that case below.
template <typename Size, typename Members>
typename std::enable_if_t<!std::is_integral<Members>::value, future<>>
parse(sstable_version_types v, random_access_reader& in, Size& len, utils::chunked_vector<Members>& arr) {

    auto count = make_lw_shared<size_t>(0);
    auto eoarr = [count, len] { return *count == len; };

    return do_until(eoarr, [v, count, &in, &arr] {
        arr.emplace_back();
        (*count)++;
        return parse(v, in, arr.back());
    });
}

template <typename Size, typename Members>
typename std::enable_if_t<std::is_integral<Members>::value, future<>>
parse(sstable_version_types, random_access_reader& in, Size& len, utils::chunked_vector<Members>& arr) {
    auto done = make_lw_shared<size_t>(0);
    return repeat([&in, &len, &arr, done]  {
        auto now = std::min(len - *done, 100000 / sizeof(Members));
        return in.read_exactly(now * sizeof(Members)).then([&arr, len, now, done] (auto buf) {
            check_buf_size(buf, now * sizeof(Members));

            auto *nr = reinterpret_cast<const net::packed<Members> *>(buf.get());
            for (size_t i = 0; i < now; ++i) {
                arr.push_back(net::ntoh(nr[i]));
            }
            *done += now;
            return make_ready_future<stop_iteration>(*done == len ? stop_iteration::yes : stop_iteration::no);
        });
    });
}

// We resize the array here, before we pass it to the integer / non-integer
// specializations
template <typename Size, typename Members>
future<> parse(sstable_version_types v, random_access_reader& in, disk_array<Size, Members>& arr) {
    auto len = make_lw_shared<Size>();
    auto f = parse(v, in, *len);
    return f.then([v, &in, &arr, len] {
        arr.elements.reserve(*len);
        return parse(v, in, *len, arr.elements);
    }).finally([len] {});
}

template <typename Members>
inline typename std::enable_if_t<!std::is_integral<Members>::value, void>
write(sstable_version_types v, file_writer& out, const utils::chunked_vector<Members>& arr) {
    for (auto& a : arr) {
        write(v, out, a);
    }
}

template <typename Members>
inline typename std::enable_if_t<std::is_integral<Members>::value, void>
write(sstable_version_types v, file_writer& out, const utils::chunked_vector<Members>& arr) {
    std::vector<Members> tmp;
    size_t per_loop = 100000 / sizeof(Members);
    tmp.resize(per_loop);
    size_t idx = 0;
    while (idx != arr.size()) {
        auto now = std::min(arr.size() - idx, per_loop);
        // copy arr into tmp converting each entry into big-endian representation.
        auto nr = arr.begin() + idx;
        for (size_t i = 0; i < now; i++) {
            tmp[i] = net::hton(nr[i]);
        }
        auto p = reinterpret_cast<const char*>(tmp.data());
        auto bytes = now * sizeof(Members);
        out.write(p, bytes).get();
        idx += now;
    }
}

template <typename Size, typename Members>
inline void write(sstable_version_types v, file_writer& out, const disk_array<Size, Members>& arr) {
    Size len = 0;
    check_truncate_and_assign(len, arr.elements.size());
    write(v, out, len);
    write(v, out, arr.elements);
}

template <typename Members>
inline void write(sstable_version_types v, file_writer& out, const disk_array_vint_size<Members>& arr) {
    uint64_t len = 0;
    check_truncate_and_assign(len, arr.elements.size());
    write_vint(out, len);
    write(v, out, arr.elements);
}

template <typename Size, typename Members>
inline void write(sstable_version_types v, file_writer& out, const disk_array_ref<Size, Members>& arr) {
    Size len = 0;
    check_truncate_and_assign(len, arr.elements.size());
    write(v, out, len);
    write(v, out, arr.elements);
}

template <typename Size, typename Key, typename Value>
future<> parse(sstable_version_types v, random_access_reader& in, Size& len, std::unordered_map<Key, Value>& map) {
    return do_with(Size(), [v, &in, len, &map] (Size& count) {
        auto eos = [len, &count] { return len == count++; };
        return do_until(eos, [v, len, &in, &map] {
            struct kv {
                Key key;
                Value value;
            };

            return do_with(kv(), [v, &in, &map] (auto& el) {
                return parse(v, in, el.key, el.value).then([&el, &map] {
                    map.emplace(el.key, el.value);
                });
            });
        });
    });
}

template <typename First, typename Second>
future<> parse(sstable_version_types v, random_access_reader& in, std::pair<First, Second>& p) {
    return parse(v, in, p.first, p.second);
}

template <typename Size, typename Key, typename Value>
future<> parse(sstable_version_types v, random_access_reader& in, disk_hash<Size, Key, Value>& h) {
    auto w = std::make_unique<Size>();
    auto f = parse(v, in, *w);
    return f.then([v, &in, &h, w = std::move(w)] {
        return parse(v, in, *w, h.map);
    });
}

template <typename Key, typename Value>
inline void write(sstable_version_types v, file_writer& out, const std::unordered_map<Key, Value>& map) {
    for (auto& val: map) {
        write(v, out, val.first, val.second);
    };
}

template <typename First, typename Second>
inline void write(sstable_version_types v, file_writer& out, const std::pair<First, Second>& val) {
    write(v, out, val.first, val.second);
}

template <typename Size, typename Key, typename Value>
inline void write(sstable_version_types v, file_writer& out, const disk_hash<Size, Key, Value>& h) {
    Size len = 0;
    check_truncate_and_assign(len, h.map.size());
    write(v, out, len);
    write(v, out, h.map);
}

// Abstract parser/sizer/writer for a single tagged member of a tagged union
template <typename DiskSetOfTaggedUnion>
struct single_tagged_union_member_serdes {
    using value_type = typename DiskSetOfTaggedUnion::value_type;
    virtual ~single_tagged_union_member_serdes() {}
    virtual future<> do_parse(sstable_version_types version, random_access_reader& in, value_type& v) const = 0;
    virtual uint32_t do_size(sstable_version_types version, const value_type& v) const = 0;
    virtual void do_write(sstable_version_types version, file_writer& out, const value_type& v) const = 0;
};

// Concrete parser for a single member of a tagged union; parses type "Member"
template <typename DiskSetOfTaggedUnion, typename Member>
struct single_tagged_union_member_serdes_for final : single_tagged_union_member_serdes<DiskSetOfTaggedUnion> {
    using base = single_tagged_union_member_serdes<DiskSetOfTaggedUnion>;
    using value_type = typename base::value_type;
    virtual future<> do_parse(sstable_version_types version, random_access_reader& in, value_type& v) const {
        v = Member();
        return parse(version, in, boost::get<Member>(v).value);
    }
    virtual uint32_t do_size(sstable_version_types version, const value_type& v) const override {
        return serialized_size(version, boost::get<Member>(v).value);
    }
    virtual void do_write(sstable_version_types version, file_writer& out, const value_type& v) const override {
        write(version, out, boost::get<Member>(v).value);
    }
};

template <typename TagType, typename... Members>
struct disk_set_of_tagged_union<TagType, Members...>::serdes {
    using disk_set = disk_set_of_tagged_union<TagType, Members...>;
    // We can't use unique_ptr, because we initialize from an std::intializer_list, which is not move compatible.
    using serdes_map_type = std::unordered_map<TagType, shared_ptr<single_tagged_union_member_serdes<disk_set>>, typename disk_set::hash_type>;
    using value_type = typename disk_set::value_type;
    serdes_map_type map = {
        {Members::tag(), make_shared<single_tagged_union_member_serdes_for<disk_set, Members>>()}...
    };
    future<> lookup_and_parse(sstable_version_types v, random_access_reader& in, TagType tag, uint32_t& size, disk_set& s, value_type& value) const {
        auto i = map.find(tag);
        if (i == map.end()) {
            return in.read_exactly(size).discard_result();
        } else {
            return i->second->do_parse(v, in, value).then([tag, &s, &value] () mutable {
                s.data.emplace(tag, std::move(value));
            });
        }
    }
    uint32_t lookup_and_size(sstable_version_types v, TagType tag, const value_type& value) const {
        return map.at(tag)->do_size(v, value);
    }
    void lookup_and_write(sstable_version_types v, file_writer& out, TagType tag, const value_type& value) const {
        return map.at(tag)->do_write(v, out, value);
    }
};

template <typename TagType, typename... Members>
typename disk_set_of_tagged_union<TagType, Members...>::serdes disk_set_of_tagged_union<TagType, Members...>::s_serdes;

template <typename TagType, typename... Members>
future<>
parse(sstable_version_types v, random_access_reader& in, disk_set_of_tagged_union<TagType, Members...>& s) {
    using disk_set = disk_set_of_tagged_union<TagType, Members...>;
    using key_type = typename disk_set::key_type;
    using value_type = typename disk_set::value_type;
    return do_with(0u, 0u, 0u, value_type{}, [&] (key_type& nr_elements, key_type& new_key, unsigned& new_size, value_type& new_value) {
        return parse(v, in, nr_elements).then([&, v] {
            auto rng = boost::irange<key_type>(0, nr_elements); // do_for_each doesn't like an rvalue range
            return do_for_each(rng.begin(), rng.end(), [&, v] (key_type ignore) {
                return parse(v, in, new_key).then([&, v] {
                    return parse(v, in, new_size).then([&, v] {
                        return disk_set::s_serdes.lookup_and_parse(v, in, TagType(new_key), new_size, s, new_value);
                    });
                });
            });
        });
    });
}

template <typename TagType, typename... Members>
void write(sstable_version_types v, file_writer& out, const disk_set_of_tagged_union<TagType, Members...>& s) {
    using disk_set = disk_set_of_tagged_union<TagType, Members...>;
    write(v, out, uint32_t(s.data.size()));
    for (auto&& kv : s.data) {
        auto&& tag = kv.first;
        auto&& value = kv.second;
        write(v, out, tag);
        write(v, out, uint32_t(disk_set::s_serdes.lookup_and_size(v, tag, value)));
        disk_set::s_serdes.lookup_and_write(v, out, tag, value);
    }
}

future<> parse(sstable_version_types v, random_access_reader& in, summary& s) {
    using pos_type = typename decltype(summary::positions)::value_type;

    return parse(v, in, s.header.min_index_interval,
                     s.header.size,
                     s.header.memory_size,
                     s.header.sampling_level,
                     s.header.size_at_full_sampling).then([v, &in, &s] {
        return in.read_exactly(s.header.size * sizeof(pos_type)).then([&in, &s] (auto buf) {
            auto len = s.header.size * sizeof(pos_type);
            check_buf_size(buf, len);

            // Positions are encoded in little-endian.
            auto b = buf.get();
            s.positions = utils::chunked_vector<pos_type>();
            return do_until([&s] { return s.positions.size() == s.header.size; }, [&s, buf = std::move(buf), b] () mutable {
                s.positions.push_back(seastar::read_le<pos_type>(b));
                b += sizeof(pos_type);
                return make_ready_future<>();
            }).then([&s] {
                // Since the keys in the index are not sized, we need to calculate
                // the start position of the index i+1 to determine the boundaries
                // of index i. The "memory_size" field in the header determines the
                // total memory used by the map, so if we push it to the vector, we
                // can guarantee that no conditionals are used, and we can always
                // query the position of the "next" index.
                s.positions.push_back(s.header.memory_size);
                return make_ready_future<>();
            });
        }).then([v, &in, &s] {
            in.seek(sizeof(summary::header) + s.header.memory_size);
            return parse(v, in, s.first_key, s.last_key);
        }).then([&in, &s] {
            in.seek(s.positions[0] + sizeof(summary::header));
            s.entries.reserve(s.header.size);

            return do_with(int(0), [&in, &s] (int& idx) mutable {
                return do_until([&s] { return s.entries.size() == s.header.size; }, [&s, &in, &idx] () mutable {
                    auto pos = s.positions[idx++];
                    auto next = s.positions[idx];

                    auto entrysize = next - pos;
                    return in.read_exactly(entrysize).then([&s, entrysize] (auto buf) mutable {
                        check_buf_size(buf, entrysize);

                        auto keysize = entrysize - 8;
                        auto key_data = s.add_summary_data(bytes_view(reinterpret_cast<const int8_t*>(buf.get()), keysize));
                        buf.trim_front(keysize);

                        // position is little-endian encoded
                        auto position = seastar::read_le<uint64_t>(buf.get());
                        auto token = dht::global_partitioner().get_token(key_view(key_data));
                        auto token_data = s.add_summary_data(bytes_view(token._data));
                        s.entries.push_back({ dht::token_view(dht::token::kind::key, token_data), key_data, position });
                        return make_ready_future<>();
                    });
                });
            }).then([&s] {
                // Delete last element which isn't part of the on-disk format.
                s.positions.pop_back();
            });
        });
    });
}

inline void write(sstable_version_types v, file_writer& out, const summary_entry& entry) {
    // FIXME: summary entry is supposedly written in memory order, but that
    // would prevent portability of summary file between machines of different
    // endianness. We can treat it as little endian to preserve portability.
    write(v, out, entry.key);
    auto p = seastar::cpu_to_le<uint64_t>(entry.position);
    out.write(reinterpret_cast<const char*>(&p), sizeof(p)).get();
}

inline void write(sstable_version_types v, file_writer& out, const summary& s) {
    // NOTE: positions and entries must be stored in LITTLE-ENDIAN.
    write(v, out, s.header.min_index_interval,
                  s.header.size,
                  s.header.memory_size,
                  s.header.sampling_level,
                  s.header.size_at_full_sampling);
    for (auto&& e : s.positions) {
        auto p = seastar::cpu_to_le(e);
        out.write(reinterpret_cast<const char*>(&p), sizeof(p)).get();
    }
    write(v, out, s.entries);
    write(v, out, s.first_key, s.last_key);
}

future<summary_entry&> sstable::read_summary_entry(size_t i) {
    // The last one is the boundary marker
    if (i >= (_components->summary.entries.size())) {
        throw std::out_of_range(sprint("Invalid Summary index: %ld", i));
    }

    return make_ready_future<summary_entry&>(_components->summary.entries[i]);
}

future<> parse(sstable_version_types v, random_access_reader& in, deletion_time& d) {
    return parse(v, in, d.local_deletion_time, d.marked_for_delete_at);
}

template <typename Child>
future<> parse(sstable_version_types v, random_access_reader& in, std::unique_ptr<metadata>& p) {
    p.reset(new Child);
    return parse(v, in, *static_cast<Child *>(p.get()));
}

template <typename Child>
inline void write(sstable_version_types v, file_writer& out, const std::unique_ptr<metadata>& p) {
    write(v, out, *static_cast<Child *>(p.get()));
}

future<> parse(sstable_version_types v, random_access_reader& in, statistics& s) {
    return parse(v, in, s.offsets).then([v, &in, &s] {
        // Old versions of Scylla do not respect the order.
        // See https://github.com/scylladb/scylla/issues/3937
        boost::sort(s.offsets.elements, [] (auto&& e1, auto&& e2) { return e1.first < e2.first; });
        return do_for_each(s.offsets.elements.begin(), s.offsets.elements.end(), [v, &in, &s] (auto val) mutable {
            in.seek(val.second);

            switch (val.first) {
                case metadata_type::Validation:
                    return parse<validation_metadata>(v, in, s.contents[val.first]);
                case metadata_type::Compaction:
                    return parse<compaction_metadata>(v, in, s.contents[val.first]);
                case metadata_type::Stats:
                    return parse<stats_metadata>(v, in, s.contents[val.first]);
                case metadata_type::Serialization:
                    if (v != sstable_version_types::mc) {
                        throw std::runtime_error(
                            "Statistics is malformed: SSTable is in 2.x format but contains serialization header.");
                    } else {
                        return parse<serialization_header>(v, in, s.contents[val.first]);
                    }
                    return make_ready_future<>();
                default:
                    sstlog.warn("Invalid metadata type at Statistics file: {} ", int(val.first));
                    return make_ready_future<>();
                }
        });
    });
}

inline void write(sstable_version_types v, file_writer& out, const statistics& s) {
    write(v, out, s.offsets);
    for (auto&& e : s.offsets.elements) {
        s.contents.at(e.first)->write(v, out);
    }
}

future<> parse(sstable_version_types v, random_access_reader& in, utils::estimated_histogram& eh) {
    auto len = std::make_unique<uint32_t>();

    auto f = parse(v, in, *len);
    return f.then([&in, &eh, len = std::move(len)] {
        uint32_t length = *len;

        if (length == 0) {
            throw malformed_sstable_exception("Estimated histogram with zero size found. Can't continue!");
        }

        // Arrays are potentially pre-initialized by the estimated_histogram constructor.
        eh.bucket_offsets.clear();
        eh.buckets.clear();

        eh.bucket_offsets.reserve(length - 1);
        eh.buckets.reserve(length);

        auto type_size = sizeof(uint64_t) * 2;
        return in.read_exactly(length * type_size).then([&eh, length, type_size] (auto&& buf) {
            check_buf_size(buf, length * type_size);

            return do_with(size_t(0), std::move(buf), [&eh, length] (size_t& j, auto& buf) mutable {
                auto *nr = reinterpret_cast<const net::packed<uint64_t> *>(buf.get());
                return do_until([&eh, length] { return eh.buckets.size() == length; }, [nr, &eh, &j] () mutable {
                    auto offset = net::ntoh(nr[j++]);
                    auto bucket = net::ntoh(nr[j++]);
                    if (eh.buckets.size() > 0) {
                        eh.bucket_offsets.push_back(offset);
                    }
                    eh.buckets.push_back(bucket);
                    return make_ready_future<>();
                });
            });
        });
    });
}

inline void write(sstable_version_types v, file_writer& out, const utils::estimated_histogram& eh) {
    uint32_t len = 0;
    check_truncate_and_assign(len, eh.buckets.size());

    write(v, out, len);
    struct element {
        uint64_t offsets;
        uint64_t buckets;
    };
    std::vector<element> elements;
    elements.reserve(eh.buckets.size());

    auto *offsets_nr = reinterpret_cast<const net::packed<uint64_t> *>(eh.bucket_offsets.data());
    auto *buckets_nr = reinterpret_cast<const net::packed<uint64_t> *>(eh.buckets.data());
    for (size_t i = 0; i < eh.buckets.size(); i++) {
        auto offsets = net::hton(offsets_nr[i == 0 ? 0 : i - 1]);
        auto buckets = net::hton(buckets_nr[i]);
        elements.emplace_back(element{offsets, buckets});
        if (need_preempt()) {
            seastar::thread::yield();
        }
    }

    auto p = reinterpret_cast<const char*>(elements.data());
    auto bytes = elements.size() * sizeof(element);
    out.write(p, bytes).get();
}

struct streaming_histogram_element {
    using key_type = typename decltype(utils::streaming_histogram::bin)::key_type;
    using value_type = typename decltype(utils::streaming_histogram::bin)::mapped_type;
    key_type key;
    value_type value;

    template <typename Describer>
    auto describe_type(sstable_version_types v, Describer f) { return f(key, value); }
};

future<> parse(sstable_version_types v, random_access_reader& in, utils::streaming_histogram& sh) {
    auto a = std::make_unique<disk_array<uint32_t, streaming_histogram_element>>();

    auto f = parse(v, in, sh.max_bin_size, *a);
    return f.then([&sh, a = std::move(a)] {
        auto length = a->elements.size();
        if (length > sh.max_bin_size) {
            throw malformed_sstable_exception("Streaming histogram with more entries than allowed. Can't continue!");
        }

        // Find bad histogram which had incorrect elements merged due to use of
        // unordered map. The keys will be unordered. Histogram which size is
        // less than max allowed will be correct because no entries needed to be
        // merged, so we can avoid discarding those.
        // look for commit with title 'streaming_histogram: fix update' for more details.
        auto possibly_broken_histogram = length == sh.max_bin_size;
        auto less_comp = [] (auto& x, auto& y) { return x.key < y.key; };
        if (possibly_broken_histogram && !boost::is_sorted(a->elements, less_comp)) {
            return make_ready_future<>();
        }

        auto transform = [] (auto element) -> std::pair<streaming_histogram_element::key_type, streaming_histogram_element::value_type> {
            return { element.key, element.value };
        };
        boost::copy(a->elements | boost::adaptors::transformed(transform), std::inserter(sh.bin, sh.bin.end()));

        return make_ready_future<>();
    });
}

inline void write(sstable_version_types v, file_writer& out, const utils::streaming_histogram& sh) {
    uint32_t max_bin_size;
    check_truncate_and_assign(max_bin_size, sh.max_bin_size);

    disk_array<uint32_t, streaming_histogram_element> a;
    a.elements = boost::copy_range<utils::chunked_vector<streaming_histogram_element>>(sh.bin
        | boost::adaptors::transformed([&] (auto& kv) { return streaming_histogram_element{kv.first, kv.second}; }));

    write(v, out, max_bin_size, a);
}

future<> parse(sstable_version_types v, random_access_reader& in, commitlog_interval& ci) {
    return parse(v, in, ci.start).then([&ci, v, &in] {
        return parse(v, in, ci.end);
    });
}

inline void write(sstable_version_types v, file_writer& out, const commitlog_interval& ci) {
    write(v, out, ci.start);
    write(v, out, ci.end);
}

future<> parse(sstable_version_types v, random_access_reader& in, compression& c) {
    auto data_len_ptr = make_lw_shared<uint64_t>(0);
    auto chunk_len_ptr = make_lw_shared<uint32_t>(0);

    return parse(v, in, c.name, c.options, *chunk_len_ptr, *data_len_ptr).then([v, &in, &c, chunk_len_ptr, data_len_ptr] {
        c.set_uncompressed_chunk_length(*chunk_len_ptr);
        c.set_uncompressed_file_length(*data_len_ptr);

      return do_with(uint32_t(), c.offsets.get_writer(), [v, &in, &c] (uint32_t& len, compression::segmented_offsets::writer& offsets) {
        return parse(v, in, len).then([&in, &c, &len, &offsets] {
            auto eoarr = [&c, &len] { return c.offsets.size() == len; };

            return do_until(eoarr, [&in, &c, &len, &offsets] () {
                auto now = std::min(len - c.offsets.size(), 100000 / sizeof(uint64_t));
                return in.read_exactly(now * sizeof(uint64_t)).then([&offsets, now] (auto buf) {
                    uint64_t value;
                    for (size_t i = 0; i < now; ++i) {
                        std::copy_n(buf.get() + i * sizeof(uint64_t), sizeof(uint64_t), reinterpret_cast<char*>(&value));
                        offsets.push_back(net::ntoh(value));
                    }
                });
            });
        });
      });
    });
}

void write(sstable_version_types v, file_writer& out, const compression& c) {
    write(v, out, c.name, c.options, c.uncompressed_chunk_length(), c.uncompressed_file_length());

    write(v, out, static_cast<uint32_t>(c.offsets.size()));

    std::vector<uint64_t> tmp;
    const size_t per_loop = 100000 / sizeof(uint64_t);
    tmp.resize(per_loop);
    size_t idx = 0;
    while (idx != c.offsets.size()) {
        auto now = std::min(c.offsets.size() - idx, per_loop);
        // copy offsets into tmp converting each entry into big-endian representation.
        auto nr = c.offsets.begin() + idx;
        for (size_t i = 0; i < now; i++) {
            tmp[i] = net::hton(nr[i]);
        }
        auto p = reinterpret_cast<const char*>(tmp.data());
        auto bytes = now * sizeof(uint64_t);
        out.write(p, bytes).get();
        idx += now;
    }
}

// This is small enough, and well-defined. Easier to just read it all
// at once
future<> sstable::read_toc() {
    if (_recognized_components.size()) {
        return make_ready_future<>();
    }

    auto file_path = filename(component_type::TOC);

    sstlog.debug("Reading TOC file {} ", file_path);

    return open_checked_file_dma(_read_error_handler, file_path, open_flags::ro).then([this, file_path] (file f) {
        auto bufptr = allocate_aligned_buffer<char>(4096, 4096);
        auto buf = bufptr.get();

        auto fut = f.dma_read(0, buf, 4096);
        return std::move(fut).then([this, f = std::move(f), bufptr = std::move(bufptr), file_path] (size_t size) mutable {
            // This file is supposed to be very small. Theoretically we should check its size,
            // but if we so much as read a whole page from it, there is definitely something fishy
            // going on - and this simplifies the code.
            if (size >= 4096) {
                throw malformed_sstable_exception("SSTable too big: " + to_sstring(size) + " bytes", file_path);
            }

            std::experimental::string_view buf(bufptr.get(), size);
            std::vector<sstring> comps;

            boost::split(comps , buf, boost::is_any_of("\n"));

            for (auto& c: comps) {
                // accept trailing newlines
                if (c == "") {
                    continue;
                }
                try {
                    _recognized_components.insert(reverse_map(c, sstable_version_constants::get_component_map(_version)));
                } catch (std::out_of_range& oor) {
                    _unrecognized_components.push_back(c);
                    sstlog.info("Unrecognized TOC component was found: {} in sstable {}", c, file_path);
                }
            }
            if (!_recognized_components.size()) {
                throw malformed_sstable_exception("Empty TOC", file_path);
            }
            return f.close().finally([f] {});
        });
    }).then_wrapped([file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            if (e.code() == std::error_code(ENOENT, std::system_category())) {
                throw malformed_sstable_exception(file_path + ": file not found");
            }
            throw;
        }
    });

}

void sstable::generate_toc(compressor_ptr c, double filter_fp_chance) {
    // Creating table of components.
    _recognized_components.insert(component_type::TOC);
    _recognized_components.insert(component_type::Statistics);
    _recognized_components.insert(component_type::Digest);
    _recognized_components.insert(component_type::Index);
    _recognized_components.insert(component_type::Summary);
    _recognized_components.insert(component_type::Data);
    if (filter_fp_chance != 1.0) {
        _recognized_components.insert(component_type::Filter);
    }
    if (c == nullptr) {
        _recognized_components.insert(component_type::CRC);
    } else {
        _recognized_components.insert(component_type::CompressionInfo);
    }
    _recognized_components.insert(component_type::Scylla);
}

void sstable::write_toc(const io_priority_class& pc) {
    auto file_path = filename(component_type::TemporaryTOC);

    sstlog.debug("Writing TOC file {} ", file_path);

    // Writing TOC content to temporary file.
    // If creation of temporary TOC failed, it implies that that boot failed to
    // delete a sstable with temporary for this column family, or there is a
    // sstable being created in parallel with the same generation.
    file f = new_sstable_component_file(_write_error_handler, file_path, open_flags::wo | open_flags::create | open_flags::exclusive).get0();

    bool toc_exists = file_exists(filename(component_type::TOC)).get0();
    if (toc_exists) {
        // TOC will exist at this point if write_components() was called with
        // the generation of a sstable that exists.
        f.close().get();
        remove_file(file_path).get();
        throw std::runtime_error(sprint("SSTable write failed due to existence of TOC file for generation %ld of %s.%s", _generation, _schema->ks_name(), _schema->cf_name()));
    }

    file_output_stream_options options;
    options.buffer_size = 4096;
    options.io_priority_class = pc;
    auto w = file_writer(std::move(f), std::move(options));

    for (auto&& key : _recognized_components) {
            // new line character is appended to the end of each component name.
        auto value = sstable_version_constants::get_component_map(_version).at(key) + "\n";
        bytes b = bytes(reinterpret_cast<const bytes::value_type *>(value.c_str()), value.size());
        write(_version, w, b);
    }
    w.flush().get();
    w.close().get();

    // Flushing parent directory to guarantee that temporary TOC file reached
    // the disk.
    file dir_f = open_checked_directory(_write_error_handler, _dir).get0();
    sstable_write_io_check([&] {
        dir_f.flush().get();
        dir_f.close().get();
    });
}

future<> sstable::seal_sstable() {
    // SSTable sealing is about renaming temporary TOC file after guaranteeing
    // that each component reached the disk safely.
    return open_checked_directory(_write_error_handler, _dir).then([this] (file dir_f) {
        // Guarantee that every component of this sstable reached the disk.
        return sstable_write_io_check([&] { return dir_f.flush(); }).then([this] {
            // Rename TOC because it's no longer temporary.
            return sstable_write_io_check([&] {
                return engine().rename_file(filename(component_type::TemporaryTOC), filename(component_type::TOC));
            });
        }).then([this, dir_f] () mutable {
            // Guarantee that the changes above reached the disk.
            return sstable_write_io_check([&] { return dir_f.flush(); });
        }).then([this, dir_f] () mutable {
            return sstable_write_io_check([&] { return dir_f.close(); });
        }).then([this, dir_f] {
            // If this point was reached, sstable should be safe in disk.
            sstlog.debug("SSTable with generation {} of {}.{} was sealed successfully.", _generation, _schema->ks_name(), _schema->cf_name());
        });
    });
}

void write_crc(sstable_version_types v, io_error_handler& error_handler, const sstring file_path, const checksum& c) {
    sstlog.debug("Writing CRC file {} ", file_path);

    auto oflags = open_flags::wo | open_flags::create | open_flags::exclusive;
    file f = new_sstable_component_file(error_handler, file_path, oflags).get0();

    file_output_stream_options options;
    options.buffer_size = 4096;
    auto w = file_writer(std::move(f), std::move(options));
    write(v, w, c);
    w.close().get();
}

// Digest file stores the full checksum of data file converted into a string.
void write_digest(sstable_version_types v, io_error_handler& error_handler, const sstring file_path, uint32_t full_checksum) {
    sstlog.debug("Writing Digest file {} ", file_path);

    auto oflags = open_flags::wo | open_flags::create | open_flags::exclusive;
    auto f = new_sstable_component_file(error_handler, file_path, oflags).get0();

    file_output_stream_options options;
    options.buffer_size = 4096;
    auto w = file_writer(std::move(f), std::move(options));

    auto digest = to_sstring<bytes>(full_checksum);
    write(v, w, digest);
    w.close().get();
}

thread_local std::array<std::vector<int>, downsampling::BASE_SAMPLING_LEVEL> downsampling::_sample_pattern_cache;
thread_local std::array<std::vector<int>, downsampling::BASE_SAMPLING_LEVEL> downsampling::_original_index_cache;


template <component_type Type, typename T>
future<> sstable::read_simple(T& component, const io_priority_class& pc) {

    auto file_path = filename(Type);
    sstlog.debug(("Reading " + sstable_version_constants::get_component_map(_version).at(Type) + " file {} ").c_str(), file_path);
    return open_file_dma(file_path, open_flags::ro).then([this, &component] (file fi) {
        auto fut = fi.size();
        return fut.then([this, &component, fi = std::move(fi)] (uint64_t size) {
            auto f = make_checked_file(_read_error_handler, fi);
            auto r = make_lw_shared<file_random_access_reader>(std::move(f), size, sstable_buffer_size);
            auto fut = parse(_version, *r, component);
            return fut.finally([r] {
                return r->close();
            }).then([r] {});
        });
    }).then_wrapped([this, file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            if (e.code() == std::error_code(ENOENT, std::system_category())) {
                throw malformed_sstable_exception(file_path + ": file not found");
            }
            throw;
        }
    });
}

template <component_type Type, typename T>
void sstable::write_simple(const T& component, const io_priority_class& pc) {
    auto file_path = filename(Type);
    sstlog.debug(("Writing " + sstable_version_constants::get_component_map(_version).at(Type) + " file {} ").c_str(), file_path);
    file f = new_sstable_component_file(_write_error_handler, file_path, open_flags::wo | open_flags::create | open_flags::exclusive).get0();

    file_output_stream_options options;
    options.buffer_size = sstable_buffer_size;
    options.io_priority_class = pc;
    auto w = file_writer(std::move(f), std::move(options));
    write(_version, w, component);
    w.flush().get();
    w.close().get();
}

template future<> sstable::read_simple<component_type::Filter>(sstables::filter& f, const io_priority_class& pc);
template void sstable::write_simple<component_type::Filter>(const sstables::filter& f, const io_priority_class& pc);

future<> sstable::read_compression(const io_priority_class& pc) {
     // FIXME: If there is no compression, we should expect a CRC file to be present.
    if (!has_component(component_type::CompressionInfo)) {
        return make_ready_future<>();
    }

    return read_simple<component_type::CompressionInfo>(_components->compression, pc);
}

void sstable::write_compression(const io_priority_class& pc) {
    if (!has_component(component_type::CompressionInfo)) {
        return;
    }

    write_simple<component_type::CompressionInfo>(_components->compression, pc);
}

void sstable::validate_min_max_metadata() {
    auto entry = _components->statistics.contents.find(metadata_type::Stats);
    if (entry == _components->statistics.contents.end()) {
        throw std::runtime_error("Stats metadata not available");
    }
    auto& p = entry->second;
    if (!p) {
        throw std::runtime_error("Statistics is malformed");
    }

    stats_metadata& s = *static_cast<stats_metadata *>(p.get());
    auto is_composite_valid = [] (const bytes& b) {
        auto v = composite_view(b);
        try {
            size_t s = 0;
            for (auto& c : v.components()) {
                s += c.first.size() + sizeof(composite::size_type) + sizeof(composite::eoc_type);
            }
            return s == b.size();
        } catch (marshal_exception&) {
            return false;
        }
    };
    auto clear_incorrect_min_max_column_names = [&s] {
        s.min_column_names.elements.clear();
        s.max_column_names.elements.clear();
    };
    auto& min_column_names = s.min_column_names.elements;
    auto& max_column_names = s.max_column_names.elements;

    if (min_column_names.empty() && max_column_names.empty()) {
        return;
    }

    // The min/max metadata is wrong if:
    // 1) it's not empty and schema defines no clustering key.
    // 2) their size differ.
    // 3) column name is stored instead of clustering value.
    // 4) clustering component is stored as composite.
    if ((!_schema->clustering_key_size() && (min_column_names.size() || max_column_names.size())) ||
            (min_column_names.size() != max_column_names.size())) {
        clear_incorrect_min_max_column_names();
        return;
    }

    for (auto i = 0U; i < min_column_names.size(); i++) {
        if (_schema->get_column_definition(min_column_names[i].value) || _schema->get_column_definition(max_column_names[i].value)) {
            clear_incorrect_min_max_column_names();
            break;
        }

        if (_schema->is_compound() && _schema->clustering_key_size() > 1 && _schema->is_dense() &&
                (is_composite_valid(min_column_names[i].value) || is_composite_valid(max_column_names[i].value))) {
            clear_incorrect_min_max_column_names();
            break;
        }
    }
}

void sstable::validate_max_local_deletion_time() {
    if (!has_correct_max_deletion_time()) {
        auto& entry = _components->statistics.contents[metadata_type::Stats];
        auto& s = *static_cast<stats_metadata*>(entry.get());
        s.max_local_deletion_time = std::numeric_limits<int32_t>::max();
    }
}

void sstable::set_clustering_components_ranges() {
    if (!_schema->clustering_key_size()) {
        return;
    }
    auto& min_column_names = get_stats_metadata().min_column_names.elements;
    auto& max_column_names = get_stats_metadata().max_column_names.elements;

    auto s = std::min(min_column_names.size(), max_column_names.size());
    _clustering_components_ranges.reserve(s);
    for (auto i = 0U; i < s; i++) {
        auto r = nonwrapping_range<bytes_view>({{ min_column_names[i].value, true }}, {{ max_column_names[i].value, true }});
        _clustering_components_ranges.push_back(std::move(r));
    }
}

const std::vector<nonwrapping_range<bytes_view>>& sstable::clustering_components_ranges() const {
    return _clustering_components_ranges;
}

double sstable::estimate_droppable_tombstone_ratio(gc_clock::time_point gc_before) const {
    auto& st = get_stats_metadata();
    auto estimated_count = st.estimated_cells_count.mean() * st.estimated_cells_count.count();
    if (estimated_count > 0) {
        double droppable = st.estimated_tombstone_drop_time.sum(gc_before.time_since_epoch().count());
        return droppable / estimated_count;
    }
    return 0.0f;
}

future<> sstable::read_statistics(const io_priority_class& pc) {
    return read_simple<component_type::Statistics>(_components->statistics, pc);
}

void sstable::write_statistics(const io_priority_class& pc) {
    write_simple<component_type::Statistics>(_components->statistics, pc);
}

void sstable::rewrite_statistics(const io_priority_class& pc) {
    auto file_path = filename(component_type::TemporaryStatistics);
    sstlog.debug("Rewriting statistics component of sstable {}", get_filename());
    file f = new_sstable_component_file(_write_error_handler, file_path, open_flags::wo | open_flags::create | open_flags::truncate).get0();

    file_output_stream_options options;
    options.buffer_size = sstable_buffer_size;
    options.io_priority_class = pc;
    auto w = file_writer(std::move(f), std::move(options));
    write(_version, w, _components->statistics);
    w.flush().get();
    w.close().get();
    // rename() guarantees atomicity when renaming a file into place.
    sstable_write_io_check(rename_file, file_path, filename(component_type::Statistics)).get();
}

future<> sstable::read_summary(const io_priority_class& pc) {
    if (_components->summary) {
        return make_ready_future<>();
    }

    return read_toc().then([this, &pc] {
        // We'll try to keep the main code path exception free, but if an exception does happen
        // we can try to regenerate the Summary.
        if (has_component(component_type::Summary)) {
            return read_simple<component_type::Summary>(_components->summary, pc).handle_exception([this, &pc] (auto ep) {
                sstlog.warn("Couldn't read summary file {}: {}. Recreating it.", this->filename(component_type::Summary), ep);
                return this->generate_summary(pc);
            });
        } else {
            return generate_summary(pc);
        }
    });
}

future<file> sstable::open_file(component_type type, open_flags flags, file_open_options opts) {
    if ((type != component_type::Data && type != component_type::Index)
                    || get_config().extensions().sstable_file_io_extensions().empty()) {
        return new_sstable_component_file(_read_error_handler, filename(type), flags, opts);
    }
    return new_sstable_component_file_non_checked(filename(type), flags, opts).then([this, type, flags](file f) {
        return do_with(std::move(f), [this, type, flags](file& f) {
            auto ext_range = get_config().extensions().sstable_file_io_extensions();
            return do_for_each(ext_range.begin(), ext_range.end(), [this, &f, type, flags](auto& ext) {
                // note: we're potentially wrapping more than once. extension mechanism
                // is responsible for order being sane.
                return ext->wrap_file(*this, type, f, flags).then([&f](file of) {
                    if (of) {
                        f = std::move(of);
                    }
                });
            }).then([this, &f] {
                return make_checked_file(_read_error_handler, std::move(f));
            });
        });
    });
}

future<> sstable::open_data() {
    return when_all(open_file(component_type::Index, open_flags::ro),
                    open_file(component_type::Data, open_flags::ro))
                    .then([this] (auto files) {

        _index_file = std::get<file>(std::get<0>(files).get());
        _data_file = std::get<file>(std::get<1>(files).get());

        return this->update_info_for_opened_data();
    }).then([this] {
        if (_shards.empty()) {
            _shards = compute_shards_for_this_sstable();
        }
    });
}

future<> sstable::update_info_for_opened_data() {
    return _data_file.stat().then([this] (struct stat st) {
        if (this->has_component(component_type::CompressionInfo)) {
            _components->compression.update(st.st_size);
        }
        _data_file_size = st.st_size;
        _data_file_write_time = db_clock::from_time_t(st.st_mtime);
    }).then([this] {
        return _index_file.size().then([this] (auto size) {
            _index_file_size = size;
        });
    }).then([this] {
        if (this->has_component(component_type::Filter)) {
            return io_check([&] {
                return engine().file_size(this->filename(component_type::Filter));
            }).then([this] (auto size) {
                _filter_file_size = size;
            });
        }
        return make_ready_future<>();
    }).then([this] {
        this->set_clustering_components_ranges();
        this->set_first_and_last_keys();

        // Get disk usage for this sstable (includes all components).
        _bytes_on_disk = 0;
        return do_for_each(_recognized_components, [this] (component_type c) {
            return this->sstable_write_io_check([&, c] {
                return engine().file_exists(this->filename(c)).then([this, c] (bool exists) {
                    // ignore summary that isn't present in disk but was previously generated by read_summary().
                    if (!exists && c == component_type::Summary && _components->summary.memory_footprint()) {
                        return make_ready_future<uint64_t>(0);
                    }
                    return engine().file_size(this->filename(c));
                });
            }).then([this] (uint64_t bytes) {
                _bytes_on_disk += bytes;
            });
        });
    });
}

future<> sstable::create_data() {
    auto oflags = open_flags::wo | open_flags::create | open_flags::exclusive;
    file_open_options opt;
    opt.extent_allocation_size_hint = 32 << 20;
    opt.sloppy_size = true;
    return when_all(open_file(component_type::Index, oflags, opt),
                    open_file(component_type::Data, oflags, opt)).then([this] (auto files) {
        // FIXME: If both files could not be created, the first get below will
        // throw an exception, and second get() will not be attempted, and
        // we'll get a warning about the second future being destructed
        // without its exception being examined.

        _index_file = std::get<file>(std::get<0>(files).get());
        _data_file = std::get<file>(std::get<1>(files).get());
    });
}

future<> sstable::read_filter(const io_priority_class& pc) {
    if (!has_component(component_type::Filter)) {
        _components->filter = std::make_unique<utils::filter::always_present_filter>();
        return make_ready_future<>();
    }

    return seastar::async([this, &pc] () mutable {
        sstables::filter filter;
        read_simple<component_type::Filter>(filter, pc).get();
        auto nr_bits = filter.buckets.elements.size() * std::numeric_limits<typename decltype(filter.buckets.elements)::value_type>::digits;
        large_bitset bs(nr_bits, std::move(filter.buckets.elements));
        utils::filter_format format = (_version == sstable_version_types::mc)
                                      ? utils::filter_format::m_format
                                      : utils::filter_format::k_l_format;
        _components->filter = utils::filter::create_filter(filter.hashes, std::move(bs), format);
    });
}

void sstable::write_filter(const io_priority_class& pc) {
    if (!has_component(component_type::Filter)) {
        return;
    }

    auto f = static_cast<utils::filter::murmur3_bloom_filter *>(_components->filter.get());

    auto&& bs = f->bits();
    auto filter_ref = sstables::filter_ref(f->num_hashes(), bs.get_storage());
    write_simple<component_type::Filter>(filter_ref, pc);
}

// This interface is only used during tests, snapshot loading and early initialization.
// No need to set tunable priorities for it.
future<> sstable::load(const io_priority_class& pc) {
    return read_toc().then([this, &pc] {
        // Read statistics ahead of others - if summary is missing
        // we'll attempt to re-generate it and we need statistics for that
        return read_statistics(pc).then([this, &pc] {
            return seastar::when_all_succeed(
                    read_compression(pc),
                    read_scylla_metadata(pc),
                    read_filter(pc),
                    read_summary(pc)).then([this] {
                validate_min_max_metadata();
                validate_max_local_deletion_time();
                return open_data();
            });
        });
    });
}

future<> sstable::load(sstables::foreign_sstable_open_info info) {
    return read_toc().then([this, info = std::move(info)] () mutable {
        _components = std::move(info.components);
        _data_file = make_checked_file(_read_error_handler, info.data.to_file());
        _index_file = make_checked_file(_read_error_handler, info.index.to_file());
        _shards = std::move(info.owners);
        validate_min_max_metadata();
        validate_max_local_deletion_time();
        return update_info_for_opened_data();
    });
}

future<sstable_open_info> sstable::load_shared_components(const schema_ptr& s, sstring dir, int generation, version_types v, format_types f,
        const io_priority_class& pc) {
    auto sst = sstables::make_sstable(s, dir, generation, v, f);
    return sst->load(pc).then([sst] () mutable {
        auto info = sstable_open_info{make_lw_shared<shareable_components>(std::move(*sst->_components)),
            std::move(sst->_shards), std::move(sst->_data_file), std::move(sst->_index_file)};
        return make_ready_future<sstable_open_info>(std::move(info));
    });
}

future<foreign_sstable_open_info> sstable::get_open_info() & {
    return _components.copy().then([this] (auto c) mutable {
        return foreign_sstable_open_info{std::move(c), this->get_shards_for_this_sstable(), _data_file.dup(), _index_file.dup(),
            _generation, _version, _format};
    });
}

static composite::eoc bound_kind_to_start_marker(bound_kind start_kind) {
    return start_kind == bound_kind::excl_start
         ? composite::eoc::end
         : composite::eoc::start;
}

static composite::eoc bound_kind_to_end_marker(bound_kind end_kind) {
    return end_kind == bound_kind::excl_end
         ? composite::eoc::start
         : composite::eoc::end;
}

class bytes_writer_for_column_name {
    bytes _buf;
    bytes::iterator _pos;
public:
    void prepare(size_t size) {
        _buf = bytes(bytes::initialized_later(), size);
        _pos = _buf.begin();
    }

    template<typename... Args>
    void write(Args&&... args) {
        auto write_one = [this] (bytes_view data) {
            _pos = std::copy(data.begin(), data.end(), _pos);
        };
        auto ignore = { (write_one(bytes_view(args)), 0)... };
        (void)ignore;
    }

    bytes&& release() && {
        return std::move(_buf);
    }
};

class file_writer_for_column_name {
    sstable_version_types _v;
    file_writer& _fw;
public:
    file_writer_for_column_name(sstable_version_types v, file_writer& fw) : _v(v), _fw(fw) { }

    void prepare(uint16_t size) {
        sstables::write(_v, _fw, size);
    }

    template<typename... Args>
    void write(Args&&... args) {
        sstables::write(_v, _fw, std::forward<Args>(args)...);
    }
};

template<typename Writer>
static void write_compound_non_dense_column_name(sstable_version_types v, Writer& out, const composite& clustering_key, const std::vector<bytes_view>& column_names, composite::eoc marker = composite::eoc::none) {
    // was defined in the schema, for example.
    auto c = composite::from_exploded(column_names, true, marker);
    auto ck_bview = bytes_view(clustering_key);

    // The marker is not a component, so if the last component is empty (IOW,
    // only serializes to the marker), then we just replace the key's last byte
    // with the marker. If the component however it is not empty, then the
    // marker should be in the end of it, and we just join them together as we
    // do for any normal component
    if (c.size() == 1) {
        ck_bview.remove_suffix(1);
    }
    size_t sz = ck_bview.size() + c.size();
    if (sz > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error(sprint("Column name too large (%d > %d)", sz, std::numeric_limits<uint16_t>::max()));
    }
    out.prepare(uint16_t(sz));
    out.write(ck_bview, c);
}

static void write_compound_non_dense_column_name(sstable_version_types v, file_writer& out, const composite& clustering_key, const std::vector<bytes_view>& column_names, composite::eoc marker = composite::eoc::none) {
    auto w = file_writer_for_column_name(v, out);
    write_compound_non_dense_column_name(v, w, clustering_key, column_names, marker);
}

template<typename Writer>
static void write_column_name(sstable_version_types v, Writer& out, bytes_view column_names) {
    size_t sz = column_names.size();
    if (sz > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error(sprint("Column name too large (%d > %d)", sz, std::numeric_limits<uint16_t>::max()));
    }
    out.prepare(uint16_t(sz));
    out.write(column_names);
}

static void write_column_name(sstable_version_types v, file_writer& out, bytes_view column_names) {
    auto w = file_writer_for_column_name(v, out);
    write_column_name(v, w, column_names);
}

template<typename Writer>
static void write_column_name(sstable_version_types v, Writer& out, const schema& s, const composite& clustering_element, const std::vector<bytes_view>& column_names, composite::eoc marker = composite::eoc::none) {
    if (s.is_dense()) {
        write_column_name(v, out, bytes_view(clustering_element));
    } else if (s.is_compound()) {
        write_compound_non_dense_column_name(v, out, clustering_element, column_names, marker);
    } else {
        write_column_name(v, out, column_names[0]);
    }
}

void sstable::write_range_tombstone_bound(file_writer& out,
        const schema& s,
        const composite& clustering_element,
        const std::vector<bytes_view>& column_names,
        composite::eoc marker) {
    if (!_correctly_serialize_non_compound_range_tombstones && !clustering_element.is_compound()) {
        auto vals = clustering_element.values();
        write_compound_non_dense_column_name(_version, out, composite::serialize_value(vals, true), column_names, marker);
    } else {
        write_column_name(_version, out, s, clustering_element, column_names, marker);
    }
}

static void output_promoted_index_entry(bytes_ostream& promoted_index,
        const bytes& first_col,
        const bytes& last_col,
        uint64_t offset, uint64_t width) {
    char s[2];
    write_be(s, uint16_t(first_col.size()));
    promoted_index.write(s, 2);
    promoted_index.write(first_col);
    write_be(s, uint16_t(last_col.size()));
    promoted_index.write(s, 2);
    promoted_index.write(last_col);
    char q[8];
    write_be(q, uint64_t(offset));
    promoted_index.write(q, 8);
    write_be(q, uint64_t(width));
    promoted_index.write(q, 8);
}

// Call maybe_flush_pi_block() before writing the given sstable atom to the
// output. This may start a new promoted-index block depending on how much
// data we've already written since the start of the current block. Starting
// a new block involves both outputting the range of the old block to the
// index file, and outputting again the currently-open range tombstones to
// the data file.
// TODO: currently, maybe_flush_pi_block serializes the column name on every
// call, saving it in _pi_write.block_last_colname which we need for closing
// each block, as well as for closing the last block. We could instead save
// just the unprocessed arguments, and serialize them only when needed at the
// end of the block. For this we would need this function to take rvalue
// references (so data is moved in), and need not to use vector of byte_view
// (which might be gone later).
void sstable::maybe_flush_pi_block(file_writer& out,
        const composite& clustering_key,
        const std::vector<bytes_view>& column_names,
        composite::eoc marker) {
    if (!_schema->clustering_key_size()) {
        return;
    }
    bytes_writer_for_column_name w;
    write_column_name(_version, w, *_schema, clustering_key, column_names, marker);
    maybe_flush_pi_block(out, clustering_key, std::move(w).release());
}

// Overload can only be called if the schema has clustering keys.
void sstable::maybe_flush_pi_block(file_writer& out,
        const composite& clustering_key,
        bytes colname) {
    if (_pi_write.block_first_colname.empty()) {
        // This is the first column in the partition, or first column since we
        // closed a promoted-index block. Remember its name and position -
        // we'll need to write it to the promoted index.
        _pi_write.block_start_offset = out.offset();
        _pi_write.block_next_start_offset = out.offset() + _pi_write.desired_block_size;
        _pi_write.block_first_colname = colname;
        _pi_write.block_last_colname = std::move(colname);
    } else if (out.offset() >= _pi_write.block_next_start_offset) {
        // If we wrote enough bytes to the partition since we output a sample
        // to the promoted index, output one now and start a new one.
        output_promoted_index_entry(_pi_write.data,
                _pi_write.block_first_colname,
                _pi_write.block_last_colname,
                _pi_write.block_start_offset - _c_stats.start_offset,
                out.offset() - _pi_write.block_start_offset);
        _pi_write.numblocks++;
        _pi_write.block_start_offset = out.offset();
        // Because the new block can be read without the previous blocks, we
        // need to repeat the range tombstones which are still open.
        // Note that block_start_offset is before outputting those (so the new
        // block includes them), but we set block_next_start_offset after - so
        // even if we wrote a lot of open tombstones, we still get a full
        // block size of new data.
        auto& rts = _pi_write.tombstone_accumulator->range_tombstones_for_row(
                clustering_key_prefix::from_range(clustering_key.values()));
        for (const auto& rt : rts) {
            auto start = composite::from_clustering_element(*_pi_write.schemap, rt.start);
            auto end = composite::from_clustering_element(*_pi_write.schemap, rt.end);
            write_range_tombstone(out,
                    start, bound_kind_to_start_marker(rt.start_kind),
                    end, bound_kind_to_end_marker(rt.end_kind),
                    {}, rt.tomb);
        }
        _pi_write.block_next_start_offset = out.offset() + _pi_write.desired_block_size;
        _pi_write.block_first_colname = colname;
        _pi_write.block_last_colname = std::move(colname);
    } else {
        // Keep track of the last column in the partition - we'll need it to close
        // the last block in the promoted index, unfortunately.
        _pi_write.block_last_colname = std::move(colname);
    }
}

void write_cell_value(file_writer& out, const abstract_type& type, bytes_view value) {
    if (!value.empty()) {
        if (type.value_length_if_fixed()) {
            write(sstable_version_types::mc, out, value);
        } else {
            write_vint(out, value.size());
            write(sstable_version_types::mc, out, value);
        }
    }
}

void write_cell_value(file_writer& out, const abstract_type& type, atomic_cell_value_view value) {
    if (!value.empty()) {
        if (!type.value_length_if_fixed()) {
            write_vint(out, value.size_bytes());
        }
        using boost::range::for_each;
        for_each(value, [&] (bytes_view fragment) { write(sstable_version_types::mc, out, fragment); });
    }
}

static inline void update_cell_stats(column_stats& c_stats, api::timestamp_type timestamp) {
    c_stats.update_timestamp(timestamp);
    c_stats.cells_count++;
}

template <typename WriteLengthFunc>
static void write_counter_value(counter_cell_view ccv, file_writer& out, sstable_version_types v, WriteLengthFunc&& write_len_func) {
    auto shard_count = ccv.shard_count();
    static constexpr auto header_entry_size = sizeof(int16_t);
    static constexpr auto counter_shard_size = 32u; // counter_id: 16 + clock: 8 + value: 8
    auto total_size = sizeof(int16_t) + shard_count * (header_entry_size + counter_shard_size);

    write_len_func(out, uint32_t(total_size));
    write(v, out, int16_t(shard_count));
    for (auto i = 0u; i < shard_count; i++) {
        write<int16_t>(v, out, std::numeric_limits<int16_t>::min() + i);
    }
    auto write_shard = [&] (auto&& s) {
        auto uuid = s.id().to_uuid();
        write(v, out, int64_t(uuid.get_most_significant_bits()),
              int64_t(uuid.get_least_significant_bits()),
              int64_t(s.logical_clock()), int64_t(s.value()));
    };
    if (service::get_local_storage_service().cluster_supports_correct_counter_order()) {
        for (auto&& s : ccv.shards()) {
            write_shard(s);
        }
    } else {
        for (auto&& s : ccv.shards_compatible_with_1_7_4()) {
            write_shard(s);
        }
    }
}

// Intended to write all cell components that follow column name.
void sstable::write_cell(file_writer& out, atomic_cell_view cell, const column_definition& cdef) {
    api::timestamp_type timestamp = cell.timestamp();

    update_cell_stats(_c_stats, timestamp);

    if (cell.is_dead(_now)) {
        // tombstone cell

        column_mask mask = column_mask::deletion;
        uint32_t deletion_time_size = sizeof(uint32_t);
        uint32_t deletion_time = cell.deletion_time().time_since_epoch().count();

        _c_stats.update_local_deletion_time(deletion_time);
        _c_stats.tombstone_histogram.update(deletion_time);

        write(_version, out, mask, timestamp, deletion_time_size, deletion_time);
    } else if (cdef.is_counter()) {
        // counter cell
        assert(!cell.is_counter_update());

        column_mask mask = column_mask::counter;
        write(_version, out, mask, int64_t(0), timestamp);

      counter_cell_view::with_linearized(cell, [&] (counter_cell_view ccv) {
        write_counter_value(ccv, out, _version, [v = _version] (file_writer& out, uint32_t value) {
            return write(v, out, value);
        });

        _c_stats.update_local_deletion_time(std::numeric_limits<int>::max());
      });
    } else if (cell.is_live_and_has_ttl()) {
        // expiring cell

        column_mask mask = column_mask::expiration;
        uint32_t ttl = cell.ttl().count();
        uint32_t expiration = cell.expiry().time_since_epoch().count();
        disk_data_value_view<uint32_t> cell_value { cell.value() };

        _c_stats.update_local_deletion_time(expiration);
        // tombstone histogram is updated with expiration time because if ttl is longer
        // than gc_grace_seconds for all data, sstable will be considered fully expired
        // when actually nothing is expired.
        _c_stats.tombstone_histogram.update(expiration);

        write(_version, out, mask, ttl, expiration, timestamp, cell_value);
    } else {
        // regular cell

        column_mask mask = column_mask::none;
        disk_data_value_view<uint32_t> cell_value { cell.value() };

        _c_stats.update_local_deletion_time(std::numeric_limits<int>::max());

        write(_version, out, mask, timestamp, cell_value);
    }
}

void sstable::maybe_write_row_marker(file_writer& out, const schema& schema, const row_marker& marker, const composite& clustering_key) {
    if (!schema.is_compound() || schema.is_dense() || marker.is_missing()) {
        return;
    }
    // Write row mark cell to the beginning of clustered row.
    index_and_write_column_name(out, clustering_key, { bytes_view() });
    uint64_t timestamp = marker.timestamp();
    uint32_t value_length = 0;

    update_cell_stats(_c_stats, timestamp);

    if (marker.is_dead(_now)) {
        column_mask mask = column_mask::deletion;
        uint32_t deletion_time_size = sizeof(uint32_t);
        uint32_t deletion_time = marker.deletion_time().time_since_epoch().count();

        _c_stats.tombstone_histogram.update(deletion_time);

        write(_version, out, mask, timestamp, deletion_time_size, deletion_time);
    } else if (marker.is_expiring()) {
        column_mask mask = column_mask::expiration;
        uint32_t ttl = marker.ttl().count();
        uint32_t expiration = marker.expiry().time_since_epoch().count();
        write(_version, out, mask, ttl, expiration, timestamp, value_length);
    } else {
        column_mask mask = column_mask::none;
        write(_version, out, mask, timestamp, value_length);
    }
}

void sstable::write_deletion_time(file_writer& out, const tombstone t) {
    uint64_t timestamp = t.timestamp;
    uint32_t deletion_time = t.deletion_time.time_since_epoch().count();

    update_cell_stats(_c_stats, timestamp);
    _c_stats.update_local_deletion_time(deletion_time);
    _c_stats.tombstone_histogram.update(deletion_time);

    write(_version, out, deletion_time, timestamp);
}

void sstable::index_tombstone(file_writer& out, const composite& key, range_tombstone&& rt, composite::eoc marker) {
    maybe_flush_pi_block(out, key, {}, marker);
    // Remember the range tombstone so when we need to open a new promoted
    // index block, we can figure out which ranges are still open and need
    // to be repeated in the data file. Note that apply() also drops ranges
    // already closed by rt.start, so the accumulator doesn't grow boundless.
    _pi_write.tombstone_accumulator->apply(std::move(rt));
}

void sstable::maybe_write_row_tombstone(file_writer& out, const composite& key, const clustering_row& clustered_row) {
    auto t = clustered_row.tomb();
    if (!t) {
        return;
    }
    auto rt = range_tombstone(clustered_row.key(), bound_kind::incl_start, clustered_row.key(), bound_kind::incl_end, t.tomb());
    index_tombstone(out, key, std::move(rt), composite::eoc::none);
    write_range_tombstone(out, key, composite::eoc::start, key, composite::eoc::end, {}, t.regular());
    if (t.is_shadowable()) {
        write_range_tombstone(out, key, composite::eoc::start, key, composite::eoc::end, {}, t.shadowable().tomb(), column_mask::shadowable);
    }
}

void sstable::write_range_tombstone(file_writer& out,
        const composite& start,
        composite::eoc start_marker,
        const composite& end,
        composite::eoc end_marker,
        std::vector<bytes_view> suffix,
        const tombstone t,
        column_mask mask) {
    if (!_schema->is_compound() && (start_marker == composite::eoc::end || end_marker == composite::eoc::start)) {
        throw std::logic_error(sprint("Cannot represent marker type in range tombstone for non-compound schemas"));
    }
    write_range_tombstone_bound(out, *_schema, start, suffix, start_marker);
    write(_version, out, mask);
    write_range_tombstone_bound(out, *_schema, end, suffix, end_marker);
    write_deletion_time(out, t);
}

void sstable::write_collection(file_writer& out, const composite& clustering_key, const column_definition& cdef, collection_mutation_view collection) {
  collection.data.with_linearized([&] (bytes_view collection_bv) {
    auto t = static_pointer_cast<const collection_type_impl>(cdef.type);
    auto mview = t->deserialize_mutation_form(collection_bv);
    const bytes& column_name = cdef.name();
    if (mview.tomb) {
        write_range_tombstone(out, clustering_key, composite::eoc::start, clustering_key, composite::eoc::end, { column_name }, mview.tomb);
    }
    for (auto& cp: mview.cells) {
        index_and_write_column_name(out, clustering_key, { column_name, cp.first });
        write_cell(out, cp.second, cdef);
    }
  });
}

// This function is about writing a clustered_row to data file according to SSTables format.
// clustered_row contains a set of cells sharing the same clustering key.
void sstable::write_clustered_row(file_writer& out, const schema& schema, const clustering_row& clustered_row) {
    auto clustering_key = composite::from_clustering_element(schema, clustered_row.key());

    maybe_write_row_marker(out, schema, clustered_row.marker(), clustering_key);
    maybe_write_row_tombstone(out, clustering_key, clustered_row);

    if (schema.clustering_key_size()) {
        column_name_helper::min_max_components(schema, _collector.min_column_names(), _collector.max_column_names(),
            clustered_row.key().components());
    }

    // Write all cells of a partition's row.
    clustered_row.cells().for_each_cell([&] (column_id id, const atomic_cell_or_collection& c) {
        auto&& column_definition = schema.regular_column_at(id);
        // non atomic cell isn't supported yet. atomic cell maps to a single trift cell.
        // non atomic cell maps to multiple trift cell, e.g. collection.
        if (!column_definition.is_atomic()) {
            write_collection(out, clustering_key, column_definition, c.as_collection_mutation());
            return;
        }
        assert(column_definition.is_regular());
        atomic_cell_view cell = c.as_atomic_cell(column_definition);
        std::vector<bytes_view> column_name = { column_definition.name() };
        index_and_write_column_name(out, clustering_key, column_name);
        write_cell(out, cell, column_definition);
    });
}

void sstable::write_static_row(file_writer& out, const schema& schema, const row& static_row) {
    assert(schema.is_compound());
    static_row.for_each_cell([&] (column_id id, const atomic_cell_or_collection& c) {
        auto&& column_definition = schema.static_column_at(id);
        if (!column_definition.is_atomic()) {
            auto sp = composite::static_prefix(schema);
            write_collection(out, sp, column_definition, c.as_collection_mutation());
            return;
        }
        assert(column_definition.is_static());
        const auto& column_name = column_definition.name();
        auto sp = composite::static_prefix(schema);
        index_and_write_column_name(out, sp, { bytes_view(column_name) });
        atomic_cell_view cell = c.as_atomic_cell(column_definition);
        write_cell(out, cell, column_definition);
    });
}

void sstable::index_and_write_column_name(file_writer& out,
         const composite& clustering_element,
         const std::vector<bytes_view>& column_names,
         composite::eoc marker) {
    if (_schema->clustering_key_size()) {
        bytes_writer_for_column_name w;
        write_column_name(_version, w, *_schema, clustering_element, column_names, marker);
        auto&& colname = std::move(w).release();
        maybe_flush_pi_block(out, clustering_element, colname);
        write_column_name(_version, out, colname);
    } else {
        write_column_name(_version, out, *_schema, clustering_element, column_names, marker);
    }
}

static void write_index_header(sstable_version_types v, file_writer& out, disk_string_view<uint16_t>& key, uint64_t pos) {
    write(v, out, key, pos);
}

static void write_index_promoted(sstable_version_types v, file_writer& out, bytes_ostream& promoted_index,
        deletion_time deltime, uint32_t numblocks) {
    uint32_t promoted_index_size = promoted_index.size();
    if (promoted_index_size) {
        promoted_index_size += 16 /* deltime + numblocks */;
        write(v, out, promoted_index_size, deltime, numblocks, promoted_index);
    } else {
        write(v, out, promoted_index_size);
    }
}

static void prepare_summary(summary& s, uint64_t expected_partition_count, uint32_t min_index_interval) {
    assert(expected_partition_count >= 1);

    s.header.min_index_interval = min_index_interval;
    s.header.sampling_level = downsampling::BASE_SAMPLING_LEVEL;
    uint64_t max_expected_entries =
            (expected_partition_count / min_index_interval) +
            !!(expected_partition_count % min_index_interval);
    // FIXME: handle case where max_expected_entries is greater than max value stored by uint32_t.
    if (max_expected_entries > std::numeric_limits<uint32_t>::max()) {
        throw malformed_sstable_exception("Current sampling level (" + to_sstring(downsampling::BASE_SAMPLING_LEVEL) + ") not enough to generate summary.");
    }

    s.header.memory_size = 0;
}

static void seal_summary(summary& s,
        std::experimental::optional<key>&& first_key,
        std::experimental::optional<key>&& last_key,
        const index_sampling_state& state) {
    s.header.size = s.entries.size();
    s.header.size_at_full_sampling = sstable::get_size_at_full_sampling(state.partition_count, s.header.min_index_interval);

    s.header.memory_size = s.header.size * sizeof(uint32_t);
    for (auto& e: s.entries) {
        s.positions.push_back(s.header.memory_size);
        s.header.memory_size += e.key.size() + sizeof(e.position);
    }
    assert(first_key); // assume non-empty sstable
    s.first_key.value = first_key->get_bytes();

    if (last_key) {
        s.last_key.value = last_key->get_bytes();
    } else {
        // An empty last_mutation indicates we had just one partition
        s.last_key.value = s.first_key.value;
    }
}

static
void
populate_statistics_offsets(sstable_version_types v, statistics& s) {
    // copy into a sorted vector to guarantee consistent order
    auto types = boost::copy_range<std::vector<metadata_type>>(s.contents | boost::adaptors::map_keys);
    boost::sort(types);

    // populate the hash with garbage so we can calculate its size
    for (auto t : types) {
        s.offsets.elements.emplace_back(t, -1);
    }

    auto offset = serialized_size(v, s.offsets);
    s.offsets.elements.clear();
    for (auto t : types) {
        s.offsets.elements.emplace_back(t, offset);
        offset += s.contents[t]->serialized_size(v);
    }
}

static
sharding_metadata
create_sharding_metadata(schema_ptr schema, const dht::decorated_key& first_key, const dht::decorated_key& last_key, shard_id shard) {
    auto prange = dht::partition_range::make(dht::ring_position(first_key), dht::ring_position(last_key));
    auto sm = sharding_metadata();
    for (auto&& range : dht::split_range_to_single_shard(*schema, prange, shard)) {
        if (true) { // keep indentation
            // we know left/right are not infinite
            auto&& left = range.start()->value();
            auto&& right = range.end()->value();
            auto&& left_token = left.token();
            auto left_exclusive = !left.has_key() && left.bound() == dht::ring_position::token_bound::end;
            auto&& right_token = right.token();
            auto right_exclusive = !right.has_key() && right.bound() == dht::ring_position::token_bound::start;
            sm.token_ranges.elements.push_back(disk_token_range{
                {left_exclusive, to_bytes(bytes_view(left_token._data))},
                {right_exclusive, to_bytes(bytes_view(right_token._data))}});
        }
    }
    return sm;
}

static bytes_array_vint_size to_bytes_array_vint_size(bytes b) {
    bytes_array_vint_size result;
    result.value = std::move(b);
    return result;
}

static bytes_array_vint_size to_bytes_array_vint_size(const sstring& s) {
    bytes_array_vint_size result;
    result.value = to_bytes(s);
    return result;
}

static sstring pk_type_to_string(const schema& s) {
    if (s.partition_key_size() == 1) {
        return s.partition_key_columns().begin()->type->name();
    } else {
        sstring type_params = ::join(",", s.partition_key_columns()
                            | boost::adaptors::transformed(std::mem_fn(&column_definition::type))
                            | boost::adaptors::transformed(std::mem_fn(&abstract_type::name)));
        return "org.apache.cassandra.db.marshal.CompositeType(" + type_params + ")";
    }
}

static serialization_header make_serialization_header(const schema& s, const encoding_stats& enc_stats) {
    serialization_header header;
    header.min_timestamp_base.value = static_cast<uint64_t>(enc_stats.min_timestamp) - encoding_stats::timestamp_epoch;
    header.min_local_deletion_time_base.value = enc_stats.min_local_deletion_time - encoding_stats::deletion_time_epoch;
    header.min_ttl_base.value = enc_stats.min_ttl - encoding_stats::ttl_epoch;

    header.pk_type_name = to_bytes_array_vint_size(pk_type_to_string(s));

    header.clustering_key_types_names.elements.reserve(s.clustering_key_size());
    for (const auto& ck_column : s.clustering_key_columns()) {
        auto ck_type_name = to_bytes_array_vint_size(ck_column.type->name());
        header.clustering_key_types_names.elements.push_back(std::move(ck_type_name));
    }

    header.static_columns.elements.reserve(s.static_columns_count());
    for (const auto& static_column : s.static_columns()) {
        serialization_header::column_desc cd;
        cd.name = to_bytes_array_vint_size(static_column.name());
        cd.type_name = to_bytes_array_vint_size(static_column.type->name());
        header.static_columns.elements.push_back(std::move(cd));
    }

    header.regular_columns.elements.reserve(s.regular_columns_count());
    for (const auto& regular_column : s.regular_columns()) {
        serialization_header::column_desc cd;
        cd.name = to_bytes_array_vint_size(regular_column.name());
        cd.type_name = to_bytes_array_vint_size(regular_column.type->name());
        header.regular_columns.elements.push_back(std::move(cd));
    }

    return header;
}

// In the beginning of the statistics file, there is a disk_hash used to
// map each metadata type to its correspondent position in the file.
static void seal_statistics(sstable_version_types v, statistics& s, metadata_collector& collector,
        const sstring partitioner, double bloom_filter_fp_chance, schema_ptr schema,
        const dht::decorated_key& first_key, const dht::decorated_key& last_key, encoding_stats enc_stats = {}) {
    validation_metadata validation;
    compaction_metadata compaction;
    stats_metadata stats;

    validation.partitioner.value = to_bytes(partitioner);
    validation.filter_chance = bloom_filter_fp_chance;
    s.contents[metadata_type::Validation] = std::make_unique<validation_metadata>(std::move(validation));

    collector.construct_compaction(compaction);
    s.contents[metadata_type::Compaction] = std::make_unique<compaction_metadata>(std::move(compaction));

    collector.construct_stats(stats);
    s.contents[metadata_type::Stats] = std::make_unique<stats_metadata>(std::move(stats));

    if (v == sstable_version_types::mc) {
        auto header = make_serialization_header(*schema, enc_stats);
        s.contents[metadata_type::Serialization] = std::make_unique<serialization_header>(std::move(header));
    }

    populate_statistics_offsets(v, s);
}

void components_writer::maybe_add_summary_entry(summary& s, const dht::token& token, bytes_view key, uint64_t data_offset,
        uint64_t index_offset, index_sampling_state& state) {
    state.partition_count++;
    // generates a summary entry when possible (= keep summary / data size ratio within reasonable limits)
    if (data_offset >= state.next_data_offset_to_write_summary) {
        auto entry_size = 8 + 2 + key.size();  // offset + key_size.size + key.size
        state.next_data_offset_to_write_summary += state.summary_byte_cost * entry_size;
        auto token_data = s.add_summary_data(bytes_view(token._data));
        auto key_data = s.add_summary_data(key);
        s.entries.push_back({ dht::token_view(dht::token::kind::key, token_data), key_data, index_offset });
    }
}

void components_writer::maybe_add_summary_entry(const dht::token& token, bytes_view key) {
    return maybe_add_summary_entry(_sst._components->summary, token, key, get_offset(),
        _index.offset(), _index_sampling_state);
}

// Returns offset into data component.
uint64_t components_writer::get_offset() const {
    if (_sst.has_component(component_type::CompressionInfo)) {
        // Variable returned by compressed_file_length() is constantly updated by compressed output stream.
        return _sst._components->compression.compressed_file_length();
    } else {
        return _out.offset();
    }
}

file_writer components_writer::index_file_writer(sstable& sst, const io_priority_class& pc) {
    file_output_stream_options options;
    options.buffer_size = sst.sstable_buffer_size;
    options.io_priority_class = pc;
    options.write_behind = 10;
    return file_writer(std::move(sst._index_file), std::move(options));
}

// Get the currently loaded configuration, or the default configuration in
// case none has been loaded (this happens, for example, in unit tests).
static const db::config& get_config() {
    if (service::get_storage_service().local_is_initialized() &&
            service::get_local_storage_service().db().local_is_initialized()) {
        return service::get_local_storage_service().db().local().get_config();
    } else {
        static db::config default_config;
        return default_config;
    }
}

// Returns the cost for writing a byte to summary such that the ratio of summary
// to data will be 1 to cost by the time sstable is sealed.
static size_t summary_byte_cost() {
    auto summary_ratio = get_config().sstable_summary_ratio();
    return summary_ratio ? (1 / summary_ratio) : components_writer::default_summary_byte_cost;
}

components_writer::components_writer(sstable& sst, const schema& s, file_writer& out,
                                     uint64_t estimated_partitions,
                                     const sstable_writer_config& cfg,
                                     const io_priority_class& pc)
    : _sst(sst)
    , _schema(s)
    , _out(out)
    , _index(index_file_writer(sst, pc))
    , _index_needs_close(true)
    , _max_sstable_size(cfg.max_sstable_size)
    , _tombstone_written(false)
    , _range_tombstones(s)
    , _large_partition_handler(cfg.large_partition_handler)
{
    _sst._components->filter = utils::i_filter::get_filter(estimated_partitions, _schema.bloom_filter_fp_chance(), utils::filter_format::k_l_format);
    _sst._pi_write.desired_block_size = cfg.promoted_index_block_size.value_or(get_config().column_index_size_in_kb() * 1024);
    _sst._correctly_serialize_non_compound_range_tombstones = cfg.correctly_serialize_non_compound_range_tombstones;
    _index_sampling_state.summary_byte_cost = summary_byte_cost();

    prepare_summary(_sst._components->summary, estimated_partitions, _schema.min_index_interval());

    // FIXME: we may need to set repaired_at stats at this point.
}

void components_writer::consume_new_partition(const dht::decorated_key& dk) {
    // Set current index of data to later compute row size.
    _sst._c_stats.start_offset = _out.offset();

    _partition_key = key::from_partition_key(_schema, dk.key());

    maybe_add_summary_entry(dk.token(), bytes_view(*_partition_key));
    _sst._components->filter->add(bytes_view(*_partition_key));
    _sst._collector.add_key(bytes_view(*_partition_key));

    auto p_key = disk_string_view<uint16_t>();
    p_key.value = bytes_view(*_partition_key);

    // Write index file entry for partition key into index file.
    // Write an index entry minus the "promoted index" (sample of columns)
    // part. We can only write that after processing the entire partition
    // and collecting the sample of columns.
    write_index_header(_sst.get_version(), _index, p_key, _out.offset());
    _sst._pi_write.data = {};
    _sst._pi_write.numblocks = 0;
    _sst._pi_write.deltime.local_deletion_time = std::numeric_limits<int32_t>::max();
    _sst._pi_write.deltime.marked_for_delete_at = std::numeric_limits<int64_t>::min();
    _sst._pi_write.block_start_offset = _out.offset();
    _sst._pi_write.tombstone_accumulator = range_tombstone_accumulator(_schema, false);
    _sst._pi_write.schemap = &_schema; // sadly we need this

    // Write partition key into data file.
    write(_sst.get_version(), _out, p_key);

    _tombstone_written = false;
}

void components_writer::consume(tombstone t) {
    deletion_time d;

    if (t) {
        d.local_deletion_time = t.deletion_time.time_since_epoch().count();
        d.marked_for_delete_at = t.timestamp;

        _sst._c_stats.tombstone_histogram.update(d.local_deletion_time);
        _sst._c_stats.update_local_deletion_time(d.local_deletion_time);
        _sst._c_stats.update_timestamp(d.marked_for_delete_at);
    } else {
        // Default values for live, undeleted rows.
        d.local_deletion_time = std::numeric_limits<int32_t>::max();
        d.marked_for_delete_at = std::numeric_limits<int64_t>::min();
    }
    write(_sst.get_version(), _out, d);
    _tombstone_written = true;
    // TODO: need to verify we don't do this twice?
    _sst._pi_write.deltime = d;
}

stop_iteration components_writer::consume(static_row&& sr) {
    ensure_tombstone_is_written();
    _sst.write_static_row(_out, _schema, sr.cells());
    return stop_iteration::no;
}

stop_iteration components_writer::consume(clustering_row&& cr) {
    drain_tombstones(cr.position());
    _sst.write_clustered_row(_out, _schema, cr);
    return stop_iteration::no;
}

void components_writer::drain_tombstones(position_in_partition_view pos) {
    ensure_tombstone_is_written();
    while (auto mfo = _range_tombstones.get_next(pos)) {
        write_tombstone(std::move(mfo->as_mutable_range_tombstone()));
    }
}

void components_writer::drain_tombstones() {
    ensure_tombstone_is_written();
    while (auto mfo = _range_tombstones.get_next()) {
        write_tombstone(std::move(mfo->as_mutable_range_tombstone()));
    }
}

stop_iteration components_writer::consume(range_tombstone&& rt) {
    drain_tombstones(rt.position());
    _range_tombstones.apply(std::move(rt));
    return stop_iteration::no;
}

void components_writer::write_tombstone(range_tombstone&& rt) {
    auto start = composite::from_clustering_element(_schema, rt.start);
    auto start_marker = bound_kind_to_start_marker(rt.start_kind);
    auto end = composite::from_clustering_element(_schema, rt.end);
    auto end_marker = bound_kind_to_end_marker(rt.end_kind);
    auto tomb = rt.tomb;
    _sst.index_tombstone(_out, start, std::move(rt), start_marker);
    _sst.write_range_tombstone(_out, std::move(start), start_marker, std::move(end), end_marker, {}, tomb);
}

stop_iteration components_writer::consume_end_of_partition() {
    drain_tombstones();

    // If there is an incomplete block in the promoted index, write it too.
    // However, if the _promoted_index is still empty, don't add a single
    // chunk - better not output a promoted index at all in this case.
    if (!_sst._pi_write.data.empty() && !_sst._pi_write.block_first_colname.empty()) {
        output_promoted_index_entry(_sst._pi_write.data,
            _sst._pi_write.block_first_colname,
            _sst._pi_write.block_last_colname,
            _sst._pi_write.block_start_offset - _sst._c_stats.start_offset,
            _out.offset() - _sst._pi_write.block_start_offset);
        _sst._pi_write.numblocks++;
    }
    write_index_promoted(_sst.get_version(), _index, _sst._pi_write.data, _sst._pi_write.deltime,
            _sst._pi_write.numblocks);
    _sst._pi_write.data = {};
    _sst._pi_write.block_first_colname = {};

    int16_t end_of_row = 0;
    write(_sst.get_version(), _out, end_of_row);

    // compute size of the current row.
    _sst._c_stats.partition_size = _out.offset() - _sst._c_stats.start_offset;

    _large_partition_handler->maybe_update_large_partitions(_sst, *_partition_key, _sst._c_stats.partition_size);

    // update is about merging column_stats with the data being stored by collector.
    _sst._collector.update(std::move(_sst._c_stats));
    _sst._c_stats.reset();

    if (!_first_key) {
        _first_key = *_partition_key;
    }
    _last_key = std::move(*_partition_key);

    return get_offset() < _max_sstable_size ? stop_iteration::no : stop_iteration::yes;
}

void components_writer::consume_end_of_stream() {
    // what if there is only one partition? what if it is empty?
    seal_summary(_sst._components->summary, std::move(_first_key), std::move(_last_key), _index_sampling_state);

    _index_needs_close = false;
    _index.close().get();

    if (_sst.has_component(component_type::CompressionInfo)) {
        _sst._collector.add_compression_ratio(_sst._components->compression.compressed_file_length(), _sst._components->compression.uncompressed_file_length());
    }

    _sst.set_first_and_last_keys();
    seal_statistics(_sst.get_version(), _sst._components->statistics, _sst._collector, dht::global_partitioner().name(), _schema.bloom_filter_fp_chance(),
            _sst._schema, _sst.get_first_decorated_key(), _sst.get_last_decorated_key());
}

components_writer::~components_writer() {
    if (_index_needs_close) {
        try {
            _index.close().get();
        } catch (...) {
            sstlog.error("components_writer failed to close file: {}", std::current_exception());
        }
    }
}

future<>
sstable::read_scylla_metadata(const io_priority_class& pc) {
    if (_components->scylla_metadata) {
        return make_ready_future<>();
    }
    return read_toc().then([this, &pc] {
        _components->scylla_metadata.emplace();  // engaged optional means we won't try to re-read this again
        if (!has_component(component_type::Scylla)) {
            return make_ready_future<>();
        }
        return read_simple<component_type::Scylla>(*_components->scylla_metadata, pc);
    });
}

void
sstable::write_scylla_metadata(const io_priority_class& pc, shard_id shard, sstable_enabled_features features) {
    auto&& first_key = get_first_decorated_key();
    auto&& last_key = get_last_decorated_key();
    auto sm = create_sharding_metadata(_schema, first_key, last_key, shard);

    // sstable write may fail to generate empty metadata if mutation source has only data from other shard.
    // see https://github.com/scylladb/scylla/issues/2932 for details on how it can happen.
    if (sm.token_ranges.elements.empty()) {
        throw std::runtime_error(sprint("Failed to generate sharding metadata for %s", get_filename()));
    }

    if (!_components->scylla_metadata) {
        _components->scylla_metadata.emplace();
    }

    _components->scylla_metadata->data.set<scylla_metadata_type::Sharding>(std::move(sm));
    _components->scylla_metadata->data.set<scylla_metadata_type::Features>(std::move(features));

    write_simple<component_type::Scylla>(*_components->scylla_metadata, pc);
}

struct sstable_writer::writer_impl {
    virtual void consume_new_partition(const dht::decorated_key& dk) = 0;
    virtual void consume(tombstone t) = 0;
    virtual stop_iteration consume(static_row&& sr) = 0;
    virtual stop_iteration consume(clustering_row&& cr) = 0;
    virtual stop_iteration consume(range_tombstone&& rt) = 0;
    virtual stop_iteration consume_end_of_partition() = 0;
    virtual void consume_end_of_stream() = 0;
    virtual ~writer_impl() {}
};

class sstable_writer_k_l : public sstable_writer::writer_impl {
    sstable& _sst;
    const schema& _schema;
    const io_priority_class& _pc;
    bool _backup;
    bool _leave_unsealed;
    bool _compression_enabled;
    std::unique_ptr<file_writer> _writer;
    stdx::optional<components_writer> _components_writer;
    shard_id _shard; // Specifies which shard new sstable will belong to.
    write_monitor* _monitor;
    bool _correctly_serialize_non_compound_range_tombstones;
private:
    void prepare_file_writer();
    void finish_file_writer();
public:
    sstable_writer_k_l(sstable& sst, const schema& s, uint64_t estimated_partitions,
            const sstable_writer_config&, const io_priority_class& pc, shard_id shard = engine().cpu_id());
    ~sstable_writer_k_l();
    sstable_writer_k_l(sstable_writer_k_l&& o) : _sst(o._sst), _schema(o._schema), _pc(o._pc), _backup(o._backup),
            _leave_unsealed(o._leave_unsealed), _compression_enabled(o._compression_enabled), _writer(std::move(o._writer)),
            _components_writer(std::move(o._components_writer)), _shard(o._shard), _monitor(o._monitor),
            _correctly_serialize_non_compound_range_tombstones(o._correctly_serialize_non_compound_range_tombstones) { }
    void consume_new_partition(const dht::decorated_key& dk) override { return _components_writer->consume_new_partition(dk); }
    void consume(tombstone t) override { _components_writer->consume(t); }
    stop_iteration consume(static_row&& sr) override { return _components_writer->consume(std::move(sr)); }
    stop_iteration consume(clustering_row&& cr) override { return _components_writer->consume(std::move(cr)); }
    stop_iteration consume(range_tombstone&& rt) override { return _components_writer->consume(std::move(rt)); }
    stop_iteration consume_end_of_partition() override { return _components_writer->consume_end_of_partition(); }
    void consume_end_of_stream() override;
};

void sstable_writer_k_l::prepare_file_writer()
{
    file_output_stream_options options;
    options.io_priority_class = _pc;
    options.buffer_size = _sst.sstable_buffer_size;
    options.write_behind = 10;

    if (!_compression_enabled) {
        _writer = std::make_unique<adler32_checksummed_file_writer>(std::move(_sst._data_file), std::move(options));
    } else {
        _writer = std::make_unique<file_writer>(make_compressed_file_k_l_format_output_stream(
                std::move(_sst._data_file), std::move(options), &_sst._components->compression, _schema.get_compressor_params()));
    }
}

void sstable_writer_k_l::finish_file_writer()
{
    auto writer = std::move(_writer);
    writer->close().get();

    if (!_compression_enabled) {
        auto chksum_wr = static_cast<adler32_checksummed_file_writer*>(writer.get());
        write_digest(_sst.get_version(), _sst._write_error_handler, _sst.filename(component_type::Digest), chksum_wr->full_checksum());
        write_crc(_sst.get_version(), _sst._write_error_handler, _sst.filename(component_type::CRC), chksum_wr->finalize_checksum());
    } else {
        write_digest(_sst.get_version(), _sst._write_error_handler, _sst.filename(component_type::Digest), _sst._components->compression.get_full_checksum());
    }
}

sstable_writer_k_l::~sstable_writer_k_l() {
    if (_writer) {
        try {
            _writer->close().get();
        } catch (...) {
            sstlog.error("sstable_writer failed to close file: {}", std::current_exception());
        }
    }
}

sstable_writer_k_l::sstable_writer_k_l(sstable& sst, const schema& s, uint64_t estimated_partitions,
                               const sstable_writer_config& cfg, const io_priority_class& pc, shard_id shard)
    : _sst(sst)
    , _schema(s)
    , _pc(pc)
    , _backup(cfg.backup)
    , _leave_unsealed(cfg.leave_unsealed)
    , _shard(shard)
    , _monitor(cfg.monitor)
    , _correctly_serialize_non_compound_range_tombstones(cfg.correctly_serialize_non_compound_range_tombstones)
{
    _sst.generate_toc(_schema.get_compressor_params().get_compressor(), _schema.bloom_filter_fp_chance());
    _sst.write_toc(_pc);
    _sst.create_data().get();
    _compression_enabled = !_sst.has_component(component_type::CRC);
    prepare_file_writer();

    _monitor->on_write_started(_writer->offset_tracker());
    _components_writer.emplace(_sst, _schema, *_writer, estimated_partitions, cfg, _pc);
    _sst._shards = { shard };
}

static sstable_enabled_features all_features() {
    return sstable_enabled_features{(1 << sstable_feature::End) - 1};
}

void sstable_writer_k_l::consume_end_of_stream()
{
    _monitor->on_data_write_completed();

    _components_writer->consume_end_of_stream();
    _components_writer = stdx::nullopt;
    finish_file_writer();
    _sst.write_summary(_pc);
    _sst.write_filter(_pc);
    _sst.write_statistics(_pc);
    _sst.write_compression(_pc);
    auto features = all_features();
    if (!_correctly_serialize_non_compound_range_tombstones) {
        features.disable(sstable_feature::NonCompoundRangeTombstones);
    }
    _sst.write_scylla_metadata(_pc, _shard, std::move(features));

    _monitor->on_write_completed();

    if (!_leave_unsealed) {
        _sst.seal_sstable(_backup).get();
    }

    _monitor->on_flush_completed();
}

enum class cell_flags : uint8_t {
    none = 0x00,
    is_deleted_mask = 0x01, // Whether the cell is a tombstone or not.
    is_expiring_mask = 0x02, // Whether the cell is expiring.
    has_empty_value_mask = 0x04, // Whether the cell has an empty value. This will be the case for a tombstone in particular.
    use_row_timestamp_mask = 0x08, // Whether the cell has the same timestamp as the row this is a cell of.
    use_row_ttl_mask = 0x10, // Whether the cell has the same TTL as the row this is a cell of.
};

inline cell_flags operator& (cell_flags lhs, cell_flags rhs) {
    return cell_flags(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
}

inline cell_flags& operator |= (cell_flags& lhs, cell_flags rhs) {
    lhs = cell_flags(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
    return lhs;
}

enum class row_flags : uint8_t {
    none = 0x00,
    // Signal the end of the partition. Nothing follows a <flags> field with that flag.
    end_of_partition = 0x01,
    // Whether the encoded unfiltered is a marker or a row. All following flags apply only to rows.
    is_marker = 0x02,
    // Whether the encoded row has a timestamp (i.e. its liveness_info is not empty).
    has_timestamp = 0x04,
    // Whether the encoded row has some expiration info (i.e. if its liveness_info contains TTL and local_deletion).
    has_ttl = 0x08,
    // Whether the encoded row has some deletion info.
    has_deletion = 0x10,
    // Whether the encoded row has all of the columns from the header present.
    has_all_columns = 0x20,
    // Whether the encoded row has some complex deletion for at least one of its complex columns.
    has_complex_deletion = 0x40,
    // If present, another byte is read containing the "extended flags" below.
    extension_flag = 0x80
};

inline row_flags operator& (row_flags lhs, row_flags rhs) {
    return row_flags(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
}

inline row_flags& operator |= (row_flags& lhs, row_flags rhs) {
    lhs = row_flags(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
    return lhs;
}

enum class row_extended_flags : uint8_t {
    none = 0x00,
    // Whether the encoded row is a static. If there is no extended flag, the row is assumed not static.
    is_static = 0x01,
    // Cassandra-specific flag, indicates whether the row deletion is shadowable.
    // This flag is deprecated in Origin - see CASSANDRA-11500.
    // This flag is never set by Scylla and it fails to read files that have it set.
    has_shadowable_deletion_cassandra = 0x02,
    // Scylla-specific flag, indicates whether the row deletion is shadowable.
    // If set, the shadowable tombstone is writen right after the row deletion.
    // This is only used by Materialized Views that are not supposed to be exported.
    has_shadowable_deletion_scylla = 0x80,
};

// A range tombstone marker (RT marker) represents a bound of a range tombstone
// in a SSTables 3.x ('m') data file.
// RT markers can be of two types called "bounds" and "boundaries" in Origin nomenclature.
//
// A bound simply represents either a start or an end bound of a full range tombstone.
//
// A boundary can be thought of as two merged adjacent bounds and is used to represent adjacent
// range tombstones. An RT marker of a boundary type has two tombstones corresponding to two
// range tombstones this boundary belongs to.
struct rt_marker {
    clustering_key_prefix clustering;
    bound_kind_m kind;
    tombstone tomb;
    std::optional<tombstone> boundary_tomb; // only engaged for rt_marker of a boundary type

    position_in_partition_view position() const {
        return {
            position_in_partition_view::range_tombstone_tag_t(),
            bound_view{clustering, to_bound_kind(kind)}
        };
    }

    // We need this one to uniformly write rows and RT markers inside write_clustered().
    const clustering_key_prefix& key() const { return clustering; }
};

static bound_kind_m get_kind(const clustering_row&) {
    return bound_kind_m::clustering;
}

static bound_kind_m get_kind(const rt_marker& marker) {
    return marker.kind;
}

GCC6_CONCEPT(
    template<typename T>
    concept bool Clustered = requires(T t) {
        { t.key() } -> const clustering_key_prefix&;
        { get_kind(t) } -> bound_kind_m;
    };
)

static indexed_columns get_indexed_columns_partitioned_by_atomicity(schema::const_iterator_range_type columns) {
    indexed_columns result;
    result.reserve(columns.size());
    for (const auto& col: columns) {
        result.emplace_back(col);
    }
    boost::range::stable_partition(
            result,
            [](const std::reference_wrapper<const column_definition>& column) { return column.get().is_atomic();});
    return result;
}

// Used for writing SSTables in 'mc' format.
class sstable_writer_m : public sstable_writer::writer_impl {
private:
    sstable& _sst;
    const schema& _schema;
    const io_priority_class& _pc;
    sstable_writer_config _cfg;
    encoding_stats _enc_stats;
    shard_id _shard; // Specifies which shard the new SStable will belong to.
    bool _compression_enabled = false;
    std::unique_ptr<file_writer> _data_writer;
    std::optional<file_writer> _index_writer;
    bool _tombstone_written = false;
    bool _static_row_written = false;
    // The length of partition header (partition key, partition deletion and static row, if present)
    // as written to the data file
    // Used for writing promoted index
    uint64_t _partition_header_length = 0;
    uint64_t _prev_row_start = 0;
    std::optional<key> _partition_key;
    stdx::optional<key> _first_key, _last_key;
    index_sampling_state _index_sampling_state;
    range_tombstone_stream _range_tombstones;
    memory_data_sink_buffers _tmp_bufs;
    file_writer _tmp_writer; // writes into _tmp_bufs.

    // For static and regular columns, we write all simple columns first followed by collections
    // These containers have columns partitioned by atomicity
    const indexed_columns _static_columns;
    const indexed_columns _regular_columns;

    struct cdef_and_collection {
        const column_definition* cdef;
        std::reference_wrapper<const atomic_cell_or_collection> collection;
    };

    // Used to defer writing collections until all atomic cells are written
    std::vector<cdef_and_collection> _collections;

    std::optional<rt_marker> _end_open_marker;

    struct clustering_info {
        clustering_key_prefix clustering;
        bound_kind_m kind;
    };
    struct pi_block {
        clustering_info first;
        clustering_info last;
        uint64_t offset;
        uint64_t width;
        std::optional<tombstone> open_marker;
    };
    // _pi_write_m is used temporarily for building the promoted
    // index (column sample) of one partition when writing a new sstable.
    struct {
        // Unfortunately we cannot output the promoted index directly to the
        // index file because it needs to be prepended by its size.
        seastar::circular_buffer<pi_block> promoted_index;
        tombstone tomb;
        uint64_t block_start_offset;
        uint64_t block_next_start_offset;
        std::optional<clustering_info> first_clustering;
        std::optional<clustering_info> last_clustering;
        size_t desired_block_size;
    } _pi_write_m;
    column_stats _c_stats;

    void init_file_writers();
    void close_data_writer();
    void ensure_tombstone_is_written() {
        if (!_tombstone_written) {
            consume(tombstone());
        }
    }

    void ensure_static_row_is_written_if_needed() {
        if (!_static_columns.empty() && !_static_row_written) {
            consume(static_row{});
        }
    }

    void drain_tombstones(std::optional<position_in_partition_view> pos = {});

    void maybe_add_summary_entry(const dht::token& token, bytes_view key) {
        return components_writer::maybe_add_summary_entry(
            _sst._components->summary, token, key, get_data_offset(),
            _index_writer->offset(), _index_sampling_state);
    }

    void maybe_set_pi_first_clustering(const clustering_info& info);
    void maybe_add_pi_block();
    void add_pi_block();

    uint64_t get_data_offset() const {
        if (_sst.has_component(component_type::CompressionInfo)) {
            // Variable returned by compressed_file_length() is constantly updated by compressed output stream.
            return _sst._components->compression.compressed_file_length();
        } else {
            return _data_writer->offset();
        }
    }

    void write_delta_timestamp(file_writer& writer, api::timestamp_type timestamp) {
        sstables::write_delta_timestamp(writer, timestamp, _enc_stats);
    }
    void write_delta_ttl(file_writer& writer, uint32_t ttl) {
        sstables::write_delta_ttl(writer, ttl, _enc_stats);
    }
    void write_delta_local_deletion_time(file_writer& writer, uint32_t ldt) {
        sstables::write_delta_local_deletion_time(writer, ldt, _enc_stats);
    }
    void write_delta_deletion_time(file_writer& writer, deletion_time dt) {
        sstables::write_delta_deletion_time(writer, dt, _enc_stats);
    }

    struct row_time_properties {
        std::optional<api::timestamp_type> timestamp;
        std::optional<uint32_t> ttl;
        std::optional<uint32_t> local_deletion_time;
    };

    // Writes single atomic cell
    void write_cell(file_writer& writer, atomic_cell_view cell, const column_definition& cdef,
                    const row_time_properties& properties, bytes_view cell_path = {});

    // Writes information about row liveness (formerly 'row marker')
    void write_liveness_info(file_writer& writer, const row_marker& marker);

    // Writes a CQL collection (list, set or map)
    void write_collection(file_writer& writer, const column_definition& cdef, collection_mutation_view collection,
                          const row_time_properties& properties, bool has_complex_deletion);

    void write_cells(file_writer& writer, column_kind kind, const row& row_body, const row_time_properties& properties, bool has_complex_deletion);
    void write_row_body(file_writer& writer, const clustering_row& row, bool has_complex_deletion);
    void write_static_row(const row& static_row);

    // Clustered is a term used to denote an entity that has a clustering key prefix
    // and constitutes an entry of a partition.
    // Both clustered_rows and rt_markers are instances of Clustered
    void write_clustered(const clustering_row& clustered_row, uint64_t prev_row_size);
    void write_clustered(const rt_marker& marker, uint64_t prev_row_size);

    template <typename T>
    GCC6_CONCEPT(
        requires Clustered<T>
    )
    void write_clustered(const T& clustered) {
        clustering_info info {clustered.key(), get_kind(clustered)};
        maybe_set_pi_first_clustering(info);
        uint64_t pos = _data_writer->offset();
        write_clustered(clustered, pos - _prev_row_start);
        _pi_write_m.last_clustering = info;
        _prev_row_start = pos;
        maybe_add_pi_block();
    }
    void write_promoted_index(file_writer& writer);
    void consume(rt_marker&& marker);

    void flush_tmp_bufs() {
        for (auto&& buf : _tmp_bufs.buffers()) {
            _data_writer->write(buf.get(), buf.size()).get();
        }
        _tmp_bufs.clear();
    }
public:

    sstable_writer_m(sstable& sst, const schema& s, uint64_t estimated_partitions,
            const sstable_writer_config& cfg, encoding_stats enc_stats,
                      const io_priority_class& pc, shard_id shard = engine().cpu_id())
        : _sst(sst)
        , _schema(s)
        , _pc(pc)
        , _cfg(cfg)
        , _enc_stats(enc_stats)
        , _shard(shard)
        , _range_tombstones(_schema)
        , _tmp_writer(output_stream<char>(data_sink(std::make_unique<memory_data_sink>(_tmp_bufs)), _sst.sstable_buffer_size))
        , _static_columns(get_indexed_columns_partitioned_by_atomicity(s.static_columns()))
        , _regular_columns(get_indexed_columns_partitioned_by_atomicity(s.regular_columns()))
    {
        _sst.generate_toc(_schema.get_compressor_params().get_compressor(), _schema.bloom_filter_fp_chance());
        _sst.write_toc(_pc);
        _sst.create_data().get();
        _compression_enabled = !_sst.has_component(component_type::CRC);
        init_file_writers();
        _sst._shards = { shard };

        _cfg.monitor->on_write_started(_data_writer->offset_tracker());
        _sst._components->filter = utils::i_filter::get_filter(estimated_partitions, _schema.bloom_filter_fp_chance(), utils::filter_format::m_format);
        _pi_write_m.desired_block_size = cfg.promoted_index_block_size.value_or(get_config().column_index_size_in_kb() * 1024);
        _sst._correctly_serialize_non_compound_range_tombstones = _cfg.correctly_serialize_non_compound_range_tombstones;
        _index_sampling_state.summary_byte_cost = summary_byte_cost();
        prepare_summary(_sst._components->summary, estimated_partitions, _schema.min_index_interval());
    }

    ~sstable_writer_m();
    sstable_writer_m(sstable_writer_m&& o) = default;
    void consume_new_partition(const dht::decorated_key& dk) override;
    void consume(tombstone t) override;
    stop_iteration consume(static_row&& sr) override;
    stop_iteration consume(clustering_row&& cr) override;
    stop_iteration consume(range_tombstone&& rt) override;
    stop_iteration consume_end_of_partition();
    void consume_end_of_stream() override;
};

sstable_writer_m::~sstable_writer_m() {
    auto close_writer = [](auto& writer) {
        if (writer) {
            try {
                writer->close().get();
            } catch (...) {
                sstlog.error("sstable_writer_m failed to close file: {}", std::current_exception());
            }
        }
    };
    close_writer(_index_writer);
    close_writer(_data_writer);
}

void sstable_writer_m::maybe_set_pi_first_clustering(const sstable_writer_m::clustering_info& info) {
    uint64_t pos = _data_writer->offset();
    if (!_pi_write_m.first_clustering) {
        _pi_write_m.first_clustering = info;
        _pi_write_m.block_start_offset = pos;
        _pi_write_m.block_next_start_offset = pos + _pi_write_m.desired_block_size;
    }
}

static deletion_time to_deletion_time(tombstone t) {
    deletion_time dt;
    if (t) {
        dt.local_deletion_time = t.deletion_time.time_since_epoch().count();
        dt.marked_for_delete_at = t.timestamp;
    } else {
        // Default values for live, non-deleted rows.
        dt.local_deletion_time = std::numeric_limits<int32_t>::max();
        dt.marked_for_delete_at = std::numeric_limits<int64_t>::min();
    }
    return dt;
}

void sstable_writer_m::add_pi_block() {
    _pi_write_m.promoted_index.push_back({
        *_pi_write_m.first_clustering,
        *_pi_write_m.last_clustering,
        _pi_write_m.block_start_offset - _c_stats.start_offset,
        _data_writer->offset() - _pi_write_m.block_start_offset,
        (_end_open_marker ? std::make_optional(_end_open_marker->tomb) : std::optional<tombstone>{})});
}

void sstable_writer_m::maybe_add_pi_block() {
    uint64_t pos = _data_writer->offset();
    if (pos >= _pi_write_m.block_next_start_offset) {
        add_pi_block();
        _pi_write_m.first_clustering.reset();
        _pi_write_m.block_next_start_offset = pos + _pi_write_m.desired_block_size;
    }
}

void sstable_writer_m::init_file_writers() {
    file_output_stream_options options;
    options.io_priority_class = _pc;
    options.buffer_size = _sst.sstable_buffer_size;
    options.write_behind = 10;

    if (!_compression_enabled) {
        _data_writer = std::make_unique<crc32_checksummed_file_writer>(std::move(_sst._data_file), options);
    } else {
        _data_writer = std::make_unique<file_writer>(
            make_compressed_file_m_format_output_stream(
                    std::move(_sst._data_file),
                    options,
                    &_sst._components->compression,
                    _schema.get_compressor_params()));
    }
    _index_writer.emplace(std::move(_sst._index_file), options);
}

void sstable_writer_m::close_data_writer() {
    auto writer = std::move(_data_writer);
    writer->close().get();

    if (!_compression_enabled) {
        auto chksum_wr = static_cast<crc32_checksummed_file_writer*>(writer.get());
        write_digest(_sst.get_version(), _sst._write_error_handler, _sst.filename(component_type::Digest), chksum_wr->full_checksum());
        write_crc(_sst.get_version(), _sst._write_error_handler, _sst.filename(component_type::CRC), chksum_wr->finalize_checksum());
    } else {
        write_digest(
            _sst.get_version(),
            _sst._write_error_handler,
            _sst.filename(component_type::Digest),
            _sst._components->compression.get_full_checksum());
    }
}

void sstable_writer_m::drain_tombstones(std::optional<position_in_partition_view> pos) {
    auto get_next_rt = [this, &pos] {
        return pos ? _range_tombstones.get_next(*pos) : _range_tombstones.get_next();
    };

    auto get_rt_start = [] (const range_tombstone& rt) {
        return rt_marker{rt.start, to_bound_kind_m(rt.start_kind), rt.tomb};
    };

    auto get_rt_end = [] (const range_tombstone& rt) {
        return rt_marker{rt.end, to_bound_kind_m(rt.end_kind), rt.tomb};
    };

    auto flush_end_open_marker = [this] {
        consume(*std::exchange(_end_open_marker, {}));
    };

    auto write_rt_boundary = [this, &get_rt_end] (const range_tombstone& rt) {
        auto boundary_kind = rt.start_kind == bound_kind::incl_start
                ? bound_kind_m::excl_end_incl_start
                : bound_kind_m::incl_end_excl_start;
        tombstone end_tomb{std::move(_end_open_marker->tomb)};
        _end_open_marker = get_rt_end(rt);
        consume(rt_marker{rt.start, boundary_kind, end_tomb, rt.tomb});
    };

    ensure_tombstone_is_written();
    ensure_static_row_is_written_if_needed();
    position_in_partition::less_compare less{_schema};
    position_in_partition::equal_compare eq{_schema};
    while (auto mfo = get_next_rt()) {
        range_tombstone rt {std::move(mfo)->as_range_tombstone()};
        bool need_write_start = true;
        if (_end_open_marker) {
            if (eq(_end_open_marker->position(), rt.position())) {
                write_rt_boundary(rt);
                need_write_start = false;
            } else if (less(rt.position(), _end_open_marker->position())) {
                if (_end_open_marker->tomb != rt.tomb) {
                    // Previous end marker has been superseded by a range tombstone that was added later
                    // so we end the currently open one and start the new one at once
                    write_rt_boundary(rt);
                    need_write_start = false;
                } else {
                    // The range tombstone is a continuation of the currently open one
                    // so don't need to write the start marker, just update the end
                    _end_open_marker = get_rt_end(rt);
                    need_write_start = false;
                }
            } else {
                // The new range tombstone lies entirely after the currently open one.
                // Simply close it.
                flush_end_open_marker();
            }
        }

        if (need_write_start) {
            _end_open_marker = get_rt_end(rt);
            consume(get_rt_start(rt));
        }

        if (pos && rt.trim_front(_schema, *pos)) {
            _range_tombstones.apply(std::move(rt));
        }
    }

    if (_end_open_marker && (!pos || less(_end_open_marker->position(), *pos))) {
        flush_end_open_marker();
    }
}

void sstable_writer_m::consume_new_partition(const dht::decorated_key& dk) {
    _c_stats.start_offset = _data_writer->offset();
    _prev_row_start = _data_writer->offset();

    _partition_key = key::from_partition_key(_schema, dk.key());
    maybe_add_summary_entry(dk.token(), bytes_view(*_partition_key));

    _sst._components->filter->add(bytes_view(*_partition_key));
    _sst.get_metadata_collector().add_key(bytes_view(*_partition_key));

    auto p_key = disk_string_view<uint16_t>();
    p_key.value = bytes_view(*_partition_key);

    // Write index file entry from partition key into index file.
    // Write an index entry minus the "promoted index" (sample of columns)
    // part. We can only write that after processing the entire partition
    // and collecting the sample of columns.
    write(_sst.get_version(), *_index_writer, p_key);
    write_vint(*_index_writer, _data_writer->offset());

    _pi_write_m.promoted_index = {};
    _pi_write_m.tomb = {};
    _pi_write_m.first_clustering.reset();
    _pi_write_m.last_clustering.reset();

    write(_sst.get_version(), *_data_writer, p_key);
    _partition_header_length = _data_writer->offset() - _c_stats.start_offset;

    _tombstone_written = false;
    _static_row_written = false;
}

void sstable_writer_m::consume(tombstone t) {
    uint64_t current_pos = _data_writer->offset();
    auto dt = to_deletion_time(t);
    write(_sst.get_version(), *_data_writer, dt);
    _partition_header_length += (_data_writer->offset() - current_pos);
    if (t) {
        _c_stats.tombstone_histogram.update(dt.local_deletion_time);
        _c_stats.update_local_deletion_time(dt.local_deletion_time);
        _c_stats.update_timestamp(dt.marked_for_delete_at);
    }

    _pi_write_m.tomb = t;
    _tombstone_written = true;
}

void sstable_writer_m::write_cell(file_writer& writer, atomic_cell_view cell, const column_definition& cdef,
        const row_time_properties& properties, bytes_view cell_path) {

    bool is_deleted = !cell.is_live();
    bool has_value = !is_deleted && !cell.value().empty();
    bool use_row_timestamp = (properties.timestamp == cell.timestamp());
    bool is_row_expiring = properties.ttl.has_value();
    bool is_cell_expiring = cell.is_live_and_has_ttl();
    bool use_row_ttl = is_row_expiring && is_cell_expiring &&
                       properties.ttl == cell.ttl().count() &&
                       properties.local_deletion_time == cell.deletion_time().time_since_epoch().count();

    cell_flags flags = cell_flags::none;
    if (!has_value) {
        flags |= cell_flags::has_empty_value_mask;
    }
    if (is_deleted) {
        flags |= cell_flags::is_deleted_mask;
    } else if (is_cell_expiring) {
        flags |= cell_flags::is_expiring_mask;
    }
    if (use_row_timestamp) {
        flags |= cell_flags::use_row_timestamp_mask;
    }
    if (use_row_ttl) {
        flags |= cell_flags::use_row_ttl_mask;
    }
    write(_sst.get_version(), writer, flags);

    if (!use_row_timestamp) {
        write_delta_timestamp(writer, cell.timestamp());
    }

    if (!use_row_ttl) {
        if (is_deleted) {
            write_delta_local_deletion_time(writer, cell.deletion_time().time_since_epoch().count());
        } else if (is_cell_expiring) {
            write_delta_local_deletion_time(writer, cell.expiry().time_since_epoch().count());
            write_delta_ttl(writer, cell.ttl().count());
        }
    }

    if (!cell_path.empty()) {
        write_vint(writer, cell_path.size());
        write(_sst.get_version(), writer, cell_path);
    }

    if (has_value) {
        if (cdef.is_counter()) {
            assert(!cell.is_counter_update());
          counter_cell_view::with_linearized(cell, [&] (counter_cell_view ccv) {
            write_counter_value(ccv, writer, sstable_version_types::mc, [] (file_writer& out, uint32_t value) {
                return write_vint(out, value);
            });
          });
        } else {
            write_cell_value(writer, *cdef.type, cell.value());
        }
    }

    // Collect cell statistics
    _c_stats.update_timestamp(cell.timestamp());
    if (is_deleted) {
        auto ldt = cell.deletion_time().time_since_epoch().count();
        _c_stats.update_local_deletion_time(ldt);
        _c_stats.tombstone_histogram.update(ldt);
    } else if (is_cell_expiring) {
        auto expiration = cell.expiry().time_since_epoch().count();
        auto ttl = cell.ttl().count();
        _c_stats.update_ttl(ttl);
        _c_stats.update_local_deletion_time(expiration);
        // tombstone histogram is updated with expiration time because if ttl is longer
        // than gc_grace_seconds for all data, sstable will be considered fully expired
        // when actually nothing is expired.
        _c_stats.tombstone_histogram.update(expiration);
    } else { // regular live cell
        _c_stats.update_local_deletion_time(std::numeric_limits<int>::max());
    }
}

void sstable_writer_m::write_liveness_info(file_writer& writer, const row_marker& marker) {
    if (marker.is_missing()) {
        return;
    }

    uint64_t timestamp = marker.timestamp();
    _c_stats.update_timestamp(timestamp);
    write_delta_timestamp(writer, timestamp);

    auto write_expiring_liveness_info = [this, &writer] (uint32_t ttl, uint64_t ldt) {
        _c_stats.update_ttl(ttl);
        _c_stats.update_local_deletion_time(ldt);
        write_delta_ttl(writer, ttl);
        write_delta_local_deletion_time(writer, ldt);
    };
    if (!marker.is_live()) {
        write_expiring_liveness_info(expired_liveness_ttl, marker.deletion_time().time_since_epoch().count());
    } else if (marker.is_expiring()) {
        write_expiring_liveness_info(marker.ttl().count(), marker.expiry().time_since_epoch().count());
    }
}

void sstable_writer_m::write_collection(file_writer& writer, const column_definition& cdef,
        collection_mutation_view collection, const row_time_properties& properties, bool has_complex_deletion) {
    auto& ctype = *static_pointer_cast<const collection_type_impl>(cdef.type);
    collection.data.with_linearized([&] (bytes_view collection_bv) {
        auto mview = ctype.deserialize_mutation_form(collection_bv);
        if (has_complex_deletion) {
            auto dt = to_deletion_time(mview.tomb);
            write_delta_deletion_time(writer, dt);
            if (mview.tomb) {
                _c_stats.update_timestamp(dt.marked_for_delete_at);
                _c_stats.update_local_deletion_time(dt.local_deletion_time);
            }
        }

        write_vint(writer, mview.cells.size());
        if (!mview.cells.empty()) {
            ++_c_stats.column_count;
        }
        for (const auto& [cell_path, cell]: mview.cells) {
            ++_c_stats.cells_count;
            write_cell(writer, cell, cdef, properties, cell_path);
        }
    });
}

void sstable_writer_m::write_cells(file_writer& writer, column_kind kind, const row& row_body,
        const row_time_properties& properties, bool has_complex_deletion) {
    // Note that missing columns are written based on the whole set of regular columns as defined by schema.
    // This differs from Origin where all updated columns are tracked and the set of filled columns of a row
    // is compared with the set of all columns filled in the memtable. So our encoding may be less optimal in some cases
    // but still valid.
    write_missing_columns(writer, kind == column_kind::static_column ? _static_columns : _regular_columns, row_body);
    row_body.for_each_cell([this, &writer, kind, &properties, has_complex_deletion] (column_id id, const atomic_cell_or_collection& c) {
        auto&& column_definition = _schema.column_at(kind, id);
        if (!column_definition.is_atomic()) {
            _collections.push_back({&column_definition, c});
            return;
        }
        atomic_cell_view cell = c.as_atomic_cell(column_definition);
        ++_c_stats.cells_count;
        ++_c_stats.column_count;
        write_cell(writer, cell, column_definition, properties);
    });

    for (const auto& col: _collections) {
        write_collection(writer, *col.cdef, col.collection.get().as_collection_mutation(), properties, has_complex_deletion);
    }
    _collections.clear();
}

void sstable_writer_m::write_row_body(file_writer& writer, const clustering_row& row, bool has_complex_deletion) {
    write_liveness_info(writer, row.marker());
    auto write_tombstone_and_update_stats = [this, &writer] (const tombstone& t) {
         auto dt = to_deletion_time(t);
        _c_stats.update_timestamp(dt.marked_for_delete_at);
        _c_stats.update_local_deletion_time(dt.local_deletion_time);
        write_delta_deletion_time(writer, dt);
    };
    if (row.tomb().regular()) {
        write_tombstone_and_update_stats(row.tomb().regular());
    }
    if (row.tomb().is_shadowable()) {
        write_tombstone_and_update_stats(row.tomb().tomb());
    }
    row_time_properties properties;
    if (!row.marker().is_missing()) {
        properties.timestamp = row.marker().timestamp();
        if (row.marker().is_expiring()) {
            properties.ttl = row.marker().ttl().count();
            properties.local_deletion_time = row.marker().deletion_time().time_since_epoch().count();
        }
    }

    return write_cells(writer, column_kind::regular_column, row.cells(), properties, has_complex_deletion);
}

template <typename Func>
uint64_t calculate_write_size(Func&& func) {
    uint64_t written_size = 0;
    {
        auto counting_writer = file_writer(make_sizing_output_stream(written_size));
        func(counting_writer);
        counting_writer.flush().get();
        counting_writer.close().get();
    }
    return written_size;
}

// Find if any collection in the row contains a collection-wide tombstone
static bool row_has_complex_deletion(const schema& s, const row& r, column_kind kind) {
    bool result = false;
    r.for_each_cell_until([&] (column_id id, const atomic_cell_or_collection& c) {
        auto&& cdef = s.column_at(kind, id);
        if (cdef.is_atomic()) {
            return stop_iteration::no;
        }
        auto t = static_pointer_cast<const collection_type_impl>(cdef.type);
        return c.as_collection_mutation().data.with_linearized([&] (bytes_view c_bv) {
            auto mview = t->deserialize_mutation_form(c_bv);
            if (mview.tomb) {
                result = true;
            }
            return stop_iteration(static_cast<bool>(mview.tomb));
        });
    });

    return result;
}

void sstable_writer_m::write_static_row(const row& static_row) {
    assert(_schema.is_compound());

    uint64_t current_pos = _data_writer->offset();
    // Static row flag is stored in extended flags so extension_flag is always set for static rows
    row_flags flags = row_flags::extension_flag;
    if (static_row.size() == _schema.static_columns_count()) {
        flags |= row_flags::has_all_columns;
    }
    bool has_complex_deletion = row_has_complex_deletion(_schema, static_row, column_kind::static_column);
    if (has_complex_deletion) {
        flags |= row_flags::has_complex_deletion;
    }
    write(_sst.get_version(), *_data_writer, flags);
    write(_sst.get_version(), *_data_writer, row_extended_flags::is_static);

    write_cells(_tmp_writer, column_kind::static_column, static_row, row_time_properties{}, has_complex_deletion);
    _tmp_writer.flush().get();

    uint64_t row_body_size = _tmp_bufs.size() + unsigned_vint::serialized_size(0);
    write_vint(*_data_writer, row_body_size);
    write_vint(*_data_writer, 0); // as the static row always comes first, the previous row size is always zero
    flush_tmp_bufs();

    _partition_header_length += (_data_writer->offset() - current_pos);

    // Collect statistics
    ++_c_stats.rows_count;
}

stop_iteration sstable_writer_m::consume(static_row&& sr) {
    ensure_tombstone_is_written();
    write_static_row(sr.cells());
    _static_row_written = true;
    return stop_iteration::no;
}

void sstable_writer_m::write_clustered(const clustering_row& clustered_row, uint64_t prev_row_size) {
    row_flags flags = row_flags::none;
    row_extended_flags ext_flags = row_extended_flags::none;
    const row_marker& marker = clustered_row.marker();
    if (!marker.is_missing()) {
        flags |= row_flags::has_timestamp;
        if (!marker.is_live() || marker.is_expiring()) {
            flags |= row_flags::has_ttl;
        }
    }

    if (clustered_row.tomb().regular()) {
        flags |= row_flags::has_deletion;
    }
    if (clustered_row.tomb().is_shadowable()) {
        flags |= row_flags::extension_flag;
        ext_flags = row_extended_flags::has_shadowable_deletion_scylla;
    }

    if (clustered_row.cells().size() == _schema.regular_columns_count()) {
        flags |= row_flags::has_all_columns;
    }
    bool has_complex_deletion = row_has_complex_deletion(_schema, clustered_row.cells(), column_kind::regular_column);
    if (has_complex_deletion) {
        flags |= row_flags::has_complex_deletion;
    }
    write(_sst.get_version(), *_data_writer, flags);
    if (ext_flags != row_extended_flags::none) {
        write(_sst.get_version(), *_data_writer, ext_flags);
    }

    write_clustering_prefix(*_data_writer, _schema, clustered_row.key(), ephemerally_full_prefix{_schema.is_compact_table()});

    write_row_body(_tmp_writer, clustered_row, has_complex_deletion);
    _tmp_writer.flush().get();

    uint64_t row_body_size = _tmp_bufs.size() + unsigned_vint::serialized_size(prev_row_size);
    write_vint(*_data_writer, row_body_size);
    write_vint(*_data_writer, prev_row_size);
    flush_tmp_bufs();

    // Collect statistics
    if (_schema.clustering_key_size()) {
        column_name_helper::min_max_components(_schema, _sst.get_metadata_collector().min_column_names(),
            _sst.get_metadata_collector().max_column_names(), clustered_row.key().components());
    }
    ++_c_stats.rows_count;
}

stop_iteration sstable_writer_m::consume(clustering_row&& cr) {
    drain_tombstones(position_in_partition_view::after_key(cr.key()));
    write_clustered(cr);
    return stop_iteration::no;
}

// Write clustering prefix along with its bound kind and, if not full, its size
static void write_clustering_prefix(file_writer& writer, bound_kind_m kind,
        const schema& s, const clustering_key_prefix& clustering) {
    assert(kind != bound_kind_m::static_clustering);
    write(sstable_version_types::mc, writer, kind);
    auto is_ephemerally_full = ephemerally_full_prefix{s.is_compact_table()};
    if (kind != bound_kind_m::clustering) {
        // Don't use ephemerally full for RT bounds as they're always non-full
        is_ephemerally_full = ephemerally_full_prefix::no;
        write(sstable_version_types::mc, writer, static_cast<uint16_t>(clustering.size(s)));
    }
    write_clustering_prefix(writer, s, clustering, is_ephemerally_full);
}

void sstable_writer_m::write_promoted_index(file_writer& writer) {
    static constexpr size_t width_base = 65536;
    write_vint(writer, _partition_header_length);
    write(_sst.get_version(), writer, to_deletion_time(_pi_write_m.tomb));
    write_vint(writer, _pi_write_m.promoted_index.size());
    std::vector<uint32_t> offsets;
    offsets.reserve(_pi_write_m.promoted_index.size());
    uint64_t start = writer.offset();
    for (const pi_block& block: _pi_write_m.promoted_index) {
        offsets.push_back(writer.offset() - start);
        write_clustering_prefix(writer, block.first.kind, _schema, block.first.clustering);
        write_clustering_prefix(writer, block.last.kind, _schema, block.last.clustering);
        write_vint(writer, block.offset);
        write_signed_vint(writer, block.width - width_base);
        write(_sst.get_version(), writer, static_cast<std::byte>(block.open_marker ? 1 : 0));
        if (block.open_marker) {
            write(sstable_version_types::mc, writer, to_deletion_time(*block.open_marker));
        }
    }

    for (uint32_t offset: offsets) {
        write(_sst.get_version(), writer, offset);
    }
}

void sstable_writer_m::write_clustered(const rt_marker& marker, uint64_t prev_row_size) {
    write(sstable_version_types::mc, *_data_writer, row_flags::is_marker);
    write_clustering_prefix(*_data_writer, marker.kind, _schema, marker.clustering);
    auto write_marker_body = [this, &marker] (file_writer& writer) {
        write_delta_deletion_time(writer, to_deletion_time(marker.tomb));
        if (marker.boundary_tomb) {
            write_delta_deletion_time(writer, to_deletion_time(*marker.boundary_tomb));
        }
    };

    uint64_t marker_body_size = calculate_write_size(write_marker_body) + unsigned_vint::serialized_size(prev_row_size);

    write_vint(*_data_writer, marker_body_size);
    write_vint(*_data_writer, prev_row_size);

    write_marker_body(*_data_writer);
};

void sstable_writer_m::consume(rt_marker&& marker) {
    write_clustered(marker);
}

stop_iteration sstable_writer_m::consume(range_tombstone&& rt) {
    drain_tombstones(rt.position());
    _range_tombstones.apply(std::move(rt));
    return stop_iteration::no;
}

stop_iteration sstable_writer_m::consume_end_of_partition() {
    drain_tombstones();

    write(_sst.get_version(), *_data_writer, row_flags::end_of_partition);

    if (!_pi_write_m.promoted_index.empty() && _pi_write_m.first_clustering) {
        add_pi_block();
    }

    auto write_pi = [this] (file_writer& writer) {
        return write_promoted_index(writer);
    };

    if (_pi_write_m.promoted_index.size() < 2) {
        write_vint(*_index_writer, uint64_t(0));
    } else {
        uint64_t pi_size = calculate_write_size(write_pi);
        write_vint(*_index_writer, pi_size);
        write_pi(*_index_writer);
    }

    // compute size of the current row.
    _c_stats.partition_size = _data_writer->offset() - _c_stats.start_offset;

    _cfg.large_partition_handler->maybe_update_large_partitions(_sst, *_partition_key, _c_stats.partition_size).get();

    // update is about merging column_stats with the data being stored by collector.
    _sst.get_metadata_collector().update(std::move(_c_stats));
    _c_stats.reset();

    if (!_first_key) {
        _first_key = *_partition_key;
    }
    _last_key = std::move(*_partition_key);
    return get_data_offset() < _cfg.max_sstable_size ? stop_iteration::no : stop_iteration::yes;
}

void sstable_writer_m::consume_end_of_stream() {
    _cfg.monitor->on_data_write_completed();

    seal_summary(_sst._components->summary, std::move(_first_key), std::move(_last_key), _index_sampling_state);

    if (_sst.has_component(component_type::CompressionInfo)) {
        _sst.get_metadata_collector().add_compression_ratio(_sst._components->compression.compressed_file_length(), _sst._components->compression.uncompressed_file_length());
    }

    _index_writer->close().get();
    _index_writer.reset();
    _sst.set_first_and_last_keys();
    seal_statistics(_sst.get_version(), _sst._components->statistics, _sst.get_metadata_collector(),
            dht::global_partitioner().name(), _schema.bloom_filter_fp_chance(),
            _sst._schema, _sst.get_first_decorated_key(), _sst.get_last_decorated_key(), _enc_stats);
    close_data_writer();
    _sst.write_summary(_pc);
    _sst.write_filter(_pc);
    _sst.write_statistics(_pc);
    _sst.write_compression(_pc);
    auto features = all_features();
    if (!_cfg.correctly_serialize_non_compound_range_tombstones) {
        features.disable(sstable_feature::NonCompoundRangeTombstones);
    }
    _sst.write_scylla_metadata(_pc, _shard, std::move(features));
    _cfg.monitor->on_write_completed();
    if (!_cfg.leave_unsealed) {
        _sst.seal_sstable(_cfg.backup).get();
    }
    _tmp_writer.close().get();
    _cfg.monitor->on_flush_completed();
}

sstable_writer::sstable_writer(sstable& sst, const schema& s, uint64_t estimated_partitions,
        const sstable_writer_config& cfg, encoding_stats enc_stats, const io_priority_class& pc, shard_id shard) {
    if (sst.get_version() == sstable_version_types::mc) {
        _impl = std::make_unique<sstable_writer_m>(sst, s, estimated_partitions, cfg, enc_stats, pc, shard);
    } else {
        _impl = std::make_unique<sstable_writer_k_l>(sst, s, estimated_partitions, cfg, pc, shard);
    }
}

void sstable_writer::consume_new_partition(const dht::decorated_key& dk) {
    return _impl->consume_new_partition(dk);
}

void sstable_writer::consume(tombstone t) {
    return _impl->consume(t);
}

stop_iteration sstable_writer::consume(static_row&& sr) {
    return _impl->consume(std::move(sr));
}

stop_iteration sstable_writer::consume(clustering_row&& cr) {
    return _impl->consume(std::move(cr));
}

stop_iteration sstable_writer::consume(range_tombstone&& rt) {
    return _impl->consume(std::move(rt));
}

stop_iteration sstable_writer::consume_end_of_partition() {
    return _impl->consume_end_of_partition();
}

void sstable_writer::consume_end_of_stream() {
    return _impl->consume_end_of_stream();
}

sstable_writer::sstable_writer(sstable_writer&& o) = default;
sstable_writer& sstable_writer::operator=(sstable_writer&& o) = default;
sstable_writer::~sstable_writer() = default;

future<> sstable::seal_sstable(bool backup)
{
    return seal_sstable().then([this, backup] {
        if (backup) {
            auto dir = get_dir() + "/backups/";
            return sstable_write_io_check(touch_directory, dir).then([this, dir] {
                return create_links(dir);
            });
        }
        return make_ready_future<>();
    });
}

sstable_writer sstable::get_writer(const schema& s, uint64_t estimated_partitions,
        const sstable_writer_config& cfg, encoding_stats enc_stats, const io_priority_class& pc, shard_id shard)
{
    return sstable_writer(*this, s, estimated_partitions, cfg, enc_stats, pc, shard);
}

future<> sstable::write_components(
        flat_mutation_reader mr,
        uint64_t estimated_partitions,
        schema_ptr schema,
        const sstable_writer_config& cfg,
        encoding_stats stats,
        const io_priority_class& pc) {
    if (cfg.replay_position) {
        _collector.set_replay_position(cfg.replay_position.value());
    }
    return seastar::async([this, mr = std::move(mr), estimated_partitions, schema = std::move(schema), cfg, stats, &pc] () mutable {
        auto wr = get_writer(*schema, estimated_partitions, cfg, stats, pc);
        mr.consume_in_thread(std::move(wr), db::no_timeout);
    });
}

future<> sstable::generate_summary(const io_priority_class& pc) {
    if (_components->summary) {
        return make_ready_future<>();
    }

    sstlog.info("Summary file {} not found. Generating Summary...", filename(component_type::Summary));
    class summary_generator {
        summary& _summary;
        index_sampling_state _state;
    public:
        std::experimental::optional<key> first_key, last_key;

        summary_generator(summary& s) : _summary(s) {
            _state.summary_byte_cost = summary_byte_cost();
        }
        bool should_continue() {
            return true;
        }
        void consume_entry(index_entry&& ie, uint64_t index_offset) {
            auto token = dht::global_partitioner().get_token(ie.get_key());
            components_writer::maybe_add_summary_entry(_summary, token, ie.get_key_bytes(), ie.position(), index_offset, _state);
            if (!first_key) {
                first_key = key(to_bytes(ie.get_key_bytes()));
            } else {
                last_key = key(to_bytes(ie.get_key_bytes()));
            }
        }
        const index_sampling_state& state() const {
            return _state;
        }
    };

    return open_checked_file_dma(_read_error_handler, filename(component_type::Index), open_flags::ro).then([this, &pc] (file index_file) {
        return do_with(std::move(index_file), [this, &pc] (file index_file) {
            return index_file.size().then([this, &pc, index_file] (auto index_size) {
                // an upper bound. Surely to be less than this.
                auto estimated_partitions = index_size / sizeof(uint64_t);
                prepare_summary(_components->summary, estimated_partitions, _schema->min_index_interval());

                file_input_stream_options options;
                options.buffer_size = sstable_buffer_size;
                options.io_priority_class = pc;
                return do_with(summary_generator(_components->summary),
                        [this, &pc, options = std::move(options), index_file, index_size] (summary_generator& s) mutable {
                    auto ctx = make_lw_shared<index_consume_entry_context<summary_generator>>(
                            s, trust_promoted_index::yes, *_schema, index_file, std::move(options), 0, index_size,
                            (_version == sstable_version_types::mc
                                ? std::make_optional(get_clustering_values_fixed_lengths(get_serialization_header()))
                                : std::optional<column_values_fixed_lengths>{}));
                    return ctx->consume_input().finally([ctx] {
                        return ctx->close();
                    }).then([this, ctx, &s] {
                        seal_summary(_components->summary, std::move(s.first_key), std::move(s.last_key), s.state());
                    });
                });
            }).then([index_file] () mutable {
                return index_file.close().handle_exception([] (auto ep) {
                    sstlog.warn("sstable close index_file failed: {}", ep);
                    general_disk_error();
                });
            });
        });
    });
}

uint64_t sstable::data_size() const {
    if (has_component(component_type::CompressionInfo)) {
        return _components->compression.uncompressed_file_length();
    }
    return _data_file_size;
}

uint64_t sstable::ondisk_data_size() const {
    return _data_file_size;
}

uint64_t sstable::bytes_on_disk() {
    assert(_bytes_on_disk > 0);
    return _bytes_on_disk;
}

const bool sstable::has_component(component_type f) const {
    return _recognized_components.count(f);
}

const sstring sstable::filename(component_type f) const {
    return filename(_dir, _schema->ks_name(), _schema->cf_name(), _version, _generation, _format, f);
}

std::vector<sstring> sstable::component_filenames() const {
    std::vector<sstring> res;
    for (auto c : sstable_version_constants::get_component_map(_version) | boost::adaptors::map_keys) {
        if (has_component(c)) {
            res.emplace_back(filename(c));
        }
    }
    return res;
}

sstring sstable::toc_filename() const {
    return filename(component_type::TOC);
}

bool sstable::is_staging() const {
    return boost::algorithm::ends_with(_dir, "staging");
}

const sstring sstable::filename(sstring dir, sstring ks, sstring cf, version_types version, int64_t generation,
                                format_types format, component_type component) {

    static std::unordered_map<version_types, std::function<sstring (entry_descriptor d)>, enum_hash<version_types>> strmap = {
        { sstable::version_types::ka, [] (entry_descriptor d) {
            return d.ks + "-" + d.cf + "-" + _version_string.at(d.version) + "-" + to_sstring(d.generation) + "-"
                   + sstable_version_constants::get_component_map(d.version).at(d.component); }
        },
        { sstable::version_types::la, [] (entry_descriptor d) {
            return _version_string.at(d.version) + "-" + to_sstring(d.generation) + "-" + _format_string.at(d.format) + "-"
                   + sstable_version_constants::get_component_map(d.version).at(d.component); }
        },
        { sstable::version_types::mc, [] (entry_descriptor d) {
                return _version_string.at(d.version) + "-" + to_sstring(d.generation) + "-" + _format_string.at(d.format) + "-"
                       + sstable_version_constants::get_component_map(d.version).at(d.component); }
        },
    };

    return dir + "/" + strmap[version](entry_descriptor(dir, ks, cf, version, generation, format, component));
}

const sstring sstable::filename(sstring dir, sstring ks, sstring cf, version_types version, int64_t generation,
                                format_types format, sstring component) {
    static std::unordered_map<version_types, const char*, enum_hash<version_types>> fmtmap = {
        { sstable::version_types::ka, "{0}-{1}-{2}-{3}-{5}" },
        { sstable::version_types::la, "{2}-{3}-{4}-{5}" },
        { sstable::version_types::mc, "{2}-{3}-{4}-{5}" }
    };

    return dir + "/" + seastar::format(fmtmap[version], ks, cf, _version_string.at(version), to_sstring(generation), _format_string.at(format), component);
}

std::vector<std::pair<component_type, sstring>> sstable::all_components() const {
    std::vector<std::pair<component_type, sstring>> all;
    all.reserve(_recognized_components.size() + _unrecognized_components.size());
    for (auto& c : _recognized_components) {
        all.push_back(std::make_pair(c, sstable_version_constants::get_component_map(_version).at(c)));
    }
    for (auto& c : _unrecognized_components) {
        all.push_back(std::make_pair(component_type::Unknown, c));
    }
    return all;
}

future<> sstable::create_links(sstring dir, int64_t generation) const {
    // TemporaryTOC is always first, TOC is always last
    auto dst = sstable::filename(dir, _schema->ks_name(), _schema->cf_name(), _version, generation, _format, component_type::TemporaryTOC);
    return sstable_write_io_check(::link_file, filename(component_type::TOC), dst).then([this, dir] {
        return sstable_write_io_check(sync_directory, dir);
    }).then([this, dir, generation] {
        // FIXME: Should clean already-created links if we failed midway.
        return parallel_for_each(all_components(), [this, dir, generation] (auto p) {
            if (p.first == component_type::TOC) {
                return make_ready_future<>();
            }
            auto src = sstable::filename(_dir, _schema->ks_name(), _schema->cf_name(), _version, _generation, _format, p.second);
            auto dst = sstable::filename(dir, _schema->ks_name(), _schema->cf_name(), _version, generation, _format, p.second);
            return this->sstable_write_io_check(::link_file, std::move(src), std::move(dst));
        });
    }).then([this, dir] {
        return sstable_write_io_check(sync_directory, dir);
    }).then([dir, this, generation] {
        auto src = sstable::filename(dir, _schema->ks_name(), _schema->cf_name(), _version, generation, _format, component_type::TemporaryTOC);
        auto dst = sstable::filename(dir, _schema->ks_name(), _schema->cf_name(), _version, generation, _format, component_type::TOC);
        return sstable_write_io_check([&] {
            return engine().rename_file(src, dst);
        });
    }).then([this, dir] {
        return sstable_write_io_check(sync_directory, dir);
    });
}

future<> sstable::set_generation(int64_t new_generation) {
    return create_links(_dir, new_generation).then([this] {
        return remove_file(filename(component_type::TOC)).then([this] {
            return sstable_write_io_check(sync_directory, _dir);
        }).then([this] {
            return parallel_for_each(all_components(), [this] (auto p) {
                if (p.first == component_type::TOC) {
                    return make_ready_future<>();
                }
                return remove_file(sstable::filename(_dir, _schema->ks_name(), _schema->cf_name(), _version, _generation, _format, p.second));
            });
        });
    }).then([this, new_generation] {
        return sync_directory(_dir).then([this, new_generation] {
            _generation = new_generation;
        });
    });
}

void sstable::move_to_new_dir_in_thread(sstring new_dir, int64_t new_generation) {
    create_links(new_dir, new_generation).get();
    remove_file(filename(component_type::TOC)).get();
    sstable_write_io_check(sync_directory, _dir).get();
    sstring old_dir = std::exchange(_dir, std::move(new_dir));
    int64_t old_generation = std::exchange(_generation, new_generation);
    parallel_for_each(all_components(), [this, old_generation, old_dir] (auto p) {
        if (p.first == component_type::TOC) {
            return make_ready_future<>();
        }
        return remove_file(sstable::filename(old_dir, _schema->ks_name(), _schema->cf_name(), _version, old_generation, _format, p.second));
    }).get();
    sync_directory(_dir).get();
    sync_directory(old_dir).get();
}

entry_descriptor entry_descriptor::make_descriptor(sstring sstdir, sstring fname) {
    static std::regex la_mc("(la|mc)-(\\d+)-(\\w+)-(.*)");
    static std::regex ka("(\\w+)-(\\w+)-ka-(\\d+)-(.*)");

    static std::regex dir(".*/([^/]*)/(\\w+)-[\\da-fA-F]+(?:/staging|/upload|/snapshots/[^/]+)?/?");

    std::smatch match;

    sstable::version_types version;

    sstring generation;
    sstring format;
    sstring component;
    sstring ks;
    sstring cf;

    sstlog.debug("Make descriptor sstdir: {}; fname: {}", sstdir, fname);
    std::string s(fname);
    if (std::regex_match(s, match, la_mc)) {
        std::string sdir(sstdir);
        std::smatch dirmatch;
        if (std::regex_match(sdir, dirmatch, dir)) {
            ks = dirmatch[1].str();
            cf = dirmatch[2].str();
        } else {
            throw malformed_sstable_exception(seastar::sprint("invalid version for file %s with path %s. Path doesn't match known pattern.", fname, sstdir));
        }
        version = (match[1].str() == "la") ? sstable::version_types::la : sstable::version_types::mc;
        generation = match[2].str();
        format = sstring(match[3].str());
        component = sstring(match[4].str());
    } else if (std::regex_match(s, match, ka)) {
        ks = match[1].str();
        cf = match[2].str();
        version = sstable::version_types::ka;
        format = sstring("big");
        generation = match[3].str();
        component = sstring(match[4].str());
    } else {
        throw malformed_sstable_exception(seastar::sprint("invalid version for file %s. Name doesn't match any known version.", fname));
    }
    return entry_descriptor(sstdir, ks, cf, version, boost::lexical_cast<unsigned long>(generation), sstable::format_from_sstring(format), sstable::component_from_sstring(version, component));
}

sstable::version_types sstable::version_from_sstring(sstring &s) {
    try {
        return reverse_map(s, _version_string);
    } catch (std::out_of_range&) {
        throw std::out_of_range(seastar::sprint("Unknown sstable version: %s", s.c_str()));
    }
}

sstable::format_types sstable::format_from_sstring(sstring &s) {
    try {
        return reverse_map(s, _format_string);
    } catch (std::out_of_range&) {
        throw std::out_of_range(seastar::sprint("Unknown sstable format: %s", s.c_str()));
    }
}

component_type sstable::component_from_sstring(version_types v, sstring &s) {
    try {
        return reverse_map(s, sstable_version_constants::get_component_map(v));
    } catch (std::out_of_range&) {
        return component_type::Unknown;
    }
}

input_stream<char> sstable::data_stream(uint64_t pos, size_t len, const io_priority_class& pc, reader_resource_tracker resource_tracker, lw_shared_ptr<file_input_stream_history> history) {
    file_input_stream_options options;
    options.buffer_size = sstable_buffer_size;
    options.io_priority_class = pc;
    options.read_ahead = 4;
    options.dynamic_adjustments = std::move(history);

    auto f = resource_tracker.track(_data_file);

    input_stream<char> stream;
    if (_components->compression) {
        if (_version == sstable_version_types::mc) {
             return make_compressed_file_m_format_input_stream(f, &_components->compression,
                pos, len, std::move(options));
        } else {
            return make_compressed_file_k_l_format_input_stream(f, &_components->compression,
                pos, len, std::move(options));
        }
    }

    return make_file_input_stream(f, pos, len, std::move(options));
}

future<temporary_buffer<char>> sstable::data_read(uint64_t pos, size_t len, const io_priority_class& pc) {
    return do_with(data_stream(pos, len, pc, no_resource_tracking(), {}), [len] (auto& stream) {
        return stream.read_exactly(len).finally([&stream] {
            return stream.close();
        });
    });
}

void sstable::set_first_and_last_keys() {
    if (_first && _last) {
        return;
    }
    auto decorate_key = [this] (const char *m, const bytes& value) {
        if (value.empty()) {
            throw std::runtime_error(sprint("%s key of summary of %s is empty", m, get_filename()));
        }
        auto pk = key::from_bytes(value).to_partition_key(*_schema);
        return dht::global_partitioner().decorate_key(*_schema, std::move(pk));
    };
    _first = decorate_key("first", _components->summary.first_key.value);
    _last = decorate_key("last", _components->summary.last_key.value);
}

const partition_key& sstable::get_first_partition_key() const {
    return get_first_decorated_key().key();
 }

const partition_key& sstable::get_last_partition_key() const {
    return get_last_decorated_key().key();
}

const dht::decorated_key& sstable::get_first_decorated_key() const {
    if (!_first) {
        throw std::runtime_error(sprint("first key of %s wasn't set", get_filename()));
    }
    return *_first;
}

const dht::decorated_key& sstable::get_last_decorated_key() const {
    if (!_last) {
        throw std::runtime_error(sprint("last key of %s wasn't set", get_filename()));
    }
    return *_last;
}

int sstable::compare_by_first_key(const sstable& other) const {
    return get_first_decorated_key().tri_compare(*_schema, other.get_first_decorated_key());
}

double sstable::get_compression_ratio() const {
    if (this->has_component(component_type::CompressionInfo)) {
        return double(_components->compression.compressed_file_length()) / _components->compression.uncompressed_file_length();
    } else {
        return metadata_collector::NO_COMPRESSION_RATIO;
    }
}

void sstable::set_sstable_level(uint32_t new_level) {
    auto entry = _components->statistics.contents.find(metadata_type::Stats);
    if (entry == _components->statistics.contents.end()) {
        return;
    }
    auto& p = entry->second;
    if (!p) {
        throw std::runtime_error("Statistics is malformed");
    }
    stats_metadata& s = *static_cast<stats_metadata *>(p.get());
    sstlog.debug("set level of {} with generation {} from {} to {}", get_filename(), _generation, s.sstable_level, new_level);
    s.sstable_level = new_level;
}

future<> sstable::mutate_sstable_level(uint32_t new_level) {
    if (!has_component(component_type::Statistics)) {
        return make_ready_future<>();
    }

    auto entry = _components->statistics.contents.find(metadata_type::Stats);
    if (entry == _components->statistics.contents.end()) {
        return make_ready_future<>();
    }

    auto& p = entry->second;
    if (!p) {
        throw std::runtime_error("Statistics is malformed");
    }
    stats_metadata& s = *static_cast<stats_metadata *>(p.get());
    if (s.sstable_level == new_level) {
        return make_ready_future<>();
    }

    s.sstable_level = new_level;
    // Technically we don't have to write the whole file again. But the assumption that
    // we will always write sequentially is a powerful one, and this does not merit an
    // exception.
    return seastar::async([this] {
        // This is not part of the standard memtable flush path, but there is no reason
        // to come up with a class just for that. It is used by the snapshot/restore mechanism
        // which comprises mostly hard link creation and this operation at the end + this operation,
        // and also (eventually) by some compaction strategy. In any of the cases, it won't be high
        // priority enough so we will use the default priority
        rewrite_statistics(default_priority_class());
    });
}

int sstable::compare_by_max_timestamp(const sstable& other) const {
    auto ts1 = get_stats_metadata().max_timestamp;
    auto ts2 = other.get_stats_metadata().max_timestamp;
    return (ts1 > ts2 ? 1 : (ts1 == ts2 ? 0 : -1));
}

future<>
delete_sstables(std::vector<sstring> tocs);

sstable::~sstable() {
    if (_index_file) {
        _index_file.close().handle_exception([save = _index_file, op = background_jobs().start()] (auto ep) {
            sstlog.warn("sstable close index_file failed: {}", ep);
            general_disk_error();
        });
    }
    if (_data_file) {
        _data_file.close().handle_exception([save = _data_file, op = background_jobs().start()] (auto ep) {
            sstlog.warn("sstable close data_file failed: {}", ep);
            general_disk_error();
        });
    }

    if (_marked_for_deletion) {
        // We need to delete the on-disk files for this table. Since this is a
        // destructor, we can't wait for this to finish, or return any errors,
        // but just need to do our best. If a deletion fails for some reason we
        // log and ignore this failure, because on startup we'll again try to
        // clean up unused sstables, and because we'll never reuse the same
        // generation number anyway.
        try {
            delete_sstables({filename(component_type::TOC)}).handle_exception(
                        [op = background_jobs().start()] (std::exception_ptr eptr) {
                            try {
                                std::rethrow_exception(eptr);
                            } catch (...) {
                                sstlog.warn("Exception when deleting sstable file: {}", eptr);
                            }
                        });
        } catch (...) {
            sstlog.warn("Exception when deleting sstable file: {}", std::current_exception());
        }

    }
}

sstring
dirname(sstring fname) {
    return boost::filesystem::canonical(std::string(fname)).parent_path().string();
}

future<>
fsync_directory(const io_error_handler& error_handler, sstring fname) {
    return ::sstable_io_check(error_handler, [&] {
        return open_checked_directory(error_handler, dirname(fname)).then([] (file f) {
            return do_with(std::move(f), [] (file& f) {
                return f.flush().then([&f] {
                    return f.close();
                });
            });
        });
    });
}

future<>
remove_by_toc_name(sstring sstable_toc_name, const io_error_handler& error_handler) {
    return seastar::async([sstable_toc_name, &error_handler] () mutable {
        sstring prefix = sstable_toc_name.substr(0, sstable_toc_name.size() - sstable_version_constants::TOC_SUFFIX.size());
        auto new_toc_name = prefix + sstable_version_constants::TEMPORARY_TOC_SUFFIX;
        sstring dir;

        if (sstable_io_check(error_handler, file_exists, sstable_toc_name).get0()) {
            dir = dirname(sstable_toc_name);
            sstable_io_check(error_handler, rename_file, sstable_toc_name, new_toc_name).get();
            fsync_directory(error_handler, dir).get();
        } else if (sstable_io_check(error_handler, file_exists, new_toc_name).get0()) {
            dir = dirname(new_toc_name);
        } else {
            sstlog.warn("Unable to delete {} because it doesn't exist.", sstable_toc_name);
            return;
        }

        auto toc_file = open_checked_file_dma(error_handler, new_toc_name, open_flags::ro).get0();
        auto in = make_file_input_stream(toc_file);
        auto size = toc_file.size().get0();
        auto text = in.read_exactly(size).get0();
        in.close().get();
        std::vector<sstring> components;
        sstring all(text.begin(), text.end());
        boost::split(components, all, boost::is_any_of("\n"));
        parallel_for_each(components, [prefix, &error_handler] (sstring component) mutable {
            if (component.empty()) {
                // eof
                return make_ready_future<>();
            }
            if (component == sstable_version_constants::TOC_SUFFIX) {
                // already deleted
                return make_ready_future<>();
            }
            auto fname = prefix + component;
            return sstable_io_check(error_handler, remove_file, prefix + component).then_wrapped([fname = std::move(fname)] (future<> f) {
                // forgive ENOENT, since the component may not have been written;
                try {
                    f.get();
                } catch (std::system_error& e) {
                    if (!is_system_error_errno(ENOENT)) {
                        throw;
                    }
                    sstlog.debug("Forgiving ENOENT when deleting file {}", fname);
                }
                return make_ready_future<>();
            });
        }).get();
        fsync_directory(error_handler, dir).get();
        sstable_io_check(error_handler, remove_file, new_toc_name).get();
    });
}

future<>
sstable::remove_sstable_with_temp_toc(sstring ks, sstring cf, sstring dir, int64_t generation, version_types v, format_types f) {
    return seastar::async([ks, cf, dir, generation, v, f] {
        const io_error_handler& error_handler = sstable_write_error_handler;
        auto toc = sstable_io_check(error_handler, file_exists, filename(dir, ks, cf, v, generation, f, component_type::TOC)).get0();

        sstlog.warn("Deleting components of sstable from {}.{} of generation {} that has a temporary TOC", ks, cf, generation);

        // assert that toc doesn't exist for sstable with temporary toc.
        assert(toc == false);

        auto tmptoc = sstable_io_check(error_handler, file_exists, filename(dir, ks, cf, v, generation, f, component_type::TemporaryTOC)).get0();
        // assert that temporary toc exists for this sstable.
        assert(tmptoc == true);

        for (auto& entry : sstable_version_constants::get_component_map(v)) {
            // Skipping TemporaryTOC because it must be the last component to
            // be deleted, and unordered map doesn't guarantee ordering.
            // This is needed because we may end up with a partial delete in
            // event of a power failure.
            // If TemporaryTOC is deleted prematurely and scylla crashes,
            // the subsequent boot would fail because of that generation
            // missing a TOC.
            if (entry.first == component_type::TemporaryTOC) {
                continue;
            }

            auto file_path = filename(dir, ks, cf, v, generation, f, entry.first);
            // Skip component that doesn't exist.
            auto exists = sstable_io_check(error_handler, file_exists, file_path).get0();
            if (!exists) {
                continue;
            }
            sstable_io_check(error_handler, remove_file, file_path).get();
        }
        fsync_directory(error_handler, dir).get();
        // Removing temporary
        sstable_io_check(error_handler, remove_file, filename(dir, ks, cf, v, generation, f, component_type::TemporaryTOC)).get();
        // Fsync'ing column family dir to guarantee that deletion completed.
        fsync_directory(error_handler, dir).get();
    });
}

future<range<partition_key>>
sstable::get_sstable_key_range(const schema& s) {
    auto fut = read_summary(default_priority_class());
    return std::move(fut).then([this, &s] () mutable {
        this->set_first_and_last_keys();
        return make_ready_future<range<partition_key>>(range<partition_key>::make(get_first_partition_key(), get_last_partition_key()));
    });
}

/**
 * Returns a pair of positions [p1, p2) in the summary file corresponding to entries
 * covered by the specified range, or a disengaged optional if no such pair exists.
 */
stdx::optional<std::pair<uint64_t, uint64_t>> sstable::get_sample_indexes_for_range(const dht::token_range& range) {
    auto entries_size = _components->summary.entries.size();
    auto search = [this](bool before, const dht::token& token) {
        auto kind = before ? key::kind::before_all_keys : key::kind::after_all_keys;
        key k(kind);
        // Binary search will never returns positive values.
        return uint64_t((binary_search(_components->summary.entries, k, token) + 1) * -1);
    };
    uint64_t left = 0;
    if (range.start()) {
        left = search(range.start()->is_inclusive(), range.start()->value());
        if (left == entries_size) {
            // left is past the end of the sampling.
            return stdx::nullopt;
        }
    }
    uint64_t right = entries_size;
    if (range.end()) {
        right = search(!range.end()->is_inclusive(), range.end()->value());
        if (right == 0) {
            // The first key is strictly greater than right.
            return stdx::nullopt;
        }
    }
    if (left < right) {
        return stdx::optional<std::pair<uint64_t, uint64_t>>(stdx::in_place_t(), left, right);
    }
    return stdx::nullopt;
}

std::vector<dht::decorated_key> sstable::get_key_samples(const schema& s, const dht::token_range& range) {
    auto index_range = get_sample_indexes_for_range(range);
    std::vector<dht::decorated_key> res;
    if (index_range) {
        for (auto idx = index_range->first; idx < index_range->second; ++idx) {
            auto pkey = _components->summary.entries[idx].get_key().to_partition_key(s);
            res.push_back(dht::global_partitioner().decorate_key(s, std::move(pkey)));
        }
    }
    return res;
}

uint64_t sstable::estimated_keys_for_range(const dht::token_range& range) {
    auto sample_index_range = get_sample_indexes_for_range(range);
    uint64_t sample_key_count = sample_index_range ? sample_index_range->second - sample_index_range->first : 0;
    // adjust for the current sampling level
    uint64_t estimated_keys = sample_key_count * ((downsampling::BASE_SAMPLING_LEVEL * _components->summary.header.min_index_interval) / _components->summary.header.sampling_level);
    return std::max(uint64_t(1), estimated_keys);
}

std::vector<unsigned>
sstable::compute_shards_for_this_sstable() const {
    std::unordered_set<unsigned> shards;
    dht::partition_range_vector token_ranges;
    const auto* sm = _components->scylla_metadata
            ? _components->scylla_metadata->data.get<scylla_metadata_type::Sharding, sharding_metadata>()
            : nullptr;
    if (!sm || sm->token_ranges.elements.empty()) {
        token_ranges.push_back(dht::partition_range::make(
                dht::ring_position::starting_at(get_first_decorated_key().token()),
                dht::ring_position::ending_at(get_last_decorated_key().token())));
    } else {
        auto disk_token_range_to_ring_position_range = [] (const disk_token_range& dtr) {
            auto t1 = dht::token(dht::token::kind::key, managed_bytes(bytes_view(dtr.left.token)));
            auto t2 = dht::token(dht::token::kind::key, managed_bytes(bytes_view(dtr.right.token)));
            return dht::partition_range::make(
                    (dtr.left.exclusive ? dht::ring_position::ending_at : dht::ring_position::starting_at)(std::move(t1)),
                    (dtr.right.exclusive ? dht::ring_position::starting_at : dht::ring_position::ending_at)(std::move(t2)));
        };
        token_ranges = boost::copy_range<dht::partition_range_vector>(
                sm->token_ranges.elements
                | boost::adaptors::transformed(disk_token_range_to_ring_position_range));
    }
    auto sharder = dht::ring_position_range_vector_sharder(std::move(token_ranges));
    auto rpras = sharder.next(*_schema);
    while (rpras) {
        shards.insert(rpras->shard);
        rpras = sharder.next(*_schema);
    }
    return boost::copy_range<std::vector<unsigned>>(shards);
}

future<bool> sstable::has_partition_key(const utils::hashed_key& hk, const dht::decorated_key& dk) {
    shared_sstable s = shared_from_this();
    if (!filter_has_key(hk)) {
        return make_ready_future<bool>(false);
    }
    seastar::shared_ptr<sstables::index_reader> lh_index = seastar::make_shared<sstables::index_reader>(s, default_priority_class());
    return lh_index->advance_lower_and_check_if_present(dk).then([lh_index, s, this] (bool present) {
        return make_ready_future<bool>(present);
    });
}

utils::hashed_key sstable::make_hashed_key(const schema& s, const partition_key& key) {
    return utils::make_hashed_key(static_cast<bytes_view>(key::from_partition_key(s, key)));
}

future<>
delete_sstables(std::vector<sstring> tocs) {
    // FIXME: this needs to be done atomically (using a log file of sstables we intend to delete)
    return parallel_for_each(tocs, [] (sstring name) {
        return remove_by_toc_name(name);
    });
}

future<>
delete_atomically(std::vector<shared_sstable> ssts, const db::large_partition_handler& large_partition_handler) {
    // Asynchronously issue delete operations for large partitions, do not handle their outcome.
    // If any of the operations fail, large_partition_handler should be responsible for logging or otherwise handling it.
    for (const auto& sst : ssts) {
        large_partition_handler.maybe_delete_large_partitions_entry(*sst);
    }
    auto sstables_to_delete_atomically = boost::copy_range<std::vector<sstring>>(ssts
            | boost::adaptors::transformed([] (auto&& sst) { return sst->toc_filename(); }));

    return delete_sstables(std::move(sstables_to_delete_atomically));
}

thread_local shared_index_lists::stats shared_index_lists::_shard_stats;
static thread_local seastar::metrics::metric_groups metrics;

future<> init_metrics() {
  return seastar::smp::invoke_on_all([] {
    namespace sm = seastar::metrics;
    metrics.add_group("sstables", {
        sm::make_derive("index_page_hits", [] { return shared_index_lists::shard_stats().hits; },
            sm::description("Index page requests which could be satisfied without waiting")),
        sm::make_derive("index_page_misses", [] { return shared_index_lists::shard_stats().misses; },
            sm::description("Index page requests which initiated a read from disk")),
        sm::make_derive("index_page_blocks", [] { return shared_index_lists::shard_stats().blocks; },
            sm::description("Index page requests which needed to wait due to page not being loaded yet")),
    });
  });
}

mutation_source sstable::as_mutation_source() {
    return mutation_source([sst = shared_from_this()] (schema_ptr s,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_ptr,
            streamed_mutation::forwarding fwd,
            mutation_reader::forwarding fwd_mr) mutable {
        // CAVEAT: if as_mutation_source() is called on a single partition
        // we want to optimize and read exactly this partition. As a
        // consequence, fast_forward_to() will *NOT* work on the result,
        // regardless of what the fwd_mr parameter says.
        if (range.is_singular() && range.start()->value().has_key()) {
            return sst->read_row_flat(s, range.start()->value(), slice, pc, no_resource_tracking(), fwd);
        } else {
            return sst->read_range_rows_flat(s, range, slice, pc, no_resource_tracking(), fwd, fwd_mr);
        }
    });
}

bool supports_correct_non_compound_range_tombstones() {
    return service::get_local_storage_service().cluster_supports_reading_correctly_serialized_range_tombstones();
}

}

std::ostream& operator<<(std::ostream& out, const sstables::component_type& comp_type) {
    using ct = sstables::component_type;
    switch (comp_type) {
    case ct::Index: out << "Index"; break;
    case ct::CompressionInfo: out << "CompressionInfo"; break;
    case ct::Data: out << "Data"; break;
    case ct::TOC: out << "TOC"; break;
    case ct::Summary: out << "Summary"; break;
    case ct::Digest: out << "Digest"; break;
    case ct::CRC: out << "CRC"; break;
    case ct::Filter: out << "Filter"; break;
    case ct::Statistics: out << "Statistics"; break;
    case ct::TemporaryTOC: out << "TemporaryTOC"; break;
    case ct::TemporaryStatistics: out << "TemporaryStatistics"; break;
    case ct::Scylla: out << "Scylla"; break;
    case ct::Unknown: out << "Unknown"; break;
    }
    return out;
}

namespace seastar {

void
lw_shared_ptr_deleter<sstables::sstable>::dispose(sstables::sstable* s) {
    delete s;
}

}
