/*
 * Copyright (C) 2018 ScyllaDB
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

#include <set>
#include <fstream>
#include <iterator>

#include <boost/test/unit_test.hpp>

#include <seastar/core/thread.hh>
#include <seastar/tests/test-utils.hh>

#include "sstables/sstables.hh"
#include "sstables/compaction_manager.hh"
#include "cell_locking.hh"
#include "compress.hh"
#include "counters.hh"
#include "schema_builder.hh"
#include "sstable_test.hh"
#include "flat_mutation_reader_assertions.hh"
#include "sstable_test.hh"
#include "tests/test_services.hh"
#include "tests/tmpdir.hh"
#include "tests/sstable_utils.hh"
#include "tests/index_reader_assertions.hh"
#include "sstables/types.hh"
#include "keys.hh"
#include "types.hh"
#include "partition_slice_builder.hh"
#include "schema.hh"
#include "utils/UUID_gen.hh"

using namespace sstables;

class sstable_assertions final {
    shared_sstable _sst;
public:
    sstable_assertions(schema_ptr schema, const sstring& path, int generation = 1)
        : _sst(make_sstable(std::move(schema),
                            path,
                            generation,
                            sstable_version_types::mc,
                            sstable_format_types::big,
                            gc_clock::now(),
                            default_io_error_handler_gen(),
                            1))
    { }
    void read_toc() {
        _sst->read_toc().get();
    }
    void read_summary() {
        _sst->read_summary(default_priority_class()).get();
    }
    void read_filter() {
        _sst->read_filter(default_priority_class()).get();
    }
    void read_statistics() {
        _sst->read_statistics(default_priority_class()).get();
    }
    void load() {
        _sst->load().get();
    }
    future<index_list> read_index() {
        load();
        return sstables::test(_sst).read_indexes();
    }
    flat_mutation_reader read_rows_flat() {
        return _sst->read_rows_flat(_sst->_schema);
    }

    flat_mutation_reader read_range_rows_flat(
            const dht::partition_range& range,
            const query::partition_slice& slice,
            const io_priority_class& pc = default_priority_class(),
            reader_resource_tracker resource_tracker = no_resource_tracking(),
            streamed_mutation::forwarding fwd = streamed_mutation::forwarding::no,
            mutation_reader::forwarding fwd_mr = mutation_reader::forwarding::yes,
            read_monitor& monitor = default_read_monitor()) {
        return _sst->read_range_rows_flat(_sst->_schema,
                                          range,
                                          slice,
                                          pc,
                                          std::move(resource_tracker),
                                          fwd,
                                          fwd_mr,
                                          monitor);
    }
    void assert_toc(const std::set<component_type>& expected_components) {
        for (auto& expected : expected_components) {
            if(_sst->_recognized_components.count(expected) == 0) {
                BOOST_FAIL(sprint("Expected component of TOC missing: %s\n ... in: %s",
                                  expected,
                                  std::set<component_type>(
                                      cbegin(_sst->_recognized_components),
                                      cend(_sst->_recognized_components))));
            }
        }
        for (auto& present : _sst->_recognized_components) {
            if (expected_components.count(present) == 0) {
                BOOST_FAIL(sprint("Unexpected component of TOC: %s\n ... when expecting: %s",
                                  present,
                                  expected_components));
            }
        }
    }
};

// Following tests run on files in tests/sstables/3.x/uncompressed/filtering_and_forwarding
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, ck INT, s INT STATIC, val INT, PRIMARY KEY(pk, ck))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, s) VALUES(1, 1);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 101, 1001);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 102, 1002);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 103, 1003);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 104, 1004);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 105, 1005);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 106, 1006);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 107, 1007);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 108, 1008);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 109, 1009);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 110, 1010);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 101, 1001);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 102, 1002);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 103, 1003);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 104, 1004);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 105, 1005);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 106, 1006);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 107, 1007);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 108, 1008);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 109, 1009);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 110, 1010);

static thread_local const sstring UNCOMPRESSED_FILTERING_AND_FORWARDING_PATH =
    "tests/sstables/3.x/uncompressed/filtering_and_forwarding";
static thread_local const schema_ptr UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("s", int32_type, column_kind::static_column)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_filtering_and_forwarding_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA,
                           UNCOMPRESSED_FILTERING_AND_FORWARDING_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA, pk);
    };

    auto to_ck = [] (int ck) {
        return clustering_key::from_single_value(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA,
                                                 int32_type->decompose(ck));
    };

    auto s_cdef = UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA->get_column_definition(to_bytes("s"));
    BOOST_REQUIRE(s_cdef);
    auto val_cdef = UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(val_cdef);

    auto to_expected = [val_cdef] (int val) {
        return std::vector<flat_reader_assertions::expected_column>{{val_cdef, int32_type->decompose(int32_t(val))}};
    };

    // Sequential read
    {
        assert_that(sst.read_rows_flat())
            .produces_partition_start(to_key(1))
            .produces_static_row({{s_cdef, int32_type->decompose(int32_t(1))}})
            .produces_row(to_ck(101), to_expected(1001))
            .produces_row(to_ck(102), to_expected(1002))
            .produces_row(to_ck(103), to_expected(1003))
            .produces_row(to_ck(104), to_expected(1004))
            .produces_row(to_ck(105), to_expected(1005))
            .produces_row(to_ck(106), to_expected(1006))
            .produces_row(to_ck(107), to_expected(1007))
            .produces_row(to_ck(108), to_expected(1008))
            .produces_row(to_ck(109), to_expected(1009))
            .produces_row(to_ck(110), to_expected(1010))
            .produces_partition_end()
            .produces_partition_start(to_key(2))
            .produces_row(to_ck(101), to_expected(1001))
            .produces_row(to_ck(102), to_expected(1002))
            .produces_row(to_ck(103), to_expected(1003))
            .produces_row(to_ck(104), to_expected(1004))
            .produces_row(to_ck(105), to_expected(1005))
            .produces_row(to_ck(106), to_expected(1006))
            .produces_row(to_ck(107), to_expected(1007))
            .produces_row(to_ck(108), to_expected(1008))
            .produces_row(to_ck(109), to_expected(1009))
            .produces_row(to_ck(110), to_expected(1010))
            .produces_partition_end()
            .produces_end_of_stream();
    }

    // filtering read
    {
        auto slice =  partition_slice_builder(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA)
            .with_range(query::clustering_range::make({to_ck(102), true}, {to_ck(104), false}))
            .with_range(query::clustering_range::make({to_ck(106), false}, {to_ck(108), true}))
            .build();
        assert_that(sst.read_range_rows_flat(query::full_partition_range, slice))
            .produces_partition_start(to_key(1))
            .produces_static_row({{s_cdef, int32_type->decompose(int32_t(1))}})
            .produces_row(to_ck(102), to_expected(1002))
            .produces_row(to_ck(103), to_expected(1003))
            .produces_row(to_ck(107), to_expected(1007))
            .produces_row(to_ck(108), to_expected(1008))
            .produces_partition_end()
            .produces_partition_start(to_key(2))
            .produces_row(to_ck(102), to_expected(1002))
            .produces_row(to_ck(103), to_expected(1003))
            .produces_row(to_ck(107), to_expected(1007))
            .produces_row(to_ck(108), to_expected(1008))
            .produces_partition_end()
            .produces_end_of_stream();
    }

    // forwarding read
    {
        auto r = assert_that(sst.read_range_rows_flat(query::full_partition_range,
                                                      UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA->full_slice(),
                                                      default_priority_class(),
                                                      no_resource_tracking(),
                                                      streamed_mutation::forwarding::yes));
        r.produces_partition_start(to_key(1))
            .produces_static_row({{s_cdef, int32_type->decompose(int32_t(1))}})
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(102), to_ck(105));

        r.produces_row(to_ck(102), to_expected(1002))
            .produces_row(to_ck(103), to_expected(1003))
            .produces_row(to_ck(104), to_expected(1004))
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(107), to_ck(109));

        r.produces_row(to_ck(107), to_expected(1007))
            .produces_row(to_ck(108), to_expected(1008))
            .produces_end_of_stream();

        r.next_partition();

        r.produces_partition_start(to_key(2))
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(103), to_ck(104));

        r.produces_row(to_ck(103), to_expected(1003))
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(106), to_ck(111));

        r.produces_row(to_ck(106), to_expected(1006))
            .produces_row(to_ck(107), to_expected(1007))
            .produces_row(to_ck(108), to_expected(1008))
            .produces_row(to_ck(109), to_expected(1009))
            .produces_row(to_ck(110), to_expected(1010))
            .produces_end_of_stream();
    }

    // filtering and forwarding read
    {
        auto slice =  partition_slice_builder(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA)
            .with_range(query::clustering_range::make({to_ck(102), true}, {to_ck(103), true}))
            .with_range(query::clustering_range::make({to_ck(109), true}, {to_ck(110), true}))
            .build();
        auto r = assert_that(sst.read_range_rows_flat(query::full_partition_range,
                                                      slice,
                                                      default_priority_class(),
                                                      no_resource_tracking(),
                                                      streamed_mutation::forwarding::yes));

        r.produces_partition_start(to_key(1))
            .produces_static_row({{s_cdef, int32_type->decompose(int32_t(1))}})
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(102), to_ck(105));

        r.produces_row(to_ck(102), to_expected(1002))
            .produces_row(to_ck(103), to_expected(1003))
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(107), to_ck(109));

        r.produces_end_of_stream();

        r.next_partition();

        r.produces_partition_start(to_key(2))
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(103), to_ck(104));

        r.produces_row(to_ck(103), to_expected(1003))
            .produces_end_of_stream();
        r.fast_forward_to(to_ck(106), to_ck(111));

        r.produces_row(to_ck(109), to_expected(1009))
            .produces_row(to_ck(110), to_expected(1010))
            .produces_end_of_stream();
    }
}

/*
 * The test covering support for skipping through wide partitions using index.
 * Test files are generated using the following script <excerpt>:
 *
     session.execute("""
        CREATE TABLE IF NOT EXISTS skip_index_test (
            pk int,
            ck int,
            st int static,
            rc text,
            PRIMARY KEY (pk, ck)
        )
        WITH compression = { 'sstable_compression' : '' }
        AND caching = {'keys': 'NONE', 'rows_per_partition': 'NONE'}
        """)

    query_static = SimpleStatement("""
        INSERT INTO skip_index_test (pk, st)
        VALUES (%s, %s) USING TIMESTAMP 1525385507816568
	""", consistency_level=ConsistencyLevel.ONE)

    query = SimpleStatement("""
        INSERT INTO skip_index_test (pk, ck, rc)
        VALUES (%s, %s, %s) USING TIMESTAMP 1525385507816568
        """, consistency_level=ConsistencyLevel.ONE)

    session.execute(query_static, [1, 777])

    for i in range(1024):
        log.info("inserting row %d" % i)
        session.execute(query, [1, i, "%s%d" %('b' * 1024, i)])

    query_static2 = SimpleStatement("""
        INSERT INTO skip_index_test (pk, st)
        VALUES (%s, %s) USING TIMESTAMP 1525385507816578
	""", consistency_level=ConsistencyLevel.ONE)

    query2 = SimpleStatement("""
        INSERT INTO skip_index_test (pk, ck, rc)
        VALUES (%s, %s, %s) USING TIMESTAMP 1525385507816578
        """, consistency_level=ConsistencyLevel.ONE)

    session.execute(query_static2, [2, 999])

    for i in range(1024):
        log.info("inserting row %d" % i)
        session.execute(query2, [2, i, "%s%d" %('b' * 1024, i)])

    The index file contains promoted indices for two partitions, each consisting
    of 17 blocks with the following clustering key bounds:
    0 - 63
    64 - 126
    127 - 189
    190 - 252
    253 - 315
    316 - 378
    379 - 441
    442 - 504
    505 - 567
    568 - 630
    631 - 693
    694 - 756
    757 - 819
    820 - 882
    883 - 945
    946 - 1008
    1009 - 1023
 *
 */

static thread_local const sstring UNCOMPRESSED_SKIP_USING_INDEX_ROWS_PATH =
    "tests/sstables/3.x/uncompressed/skip_using_index_rows";
static thread_local const schema_ptr UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("st", int32_type, column_kind::static_column)
        .with_column("rc", utf8_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_skip_using_index_rows) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA,
                           UNCOMPRESSED_SKIP_USING_INDEX_ROWS_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA, pk);
    };

    auto to_ck = [] (int ck) {
        return clustering_key::from_single_value(*UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA,
                                                 int32_type->decompose(ck));
    };

    auto st_cdef = UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA->get_column_definition(to_bytes("st"));
    BOOST_REQUIRE(st_cdef);
    auto rc_cdef = UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA->get_column_definition(to_bytes("rc"));
    BOOST_REQUIRE(rc_cdef);

    auto to_expected = [rc_cdef] (sstring val) {
        return std::vector<flat_reader_assertions::expected_column>{{rc_cdef, utf8_type->decompose(val)}};
    };
    sstring rc_base(1024, 'b');

    auto make_reads_tracker = [] {
        reactor& r = *local_engine;
        return [&r, io_snapshot = r.get_io_stats()] {
            return r.get_io_stats().aio_reads - io_snapshot.aio_reads;
        };
    };
    uint64_t max_reads = 0;
    // Sequential read
    {
        auto aio_reads_tracker = make_reads_tracker();
        auto rd = sst.read_rows_flat();
        rd.set_max_buffer_size(1);
        auto r = assert_that(std::move(rd));
        r.produces_partition_start(to_key(1))
            .produces_static_row({{st_cdef, int32_type->decompose(int32_t(777))}});

        for (auto idx: boost::irange(0, 1024)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_partition_end();

        r.produces_partition_start(to_key(2))
            .produces_static_row({{st_cdef, int32_type->decompose(int32_t(999))}});
        for (auto idx: boost::irange(0, 1024)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_partition_end()
            .produces_end_of_stream();
        max_reads = aio_reads_tracker();
    }
    // filtering read
    {
        auto aio_reads_tracker = make_reads_tracker();
        auto slice = partition_slice_builder(*UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA)
            .with_range(query::clustering_range::make({to_ck(70), true}, {to_ck(80), false}))
            .with_range(query::clustering_range::make({to_ck(1000), false}, {to_ck(1023), true}))
            .build();

        auto rd = sst.read_range_rows_flat(query::full_partition_range, slice);
        rd.set_max_buffer_size(1);
        auto r = assert_that(std::move(rd));
        r.produces_partition_start(to_key(1))
            .produces_static_row({{st_cdef, int32_type->decompose(int32_t(777))}});
        for (auto idx: boost::irange(70, 80)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        for (auto idx: boost::irange(1001, 1024)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_partition_end();

        r.produces_partition_start(to_key(2))
            .produces_static_row({{st_cdef, int32_type->decompose(int32_t(999))}});
        for (auto idx: boost::irange(70, 80)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        for (auto idx: boost::irange(1001, 1024)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }

        r.produces_partition_end()
            .produces_end_of_stream();
        BOOST_REQUIRE(aio_reads_tracker() < max_reads);
    }
    // forwarding read
    {
        auto rd = sst.read_range_rows_flat(query::full_partition_range,
                                           UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA->full_slice(),
                                           default_priority_class(),
                                           no_resource_tracking(),
                                           streamed_mutation::forwarding::yes);
        rd.set_max_buffer_size(1);
        auto r = assert_that(std::move(rd));
        r.produces_partition_start(to_key(1));

        r.fast_forward_to(to_ck(316), to_ck(379));
        for (auto idx: boost::irange(316, 379)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_end_of_stream();

        r.next_partition();

        r.produces_partition_start(to_key(2))
                .produces_static_row({{st_cdef, int32_type->decompose(int32_t(999))}})
                .produces_end_of_stream();

        r.fast_forward_to(to_ck(442), to_ck(450));
        for (auto idx: boost::irange(442, 450)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_end_of_stream();

        r.fast_forward_to(to_ck(1009), to_ck(1024));
        for (auto idx: boost::irange(1009, 1024)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_end_of_stream();
    }
    // filtering and forwarding read
    {
        auto aio_reads_tracker = make_reads_tracker();
        auto slice =  partition_slice_builder(*UNCOMPRESSED_SKIP_USING_INDEX_ROWS_SCHEMA)
            .with_range(query::clustering_range::make({to_ck(210), true}, {to_ck(240), true}))
            .with_range(query::clustering_range::make({to_ck(1000), true}, {to_ck(1023), true}))
            .build();
        auto rd = sst.read_range_rows_flat(query::full_partition_range,
                                                      slice,
                                                      default_priority_class(),
                                                      no_resource_tracking(),
                                                      streamed_mutation::forwarding::yes);
        rd.set_max_buffer_size(1);
        auto r = assert_that(std::move(rd));

        r.produces_partition_start(to_key(1));
        r.fast_forward_to(to_ck(200), to_ck(250));

        for (auto idx: boost::irange(210, 241)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_end_of_stream();

        r.fast_forward_to(to_ck(900), to_ck(1001));
        r.produces_row(to_ck(1000), to_expected(format("{}{}", rc_base, 1000)))
            .produces_end_of_stream();

        r.next_partition();

        r.produces_partition_start(to_key(2))
            .produces_static_row({{st_cdef, int32_type->decompose(int32_t(999))}})
            .produces_end_of_stream();

        r.fast_forward_to(to_ck(200), to_ck(250));
        for (auto idx: boost::irange(210, 241)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_end_of_stream();

        r.fast_forward_to(to_ck(900), to_ck(1010));
        for (auto idx: boost::irange(1000, 1010)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_end_of_stream();

        r.fast_forward_to(to_ck(1010), to_ck(1024));
        for (auto idx: boost::irange(1010, 1024)) {
            r.produces_row(to_ck(idx), to_expected(format("{}{}", rc_base, idx)));
        }
        r.produces_end_of_stream();
        BOOST_REQUIRE(aio_reads_tracker() < max_reads);
    }
}

/*
 * Test file are generated using the following script (Python):
     session.execute("""
        CREATE TABLE IF NOT EXISTS filtering_and_ff_rt (
            pk int,
            ck1 int,
            ck2 int,
            st int static,
            rc int,
            PRIMARY KEY (pk, ck1, ck2)
        )
        WITH compression = { 'sstable_compression' : '' }
        AND caching = {'keys': 'NONE', 'rows_per_partition': 'NONE'}
        """)

    query_static = SimpleStatement("""
        INSERT INTO filtering_and_ff_rt (pk, st)
        VALUES (%s, %s) USING TIMESTAMP 1525385507816568
	""", consistency_level=ConsistencyLevel.ONE)

    query = SimpleStatement("""
        INSERT INTO filtering_and_ff_rt (pk, ck1, ck2, rc)
        VALUES (%s, %s, %s, %s) USING TIMESTAMP 1525385507816568
        """, consistency_level=ConsistencyLevel.ONE)

    query_del = SimpleStatement("""
	DELETE FROM filtering_and_ff_rt USING TIMESTAMP 1525385507816568
	WHERE pk = %s AND ck1 >= %s AND ck1 <= %s
        """, consistency_level=ConsistencyLevel.ONE)

    session.execute(query_static, [1, 777])

    for i in range(1, 1024 * 128, 3):
        log.info("inserting row %d" % i)
        session.execute(query, [1, i, i, i])
	session.execute(query_del, [1, i + 1, i + 2])

    session.execute(query_static, [2, 999])

    for i in range(1, 1024 * 128, 3):
        log.info("inserting row %d" % i)
        session.execute(query, [2, i, i, i])
	session.execute(query_del, [2, i + 1, i + 2])
 *
 * Bounds of promoted index blocks:
 * (1, 1) - (4469) <incl_start>, end_open_marker set
 * (4470) <incl_end> - (8938, 8938), end_open_marker empty
 * (8939) <incl_start> - (13407) <incl_end>, end_open_marker empty
 * (13408, 13408) - (17876) <incl_start>, end_open_marker set
 * (17877) <incl_end> - (22345, 22345), end_open_marker empty
 * ...
 */
static thread_local const sstring UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_PATH =
    "tests/sstables/3.x/uncompressed/filtering_and_forwarding_range_tombstones";
static thread_local const schema_ptr UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck1", int32_type, column_kind::clustering_key)
        .with_column("ck2", int32_type, column_kind::clustering_key)
        .with_column("st", int32_type, column_kind::static_column)
        .with_column("rc", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_filtering_and_forwarding_range_tombstones_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA,
                           UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_PATH);
    sst.load();

    auto to_pkey = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA, pk);
    };
    auto to_non_full_ck = [] (int ck) {
        return clustering_key_prefix::from_single_value(
                *UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA,
                int32_type->decompose(ck));
    };
    auto to_full_ck = [] (int ck1, int ck2) {
        return clustering_key::from_deeply_exploded(
                *UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA,
                { data_value{ck1}, data_value{ck2} });
    };
    auto make_tombstone = [] (int64_t ts, int32_t tp) {
        return tombstone{api::timestamp_type{ts}, gc_clock::time_point(gc_clock::duration(tp))};
    };
    auto make_range_tombstone = [] (clustering_key_prefix start, clustering_key_prefix end, tombstone t) {
        return range_tombstone {
            std::move(start),
            bound_kind::incl_start,
            std::move(end),
            bound_kind::incl_end,
            t};
    };

    auto make_clustering_range = [] (clustering_key_prefix&& start, clustering_key_prefix&& end) {
        return query::clustering_range::make(
            query::clustering_range::bound(std::move(start), true),
            query::clustering_range::bound(std::move(end), true));
    };

    auto make_assertions = [] (flat_mutation_reader rd) {
        rd.set_max_buffer_size(1);
        return assert_that(std::move(rd));
    };

    std::array<int32_t, 2> static_row_values {777, 999};
    auto st_cdef = UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA->get_column_definition(to_bytes("st"));
    BOOST_REQUIRE(st_cdef);
    auto rc_cdef = UNCOMPRESSED_FILTERING_AND_FORWARDING_RANGE_TOMBSTONES_SCHEMA->get_column_definition(to_bytes("rc"));
    BOOST_REQUIRE(rc_cdef);

    auto to_expected = [rc_cdef] (int val) {
        return std::vector<flat_reader_assertions::expected_column>{{rc_cdef, int32_type->decompose(int32_t(val))}};
    };

    // Sequential read
    {
        auto r = make_assertions(sst.read_rows_flat());
        tombstone tomb = make_tombstone(1525385507816568, 1534898526);
        for (auto pkey : boost::irange(1, 3)) {
            r.produces_partition_start(to_pkey(pkey))
            .produces_static_row({{st_cdef, int32_type->decompose(static_row_values[pkey - 1])}});

            for (auto idx : boost::irange(1, 1024 * 128, 3)) {
                range_tombstone rt =
                        make_range_tombstone( to_non_full_ck(idx + 1), to_non_full_ck(idx + 2), tomb);

                r.produces_row(to_full_ck(idx, idx), to_expected(idx))
                .produces_range_tombstone(rt);
            }
            r.produces_partition_end();
        }
        r.produces_end_of_stream();
    }

    // forwarding read
    {
        auto r = make_assertions(sst.read_range_rows_flat(query::full_partition_range,
                                           UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA->full_slice(),
                                           default_priority_class(),
                                           no_resource_tracking(),
                                           streamed_mutation::forwarding::yes));
        std::array<int32_t, 2> rt_deletion_times {1534898600, 1534899416};
        for (auto pkey : boost::irange(1, 3)) {
            const tombstone tomb = make_tombstone(1525385507816568, rt_deletion_times[pkey - 1]);
            r.produces_partition_start(to_pkey(pkey));
            // First, fast-forward to a block that start with an end open marker set
            // and step over it so that it doesn't get emitted
            r.fast_forward_to(to_full_ck(4471, 4471), to_full_ck(4653, 4653));
            for (const auto idx : boost::irange(4471, 4652, 3)) {
                r.produces_row(to_full_ck(idx, idx), to_expected(idx))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(idx + 1), to_non_full_ck(idx + 2), tomb));
            }
            r.produces_end_of_stream();

            // We have a range tombstone start read, now make sure we reset it properly
            // when we fast-forward to a block that doesn't have an end open marker.
            {
                r.fast_forward_to(to_full_ck(13413, 13413), to_non_full_ck(13417));
                auto slice = make_clustering_range(to_non_full_ck(13412), to_non_full_ck(13417));
                r.produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13412), to_non_full_ck(13413), tomb),
                    {slice})
                .produces_row(to_full_ck(13414, 13414), to_expected(13414))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13415), to_non_full_ck(13416), tomb),
                    {slice})
                .produces_end_of_stream();
            }

            {
                r.fast_forward_to(to_non_full_ck(13417), to_non_full_ck(13420));
                auto slice = make_clustering_range(to_non_full_ck(13419), to_non_full_ck(13420));
                r.produces_row(to_full_ck(13417, 13417), to_expected(13417))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13418), to_non_full_ck(13419), tomb),
                    {slice})
                .produces_end_of_stream();

                r.fast_forward_to(to_non_full_ck(13420), to_full_ck(13420, 13421));
                r.produces_row(to_full_ck(13420, 13420), to_expected(13420))
                        .produces_end_of_stream();

                r.fast_forward_to(to_non_full_ck(13423), to_full_ck(13423, 13424));
                r.produces_row(to_full_ck(13423, 13423), to_expected(13423))
                        .produces_end_of_stream();
            }

            {
                r.fast_forward_to(to_non_full_ck(13425), to_non_full_ck(13426));
                auto slice = make_clustering_range(to_non_full_ck(13425), to_non_full_ck(13426));
                r.produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13424), to_non_full_ck(13425), tomb),
                    {slice})
                .produces_end_of_stream();
            }

            r.fast_forward_to(to_full_ck(13429, 13428), to_full_ck(13429, 13429));
            r.produces_end_of_stream();
            r.next_partition();
        }
        r.produces_end_of_stream();
    }

    // filtering read
    {
        auto slice = partition_slice_builder(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA)
            .with_range(query::clustering_range::make({to_full_ck(4471, 4471), true}, {to_full_ck(4653, 4653), false}))
            .with_range(query::clustering_range::make({to_full_ck(13413, 13413), true}, {to_non_full_ck(13417), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13417), true}, {to_non_full_ck(13420), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13420), true}, {to_full_ck(13420, 13421), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13423), true}, {to_full_ck(13423, 13424), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13425), true}, {to_non_full_ck(13426), false}))
            .with_range(query::clustering_range::make({to_full_ck(13429, 13428), true}, {to_full_ck(13429, 13429), false}))
            .build();

        auto r = make_assertions(sst.read_range_rows_flat(query::full_partition_range, slice));
        std::array<int32_t, 2> rt_deletion_times {1534898600, 1534899416};
        for (auto pkey : boost::irange(1, 3)) {
            const tombstone tomb = make_tombstone(1525385507816568, rt_deletion_times[pkey - 1]);
            r.produces_partition_start(to_pkey(pkey))
            .produces_static_row({{st_cdef, int32_type->decompose(static_row_values[pkey - 1])}});
            for (const auto idx : boost::irange(4471, 4652, 3)) {
                r.produces_row(to_full_ck(idx, idx), to_expected(idx))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(idx + 1), to_non_full_ck(idx + 2), tomb));
            }

            {
                auto slice = make_clustering_range(to_non_full_ck(13412), to_non_full_ck(13417));
                r.produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13412), to_non_full_ck(13413), tomb),
                    {slice})
                .produces_row(to_full_ck(13414, 13414), to_expected(13414))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13415), to_non_full_ck(13416), tomb),
                    {slice});
            }

            {
                auto slice = make_clustering_range(to_non_full_ck(13419), to_non_full_ck(13420));
                r.produces_row(to_full_ck(13417, 13417), to_expected(13417))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13418), to_non_full_ck(13419), tomb),
                    {slice})
                .produces_row(to_full_ck(13420, 13420), to_expected(13420))
                .produces_row(to_full_ck(13423, 13423), to_expected(13423));
            }

            {
                auto slice = make_clustering_range(to_non_full_ck(13425), to_non_full_ck(13426));
                r.produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13424), to_non_full_ck(13425), tomb),
                    {slice});
            }

            r.next_partition();
        }
        r.produces_end_of_stream();
    }

    // filtering and forwarding read
    {
        auto slice = partition_slice_builder(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA)
            .with_range(query::clustering_range::make({to_full_ck(4471, 4471), true}, {to_full_ck(4653, 4653), false}))
            .with_range(query::clustering_range::make({to_full_ck(13413, 13413), true}, {to_non_full_ck(13417), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13417), true}, {to_non_full_ck(13420), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13420), true}, {to_full_ck(13420, 13421), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13423), true}, {to_full_ck(13423, 13424), false}))
            .with_range(query::clustering_range::make({to_non_full_ck(13425), true}, {to_non_full_ck(13426), false}))
            .with_range(query::clustering_range::make({to_full_ck(13429, 13428), true}, {to_full_ck(13429, 13429), false}))
            .build();

        auto r = make_assertions(sst.read_range_rows_flat(query::full_partition_range,
                                                      slice,
                                                      default_priority_class(),
                                                      no_resource_tracking(),
                                                      streamed_mutation::forwarding::yes));

        std::array<int32_t, 2> rt_deletion_times {1534898600, 1534899416};
        for (auto pkey : boost::irange(1, 3)) {
            const tombstone tomb = make_tombstone(1525385507816568, rt_deletion_times[pkey - 1]);
            r.produces_partition_start(to_pkey(pkey))
            .produces_static_row({{st_cdef, int32_type->decompose(static_row_values[pkey - 1])}})
            .produces_end_of_stream();

            r.fast_forward_to(to_non_full_ck(3000), to_non_full_ck(6000));
            for (const auto idx : boost::irange(4471, 4652, 3)) {
                r.produces_row(to_full_ck(idx, idx), to_expected(idx))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(idx + 1), to_non_full_ck(idx + 2), tomb));
            }
            r.produces_end_of_stream();

            r.fast_forward_to(to_non_full_ck(13000), to_non_full_ck(15000));
            {
                auto slice = make_clustering_range(to_non_full_ck(13412), to_non_full_ck(13417));
                r.produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13412), to_non_full_ck(13413), tomb),
                    {slice})
                .produces_row(to_full_ck(13414, 13414), to_expected(13414))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13415), to_non_full_ck(13416), tomb),
                    {slice});
            }

            {
                auto slice = make_clustering_range(to_non_full_ck(13419), to_non_full_ck(13420));
                r.produces_row(to_full_ck(13417, 13417), to_expected(13417))
                .produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13418), to_non_full_ck(13419), tomb),
                    {slice})
                .produces_row(to_full_ck(13420, 13420), to_expected(13420))
                .produces_row(to_full_ck(13423, 13423), to_expected(13423));
            }

            {
                auto slice = make_clustering_range(to_non_full_ck(13425), to_non_full_ck(13426));
                r.produces_range_tombstone(
                    make_range_tombstone(to_non_full_ck(13424), to_non_full_ck(13425), tomb),
                    {slice});
            }

            r.produces_end_of_stream();
            r.next_partition();
        }
        r.produces_end_of_stream();
    }
}

/*
 * Test file are generated using the following script (Python):
    session.execute("""
        CREATE TABLE IF NOT EXISTS slicing_interleaved_rows_and_rts (
            pk int,
            ck1 int,
            ck2 int,
            st int static,
            rc int,
            PRIMARY KEY (pk, ck1, ck2)
        )
        WITH compression = { 'sstable_compression' : '' }
        AND caching = {'keys': 'NONE', 'rows_per_partition': 'NONE'}
        """)

    query_static = SimpleStatement("""
        INSERT INTO slicing_interleaved_rows_and_rts (pk, st)
        VALUES (%s, %s) USING TIMESTAMP 1525385507816578
	""", consistency_level=ConsistencyLevel.ONE)

    query = SimpleStatement("""
        INSERT INTO slicing_interleaved_rows_and_rts (pk, ck1, ck2, rc)
        VALUES (%s, %s, %s, %s) USING TIMESTAMP 1525385507816578
        """, consistency_level=ConsistencyLevel.ONE)

    query_del = SimpleStatement("""
	DELETE FROM slicing_interleaved_rows_and_rts USING TIMESTAMP 1525385507816568
	WHERE pk = %s AND ck1 >= %s AND ck1 <= %s
        """, consistency_level=ConsistencyLevel.ONE)

    session.execute(query_static, [1, 555])

    for i in range(1, 1024 * 128, 5):
	session.execute(query_del, [1, i, i + 4])

    for i in range(1, 1024 * 128, 5):
        session.execute(query, [1, i + 3, i + 3, i + 3])
 *
 * Bounds of promoted index blocks:
 * (1) <incl_start> - (7449, 7449), end_open_marker set
 * (7450) <incl_end> - (14896) <incl_start>, end_open_marker set
 * (14899, 14899) - (22345) <incl_end>, end_open_marker empty
 * (22346) <incl_start> - (29794, 29794), end_open_marker set
 * (29795) <incl_end> - (37241) <incl_start>, end_open_marker set
 * ...
 */
static thread_local const sstring UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_PATH =
    "tests/sstables/3.x/uncompressed/slicing_interleaved_rows_and_rts";
static thread_local const schema_ptr UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck1", int32_type, column_kind::clustering_key)
        .with_column("ck2", int32_type, column_kind::clustering_key)
        .with_column("st", int32_type, column_kind::static_column)
        .with_column("rc", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_slicing_interleaved_rows_and_rts_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA,
                           UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_PATH);
    sst.load();

    auto to_pkey = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA, pk);
    };
    auto to_non_full_ck = [] (int ck) {
        return clustering_key_prefix::from_single_value(
                *UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA,
                int32_type->decompose(ck));
    };
    auto to_full_ck = [] (int ck1, int ck2) {
        return clustering_key::from_deeply_exploded(
                *UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA,
                { data_value{ck1}, data_value{ck2} });
    };
    auto make_tombstone = [] (int64_t ts, int32_t tp) {
        return tombstone{api::timestamp_type{ts}, gc_clock::time_point(gc_clock::duration(tp))};
    };
    const auto make_range_tombstone_creator = [] (bound_kind start_kind) {
        return [start_kind] (clustering_key_prefix start, clustering_key_prefix end, tombstone t) {
            return range_tombstone {
                std::move(start),
                start_kind,
                std::move(end),
                bound_kind::incl_end,
                t};
        };
    };

    const auto make_rt_incl_start = make_range_tombstone_creator(bound_kind::incl_start);
    const auto make_rt_excl_start = make_range_tombstone_creator(bound_kind::excl_start);

    auto make_clustering_range = [] (clustering_key_prefix&& start, clustering_key_prefix&& end) {
        return query::clustering_range::make(
            query::clustering_range::bound(std::move(start), true),
            query::clustering_range::bound(std::move(end), false));
    };

    auto make_assertions = [] (flat_mutation_reader rd) {
        rd.set_max_buffer_size(1);
        return assert_that(std::move(rd));
    };

    auto st_cdef = UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA->get_column_definition(to_bytes("st"));
    BOOST_REQUIRE(st_cdef);
    auto rc_cdef = UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA->get_column_definition(to_bytes("rc"));
    BOOST_REQUIRE(rc_cdef);

    auto to_expected = [rc_cdef] (int val) {
        return std::vector<flat_reader_assertions::expected_column>{{rc_cdef, int32_type->decompose(int32_t(val))}};
    };

    // Sequential read
    {
        auto r = make_assertions(sst.read_rows_flat());
        tombstone tomb = make_tombstone(1525385507816568, 1534898526);
        r.produces_partition_start(to_pkey(1))
        .produces_static_row({{st_cdef, int32_type->decompose(int32_t(555))}});

        for (auto idx : boost::irange(1, 1024 * 128, 5)) {
            range_tombstone rt1 =
                    make_rt_incl_start(to_non_full_ck(idx), to_full_ck(idx + 3, idx + 3), tomb);
            range_tombstone rt2 =
                    make_rt_excl_start(to_full_ck(idx + 3, idx + 3), to_non_full_ck(idx + 4), tomb);

            r.produces_range_tombstone(rt1)
            .produces_row(to_full_ck(idx + 3, idx + 3), to_expected(idx + 3))
            .produces_range_tombstone(rt2);
        }
        r.produces_partition_end()
        .produces_end_of_stream();
    }

    // forwarding read
    {
        auto r = make_assertions(sst.read_range_rows_flat(query::full_partition_range,
                                           UNCOMPRESSED_SLICING_INTERLEAVED_ROWS_AND_RTS_SCHEMA->full_slice(),
                                           default_priority_class(),
                                           no_resource_tracking(),
                                           streamed_mutation::forwarding::yes));

        const tombstone tomb = make_tombstone(1525385507816568, 1535592075);
        r.produces_partition_start(to_pkey(1));
        r.fast_forward_to(to_full_ck(7460, 7461), to_full_ck(7500, 7501));
        {
            auto clustering_range = make_clustering_range(to_non_full_ck(7000), to_non_full_ck(8000));

            range_tombstone rt =
                    make_rt_excl_start(to_full_ck(7459, 7459), to_non_full_ck(7460), tomb);
            r.produces_range_tombstone(rt, {clustering_range});

            for (auto idx : boost::irange(7461, 7501, 5)) {
                range_tombstone rt1 =
                        make_rt_incl_start(to_non_full_ck(idx), to_full_ck(idx + 3, idx + 3), tomb);
                range_tombstone rt2 =
                        make_rt_excl_start(to_full_ck(idx + 3, idx + 3), to_non_full_ck(idx + 4), tomb);

                r.produces_range_tombstone(rt1, {clustering_range})
                .produces_row(to_full_ck(idx + 3, idx + 3), to_expected(idx + 3))
                .produces_range_tombstone(rt2, {clustering_range});
            }
            r.produces_end_of_stream();
        }
    }

    // filtering read
    {
        auto slice = partition_slice_builder(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA)
            .with_range(make_clustering_range(to_full_ck(7460, 7461), to_full_ck(7500, 7501)))
            .build();

        auto r = make_assertions(sst.read_range_rows_flat(query::full_partition_range, slice));
        const tombstone tomb = make_tombstone(1525385507816568, 1535592075);

        r.produces_partition_start(to_pkey(1))
        .produces_static_row({{st_cdef, int32_type->decompose(int32_t(555))}});

        auto clustering_range = make_clustering_range(to_non_full_ck(7000), to_non_full_ck(8000));
        range_tombstone rt =
                make_rt_excl_start(to_full_ck(7459, 7459), to_non_full_ck(7460), tomb);

        r.produces_range_tombstone(rt, {clustering_range});

        for (auto idx : boost::irange(7461, 7501, 5)) {
            range_tombstone rt1 =
                    make_rt_incl_start(to_non_full_ck(idx), to_full_ck(idx + 3, idx + 3), tomb);
            range_tombstone rt2 =
                    make_rt_excl_start(to_full_ck(idx + 3, idx + 3), to_non_full_ck(idx + 4), tomb);

            r.produces_range_tombstone(rt1, {clustering_range})
            .produces_row(to_full_ck(idx + 3, idx + 3), to_expected(idx + 3))
            .produces_range_tombstone(rt2, {clustering_range});
        }
        r.produces_partition_end()
        .produces_end_of_stream();
    }

    // filtering and forwarding read
    {
        auto slice = partition_slice_builder(*UNCOMPRESSED_FILTERING_AND_FORWARDING_SCHEMA)
            .with_range(make_clustering_range(to_full_ck(7470, 7471), to_full_ck(7500, 7501)))
            .build();

        auto r = make_assertions(sst.read_range_rows_flat(query::full_partition_range,
                                                      slice,
                                                      default_priority_class(),
                                                      no_resource_tracking(),
                                                      streamed_mutation::forwarding::yes));
        const tombstone tomb = make_tombstone(1525385507816568, 1535592075);
        r.produces_partition_start(to_pkey(1));
        r.fast_forward_to(to_full_ck(7460, 7461), to_full_ck(7600, 7601));

        auto clustering_range = make_clustering_range(to_non_full_ck(7000), to_non_full_ck(8000));
        range_tombstone rt =
                make_rt_excl_start(to_full_ck(7469, 7469), to_non_full_ck(7470), tomb);

        r.produces_range_tombstone(rt, {clustering_range});

        for (auto idx : boost::irange(7471, 7501, 5)) {
            range_tombstone rt1 =
                    make_rt_incl_start(to_non_full_ck(idx), to_full_ck(idx + 3, idx + 3), tomb);
            range_tombstone rt2 =
                    make_rt_excl_start(to_full_ck(idx + 3, idx + 3), to_non_full_ck(idx + 4), tomb);

            r.produces_range_tombstone(rt1, {clustering_range})
            .produces_row(to_full_ck(idx + 3, idx + 3), to_expected(idx + 3))
            .produces_range_tombstone(rt2, {clustering_range});
        }
        r.produces_end_of_stream();
    }
}

// Following tests run on files in tests/sstables/3.x/uncompressed/static_row
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, ck INT, s INT STATIC, val INT, PRIMARY KEY(pk, ck))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, ck, s, val) VALUES(1, 11, 101, 1001);
// INSERT INTO test_ks.test_table(pk, ck, s, val) VALUES(2, 12, 102, 1002);
// INSERT INTO test_ks.test_table(pk, ck, s, val) VALUES(3, 13, 103, 1003);
// INSERT INTO test_ks.test_table(pk, ck, s, val) VALUES(4, 14, 104, 1004);
// INSERT INTO test_ks.test_table(pk, ck, s, val) VALUES(5, 15, 105, 1005);

static thread_local const sstring UNCOMPRESSED_STATIC_ROW_PATH =
    "tests/sstables/3.x/uncompressed/static_row";
static thread_local const schema_ptr UNCOMPRESSED_STATIC_ROW_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("s", int32_type, column_kind::static_column)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_static_row_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_STATIC_ROW_SCHEMA,
                           UNCOMPRESSED_STATIC_ROW_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_STATIC_ROW_SCHEMA, pk);
    };

    auto s_cdef = UNCOMPRESSED_STATIC_ROW_SCHEMA->get_column_definition(to_bytes("s"));
    BOOST_REQUIRE(s_cdef);
    auto val_cdef = UNCOMPRESSED_STATIC_ROW_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(val_cdef);

    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_static_row({{s_cdef, int32_type->decompose(int32_t(105))}})
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(15)),
                      {{val_cdef, int32_type->decompose(int32_t(1005))}})
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_static_row({{s_cdef, int32_type->decompose(int32_t(101))}})
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(11)),
                      {{val_cdef, int32_type->decompose(int32_t(1001))}})
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_static_row({{s_cdef, int32_type->decompose(int32_t(102))}})
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(12)),
                      {{val_cdef, int32_type->decompose(int32_t(1002))}})
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_static_row({{s_cdef, int32_type->decompose(int32_t(104))}})
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(14)),
                      {{val_cdef, int32_type->decompose(int32_t(1004))}})
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_static_row({{s_cdef, int32_type->decompose(int32_t(103))}})
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(13)),
                      {{val_cdef, int32_type->decompose(int32_t(1003))}})
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/compound_static_row
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT,
//                                   ck INT,
//                                   s_int INT STATIC,
//                                   s_text TEXT STATIC,
//                                   s_inet INET STATIC,
//                                   val INT,
//                                   PRIMARY KEY(pk, ck))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, ck, s_int, s_text, s_inet, val)
//                         VALUES(1, 11, 101, 'Text for 1', '10.0.0.1', 1001);
// INSERT INTO test_ks.test_table(pk, ck, s_int, s_text, s_inet, val)
//                         VALUES(2, 12, 102, 'Text for 2', '10.0.0.2', 1002);
// INSERT INTO test_ks.test_table(pk, ck, s_int, s_text, s_inet, val)
//                         VALUES(3, 13, 103, 'Text for 3', '10.0.0.3', 1003);
// INSERT INTO test_ks.test_table(pk, ck, s_int, s_text, s_inet, val)
//                         VALUES(4, 14, 104, 'Text for 4', '10.0.0.4', 1004);
// INSERT INTO test_ks.test_table(pk, ck, s_int, s_text, s_inet, val)
//                         VALUES(5, 15, 105, 'Text for 5', '10.0.0.5', 1005);

static thread_local const sstring UNCOMPRESSED_COMPOUND_STATIC_ROW_PATH =
    "tests/sstables/3.x/uncompressed/compound_static_row";
static thread_local const schema_ptr UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("s_int", int32_type, column_kind::static_column)
        .with_column("s_text", utf8_type, column_kind::static_column)
        .with_column("s_inet", inet_addr_type, column_kind::static_column)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_compound_static_row_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA,
                           UNCOMPRESSED_COMPOUND_STATIC_ROW_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA, pk);
    };

    auto s_int_cdef = UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA->get_column_definition(to_bytes("s_int"));
    BOOST_REQUIRE(s_int_cdef);
    auto s_text_cdef = UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA->get_column_definition(to_bytes("s_text"));
    BOOST_REQUIRE(s_text_cdef);
    auto s_inet_cdef = UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA->get_column_definition(to_bytes("s_inet"));
    BOOST_REQUIRE(s_inet_cdef);
    auto val_cdef = UNCOMPRESSED_COMPOUND_STATIC_ROW_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(val_cdef);

    auto generate = [&] (int int_val, sstring_view text_val, sstring_view inet_val) {
        std::vector<flat_reader_assertions::expected_column> columns;

        columns.push_back({s_int_cdef, int32_type->decompose(int_val)});
        columns.push_back({s_text_cdef, utf8_type->from_string(text_val)});
        columns.push_back({s_inet_cdef, inet_addr_type->from_string(inet_val)});

        return std::move(columns);
    };

    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_static_row(generate(105, "Text for 5", "10.0.0.5"))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(15)),
                      {{val_cdef, int32_type->decompose(int32_t(1005))}})
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_static_row(generate(101, "Text for 1", "10.0.0.1"))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(11)),
                      {{val_cdef, int32_type->decompose(int32_t(1001))}})
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_static_row(generate(102, "Text for 2", "10.0.0.2"))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(12)),
                      {{val_cdef, int32_type->decompose(int32_t(1002))}})
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_static_row(generate(104, "Text for 4", "10.0.0.4"))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(14)),
                      {{val_cdef, int32_type->decompose(int32_t(1004))}})
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_static_row(generate(103, "Text for 3", "10.0.0.3"))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_STATIC_ROW_SCHEMA,
                                                        int32_type->decompose(13)),
                      {{val_cdef, int32_type->decompose(int32_t(1003))}})
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/partition_key_only
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk) VALUES(1);
// INSERT INTO test_ks.test_table(pk) VALUES(2);
// INSERT INTO test_ks.test_table(pk) VALUES(3);
// INSERT INTO test_ks.test_table(pk) VALUES(4);
// INSERT INTO test_ks.test_table(pk) VALUES(5);

static thread_local const sstring UNCOMPRESSED_PARTITION_KEY_ONLY_PATH =
    "tests/sstables/3.x/uncompressed/partition_key_only";
static thread_local const schema_ptr UNCOMPRESSED_PARTITION_KEY_ONLY_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_partition_key_only_load) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_PARTITION_KEY_ONLY_SCHEMA, UNCOMPRESSED_PARTITION_KEY_ONLY_PATH);
    sst.load();
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_partition_key_only_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_PARTITION_KEY_ONLY_SCHEMA, UNCOMPRESSED_PARTITION_KEY_ONLY_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_PARTITION_KEY_ONLY_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_PARTITION_KEY_ONLY_SCHEMA, pk);
    };
    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row_with_key(clustering_key_prefix::make_empty())
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row_with_key(clustering_key_prefix::make_empty())
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row_with_key(clustering_key_prefix::make_empty())
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row_with_key(clustering_key_prefix::make_empty())
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row_with_key(clustering_key_prefix::make_empty())
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/partition_key_with_value
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, val INT, PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, val) VALUES(1, 101);
// INSERT INTO test_ks.test_table(pk, val) VALUES(2, 102);
// INSERT INTO test_ks.test_table(pk, val) VALUES(3, 103);
// INSERT INTO test_ks.test_table(pk, val) VALUES(4, 104);
// INSERT INTO test_ks.test_table(pk, val) VALUES(5, 105);

static thread_local const sstring UNCOMPRESSED_PARTITION_KEY_WITH_VALUE_PATH =
    "tests/sstables/3.x/uncompressed/partition_key_with_value";
static thread_local const schema_ptr UNCOMPRESSED_PARTITION_KEY_WITH_VALUE_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_partition_key_with_value_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_PARTITION_KEY_WITH_VALUE_SCHEMA,
                           UNCOMPRESSED_PARTITION_KEY_WITH_VALUE_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_PARTITION_KEY_WITH_VALUE_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_PARTITION_KEY_WITH_VALUE_SCHEMA, pk);
    };

    auto cdef = UNCOMPRESSED_PARTITION_KEY_WITH_VALUE_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(cdef);

    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row(clustering_key_prefix::make_empty(), {{cdef, int32_type->decompose(int32_t(105))}})
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row(clustering_key_prefix::make_empty(), {{cdef, int32_type->decompose(int32_t(101))}})
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row(clustering_key_prefix::make_empty(), {{cdef, int32_type->decompose(int32_t(102))}})
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row(clustering_key_prefix::make_empty(), {{cdef, int32_type->decompose(int32_t(104))}})
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row(clustering_key_prefix::make_empty(), {{cdef, int32_type->decompose(int32_t(103))}})
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/counters
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_table (pk INT, val COUNTER, PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 1;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 1;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 1;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 2;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 2;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 3;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 3;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 3;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 3;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 3;
// UPDATE test_ks.test_table SET val = val + 1 WHERE pk = 3;

static thread_local const sstring UNCOMPRESSED_COUNTERS_PATH =
    "tests/sstables/3.x/uncompressed/counters";
static thread_local const schema_ptr UNCOMPRESSED_COUNTERS_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("val", counter_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

static thread_local const counter_id HOST_ID = counter_id(utils::UUID("59b82720-99b0-4033-885c-e94d62106a35"));

SEASTAR_THREAD_TEST_CASE(test_uncompressed_counters_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_COUNTERS_SCHEMA,
                           UNCOMPRESSED_COUNTERS_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_COUNTERS_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_COUNTERS_SCHEMA, pk);
    };

    auto cdef = UNCOMPRESSED_COUNTERS_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(cdef);

    auto generate = [&] (api::timestamp_type timestamp, int64_t value, int64_t clock) {
        std::vector<flat_reader_assertions::assert_function> assertions;

        assertions.push_back([&, timestamp, value, clock] (const column_definition& def,
                                                           const atomic_cell_or_collection* cell) {
            BOOST_REQUIRE(def.is_counter());
            counter_cell_view::with_linearized(cell->as_atomic_cell(def), [&] (counter_cell_view cv) {
                BOOST_REQUIRE_EQUAL(timestamp, cv.timestamp());
                BOOST_REQUIRE_EQUAL(1, cv.shard_count());
                auto shard = cv.get_shard(HOST_ID);
                BOOST_REQUIRE(shard);
                BOOST_REQUIRE_EQUAL(value, shard->value());
                BOOST_REQUIRE_EQUAL(clock, shard->logical_clock());
            });
        });

        return assertions;
    };

    assert_that(sst.read_rows_flat())
    .produces_partition_start(to_key(1))
    .produces_row(clustering_key_prefix::make_empty(), {cdef->id}, generate(1528799884266910, 3, 1528799884268000))
    .produces_partition_end()
    .produces_partition_start(to_key(2))
    .produces_row(clustering_key_prefix::make_empty(), {cdef->id}, generate(1528799884272645, 2, 1528799884274000))
    .produces_partition_end()
    .produces_partition_start(to_key(3))
    .produces_row(clustering_key_prefix::make_empty(), {cdef->id}, generate(1528799885105152, 6, 1528799885107000))
    .produces_partition_end()
    .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/{uncompressed,compressed}/partition_key_with_value_of_different_types
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT,
//                                   bool_val BOOLEAN,
//                                   double_val DOUBLE,
//                                   float_val FLOAT,
//                                   int_val INT,
//                                   long_val BIGINT,
//                                   timestamp_val TIMESTAMP,
//                                   timeuuid_val TIMEUUID,
//                                   uuid_val UUID,
//                                   text_val TEXT,
//                                   PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// "WITH compression = { 'enabled' : false };" is used only for uncompressed test. Compressed test does not use it.
//
// INSERT INTO test_ks.test_table(pk, bool_val, double_val, float_val, int_val, long_val, timestamp_val, timeuuid_val,
//                                uuid_val, text_val)
//                         VALUES(1, true, 0.11, 0.1, 1, 11, '2015-05-01 09:30:54.234+0000',
//                                50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab,
//                                'variable length text 1');
// INSERT INTO test_ks.test_table(pk, bool_val, double_val, float_val, int_val, long_val, timestamp_val, timeuuid_val,
//                                uuid_val, text_val)
//                         VALUES(2, false, 0.22, 0.2, 2, 22, '2015-05-02 10:30:54.234+0000',
//                                50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab,
//                                'variable length text 2');
// INSERT INTO test_ks.test_table(pk, bool_val, double_val, float_val, int_val, long_val, timestamp_val, timeuuid_val,
//                                uuid_val, text_val)
//                         VALUES(3, true, 0.33, 0.3, 3, 33, '2015-05-03 11:30:54.234+0000',
//                                50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab,
//                                'variable length text 3');
// INSERT INTO test_ks.test_table(pk, bool_val, double_val, float_val, int_val, long_val, timestamp_val, timeuuid_val,
//                                uuid_val, text_val)
//                         VALUES(4, false, 0.44, 0.4, 4, 44, '2015-05-04 12:30:54.234+0000',
//                                50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab,
//                                'variable length text 4');
// INSERT INTO test_ks.test_table(pk, bool_val, double_val, float_val, int_val, long_val, timestamp_val, timeuuid_val,
//                                uuid_val, text_val)
//                         VALUES(5, true, 0.55, 0.5, 5, 55, '2015-05-05 13:30:54.234+0000',
//                                50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab,
//                                'variable length text 5');

static thread_local const sstring UNCOMPRESSED_PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_PATH =
    "tests/sstables/3.x/uncompressed/partition_key_with_values_of_different_types";
static thread_local const sstring COMPRESSED_PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_PATH =
    "tests/sstables/3.x/compressed/partition_key_with_values_of_different_types";
static thread_local const schema_ptr PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("bool_val", boolean_type)
        .with_column("double_val", double_type)
        .with_column("float_val", float_type)
        .with_column("int_val", int32_type)
        .with_column("long_val", long_type)
        .with_column("timestamp_val", timestamp_type)
        .with_column("timeuuid_val", timeuuid_type)
        .with_column("uuid_val", uuid_type)
        .with_column("text_val", utf8_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

static void test_partition_key_with_values_of_different_types_read(const sstring& path) {
    sstable_assertions sst(PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA, path);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA, pk);
    };

    auto bool_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("bool_val"));
    BOOST_REQUIRE(bool_cdef);
    auto double_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("double_val"));
    BOOST_REQUIRE(double_cdef);
    auto float_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("float_val"));
    BOOST_REQUIRE(float_cdef);
    auto int_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("int_val"));
    BOOST_REQUIRE(int_cdef);
    auto long_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("long_val"));
    BOOST_REQUIRE(long_cdef);
    auto timestamp_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("timestamp_val"));
    BOOST_REQUIRE(timestamp_cdef);
    auto timeuuid_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("timeuuid_val"));
    BOOST_REQUIRE(timeuuid_cdef);
    auto uuid_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("uuid_val"));
    BOOST_REQUIRE(uuid_cdef);
    auto text_cdef =
        PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_SCHEMA->get_column_definition(to_bytes("text_val"));
    BOOST_REQUIRE(text_cdef);

    auto generate = [&] (bool bool_val, double double_val, float float_val, int int_val, long long_val,
                         sstring_view timestamp_val, sstring_view timeuuid_val, sstring_view uuid_val,
                         sstring_view text_val) {
        std::vector<flat_reader_assertions::expected_column> columns;

        columns.push_back({bool_cdef, boolean_type->decompose(bool_val)});
        columns.push_back({double_cdef, double_type->decompose(double_val)});
        columns.push_back({float_cdef, float_type->decompose(float_val)});
        columns.push_back({int_cdef, int32_type->decompose(int_val)});
        columns.push_back({long_cdef, long_type->decompose(long_val)});
        columns.push_back({timestamp_cdef, timestamp_type->from_string(timestamp_val)});
        columns.push_back({timeuuid_cdef, timeuuid_type->from_string(timeuuid_val)});
        columns.push_back({uuid_cdef, uuid_type->from_string(uuid_val)});
        columns.push_back({text_cdef, utf8_type->from_string(text_val)});

        return std::move(columns);
    };

    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(true, 0.55, 0.5, 5, 55, "2015-05-05 13:30:54.234+0000",
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               "variable length text 5"))
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(true, 0.11, 0.1, 1, 11, "2015-05-01 09:30:54.234+0000",
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               "variable length text 1"))
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(false, 0.22, 0.2, 2, 22, "2015-05-02 10:30:54.234+0000",
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               "variable length text 2"))
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(false, 0.44, 0.4, 4, 44, "2015-05-04 12:30:54.234+0000",
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               "variable length text 4"))
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(true, 0.33, 0.3, 3, 33, "2015-05-03 11:30:54.234+0000",
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               "variable length text 3"))
        .produces_partition_end()
        .produces_end_of_stream();
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_partition_key_with_values_of_different_types_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    test_partition_key_with_values_of_different_types_read(
        UNCOMPRESSED_PARTITION_KEY_WITH_VALUES_OF_DIFFERENT_TYPES_PATH);
}

// Following tests run on files in tests/sstables/3.x/uncompressed/subset_of_columns
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT,
//                                   bool_val BOOLEAN,
//                                   double_val DOUBLE,
//                                   float_val FLOAT,
//                                   int_val INT,
//                                   long_val BIGINT,
//                                   timestamp_val TIMESTAMP,
//                                   timeuuid_val TIMEUUID,
//                                   uuid_val UUID,
//                                   text_val TEXT,
//                                   PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, double_val, float_val, int_val, long_val, timestamp_val, timeuuid_val,
//                                uuid_val, text_val)
//                         VALUES(1, 0.11, 0.1, 1, 11, '2015-05-01 09:30:54.234+0000',
//                                50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab,
//                                'variable length text 1');
// INSERT INTO test_ks.test_table(pk, bool_val, int_val, long_val, timestamp_val, timeuuid_val,
//                                uuid_val, text_val)
//                         VALUES(2, false, 2, 22, '2015-05-02 10:30:54.234+0000',
//                                50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab,
//                                'variable length text 2');
// INSERT INTO test_ks.test_table(pk, bool_val, double_val, float_val, long_val, timestamp_val, text_val)
//                         VALUES(3, true, 0.33, 0.3, 33, '2015-05-03 11:30:54.234+0000', 'variable length text 3');
// INSERT INTO test_ks.test_table(pk, bool_val, text_val)
//                         VALUES(4, false, 'variable length text 4');
// INSERT INTO test_ks.test_table(pk, int_val, long_val, timeuuid_val, uuid_val)
//                         VALUES(5, 5, 55, 50554d6e-29bb-11e5-b345-feff819cdc9f, 01234567-0123-0123-0123-0123456789ab);

static thread_local const sstring UNCOMPRESSED_SUBSET_OF_COLUMNS_PATH =
    "tests/sstables/3.x/uncompressed/subset_of_columns";
static thread_local const schema_ptr UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("bool_val", boolean_type)
        .with_column("double_val", double_type)
        .with_column("float_val", float_type)
        .with_column("int_val", int32_type)
        .with_column("long_val", long_type)
        .with_column("timestamp_val", timestamp_type)
        .with_column("timeuuid_val", timeuuid_type)
        .with_column("uuid_val", uuid_type)
        .with_column("text_val", utf8_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_subset_of_columns_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA,
                           UNCOMPRESSED_SUBSET_OF_COLUMNS_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA, pk);
    };

    auto bool_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("bool_val"));
    BOOST_REQUIRE(bool_cdef);
    auto double_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("double_val"));
    BOOST_REQUIRE(double_cdef);
    auto float_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("float_val"));
    BOOST_REQUIRE(float_cdef);
    auto int_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("int_val"));
    BOOST_REQUIRE(int_cdef);
    auto long_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("long_val"));
    BOOST_REQUIRE(long_cdef);
    auto timestamp_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("timestamp_val"));
    BOOST_REQUIRE(timestamp_cdef);
    auto timeuuid_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("timeuuid_val"));
    BOOST_REQUIRE(timeuuid_cdef);
    auto uuid_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("uuid_val"));
    BOOST_REQUIRE(uuid_cdef);
    auto text_cdef =
        UNCOMPRESSED_SUBSET_OF_COLUMNS_SCHEMA->get_column_definition(to_bytes("text_val"));
    BOOST_REQUIRE(text_cdef);

    auto generate = [&] (std::optional<bool> bool_val, std::optional<double> double_val,
                         std::optional<float> float_val, std::optional<int> int_val, std::optional<long> long_val,
                         std::optional<sstring_view> timestamp_val, std::optional<sstring_view> timeuuid_val,
                         std::optional<sstring_view> uuid_val, std::optional<sstring_view> text_val) {
        std::vector<flat_reader_assertions::expected_column> columns;

        if (bool_val) {
            columns.push_back({bool_cdef, boolean_type->decompose(*bool_val)});
        }
        if (double_val) {
            columns.push_back({double_cdef, double_type->decompose(*double_val)});
        }
        if (float_val) {
            columns.push_back({float_cdef, float_type->decompose(*float_val)});
        }
        if (int_val) {
            columns.push_back({int_cdef, int32_type->decompose(*int_val)});
        }
        if (long_val) {
            columns.push_back({long_cdef, long_type->decompose(*long_val)});
        }
        if (timestamp_val) {
            columns.push_back({timestamp_cdef, timestamp_type->from_string(*timestamp_val)});
        }
        if (timeuuid_val) {
            columns.push_back({timeuuid_cdef, timeuuid_type->from_string(*timeuuid_val)});
        }
        if (uuid_val) {
            columns.push_back({uuid_cdef, uuid_type->from_string(*uuid_val)});
        }
        if (text_val) {
            columns.push_back({text_cdef, utf8_type->from_string(*text_val)});
        }

        return std::move(columns);
    };

    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({}, {}, {}, 5, 55, {},
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               {}))
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({}, 0.11, 0.1, 1, 11, "2015-05-01 09:30:54.234+0000",
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               "variable length text 1"))
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(false, {}, {}, 2, 22, "2015-05-02 10:30:54.234+0000",
                               "50554d6e-29bb-11e5-b345-feff819cdc9f", "01234567-0123-0123-0123-0123456789ab",
                               "variable length text 2"))
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(false, {}, {}, {}, {}, {},
                               {}, {},
                               "variable length text 4"))
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate(true, 0.33, 0.3, {}, 33, "2015-05-03 11:30:54.234+0000",
                               {}, {},
                               "variable length text 3"))
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/large_subset_of_columns_sparse
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT,
//                                   val1 INT, val2 INT, val3 INT, val4 INT, val5 INT, val6 INT, val7 INT, val8 INT,
//                                   val9 INT,
//                                   val10 INT, val11 INT, val12 INT, val13 INT, val14 INT, val15 INT, val16 INT,
//                                   val17 INT, val18 INT, val19 INT,
//                                   val20 INT, val21 INT, val22 INT, val23 INT, val24 INT, val25 INT, val26 INT,
//                                   val27 INT, val28 INT, val29 INT,
//                                   val30 INT, val31 INT, val32 INT, val33 INT, val34 INT, val35 INT, val36 INT,
//                                   val37 INT, val38 INT, val39 INT,
//                                   val40 INT, val41 INT, val42 INT, val43 INT, val44 INT, val45 INT, val46 INT,
//                                   val47 INT, val48 INT, val49 INT,
//                                   val50 INT, val51 INT, val52 INT, val53 INT, val54 INT, val55 INT, val56 INT,
//                                   val57 INT, val58 INT, val59 INT,
//                                   val60 INT, val61 INT, val62 INT, val63 INT, val64 INT,
//                                   PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, val1) VALUES(1, 11);
// INSERT INTO test_ks.test_table(pk, val2, val5, val6, val7, val60) VALUES(2, 22, 222, 2222, 22222, 222222);
// INSERT INTO test_ks.test_table(pk,val32, val33) VALUES(3, 33, 333);
// INSERT INTO test_ks.test_table(pk, val1, val2, val3, val4, val5, val6, val7, val8, val9,
//                                    val10, val11, val12, val13, val14, val15, val16, val17, val18, val19,
//                                    val20, val21, val22, val23, val24, val25, val26, val27, val28, val29,
//                                    val30, val31)
//                         VALUES(4, 1, 2, 3, 4, 5, 6, 7, 8, 9,
//                                   10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
//                                   20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
//                                   30, 31);
// INSERT INTO test_ks.test_table(pk, val34, val35, val36, val37, val38, val39, val40, val41, val42,
//                                    val43, val44, val45, val46, val47, val48, val49, val50, val51, val52,
//                                    val53, val54, val55, val56, val57, val58, val59, val60, val61, val62,
//                                    val63, val64)
//                         VALUES(5, 1, 2, 3, 4, 5, 6, 7, 8, 9,
//                                   10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
//                                   20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
//                                   30, 31);
//
// IMPORTANT: each column has to be covered by at least one insert otherwise it won't be present in the sstable

static thread_local const sstring UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_SPARSE_PATH =
    "tests/sstables/3.x/uncompressed/large_subset_of_columns_sparse";
static thread_local const schema_ptr UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_SPARSE_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("val1", int32_type)
        .with_column("val2", int32_type)
        .with_column("val3", int32_type)
        .with_column("val4", int32_type)
        .with_column("val5", int32_type)
        .with_column("val6", int32_type)
        .with_column("val7", int32_type)
        .with_column("val8", int32_type)
        .with_column("val9", int32_type)
        .with_column("val10", int32_type)
        .with_column("val11", int32_type)
        .with_column("val12", int32_type)
        .with_column("val13", int32_type)
        .with_column("val14", int32_type)
        .with_column("val15", int32_type)
        .with_column("val16", int32_type)
        .with_column("val17", int32_type)
        .with_column("val18", int32_type)
        .with_column("val19", int32_type)
        .with_column("val20", int32_type)
        .with_column("val21", int32_type)
        .with_column("val22", int32_type)
        .with_column("val23", int32_type)
        .with_column("val24", int32_type)
        .with_column("val25", int32_type)
        .with_column("val26", int32_type)
        .with_column("val27", int32_type)
        .with_column("val28", int32_type)
        .with_column("val29", int32_type)
        .with_column("val30", int32_type)
        .with_column("val31", int32_type)
        .with_column("val32", int32_type)
        .with_column("val33", int32_type)
        .with_column("val34", int32_type)
        .with_column("val35", int32_type)
        .with_column("val36", int32_type)
        .with_column("val37", int32_type)
        .with_column("val38", int32_type)
        .with_column("val39", int32_type)
        .with_column("val40", int32_type)
        .with_column("val41", int32_type)
        .with_column("val42", int32_type)
        .with_column("val43", int32_type)
        .with_column("val44", int32_type)
        .with_column("val45", int32_type)
        .with_column("val46", int32_type)
        .with_column("val47", int32_type)
        .with_column("val48", int32_type)
        .with_column("val49", int32_type)
        .with_column("val50", int32_type)
        .with_column("val51", int32_type)
        .with_column("val52", int32_type)
        .with_column("val53", int32_type)
        .with_column("val54", int32_type)
        .with_column("val55", int32_type)
        .with_column("val56", int32_type)
        .with_column("val57", int32_type)
        .with_column("val58", int32_type)
        .with_column("val59", int32_type)
        .with_column("val60", int32_type)
        .with_column("val61", int32_type)
        .with_column("val62", int32_type)
        .with_column("val63", int32_type)
        .with_column("val64", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_large_subset_of_columns_sparse_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_SPARSE_SCHEMA,
                           UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_SPARSE_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_SPARSE_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_SPARSE_SCHEMA, pk);
    };

    std::vector<const column_definition*> column_defs(64);
    for (int i = 0; i < 64; ++i) {
        column_defs[i] = UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_SPARSE_SCHEMA->get_column_definition(to_bytes(sprint("val%d", (i + 1))));
        BOOST_REQUIRE(column_defs[i]);
    }

    auto generate = [&] (const std::vector<std::pair<int, int>>& column_values) {
        std::vector<flat_reader_assertions::expected_column> columns;

        for (auto& p : column_values) {
            columns.push_back({column_defs[p.first - 1], int32_type->decompose(p.second)});
        }

        return columns;
    };

    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{34, 1}, {35, 2}, {36, 3}, {37, 4}, {38, 5}, {39, 6}, {40, 7}, {41, 8}, {42, 9},
                                {43, 10}, {44, 11}, {45, 12}, {46, 13}, {47, 14}, {48, 15}, {49, 16}, {50, 17},
                                {51, 18}, {52, 19}, {53, 20}, {54, 21}, {55, 22}, {56, 23}, {57, 24}, {58, 25},
                                {59, 26}, {60, 27}, {61, 28}, {62, 29}, {63, 30}, {64, 31}}))
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{1, 11}}))
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{2, 22}, {5, 222}, {6, 2222}, {7, 22222}, {60, 222222}}))
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}, {8, 8}, {9, 9},
                                {10, 10}, {11, 11}, {12, 12}, {13, 13}, {14, 14}, {15, 15}, {16, 16}, {17, 17},
                                {18, 18}, {19, 19}, {20, 20}, {21, 21}, {22, 22}, {23, 23}, {24, 24}, {25, 25},
                                {26, 26}, {27, 27}, {28, 28}, {29, 29}, {30, 30}, {31, 31}}))
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{32, 33}, {33, 333}}))
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/large_subset_of_columns_dense
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT,
//                                   val1 INT, val2 INT, val3 INT, val4 INT, val5 INT, val6 INT, val7 INT, val8 INT,
//                                   val9 INT,
//                                   val10 INT, val11 INT, val12 INT, val13 INT, val14 INT, val15 INT, val16 INT,
//                                   val17 INT, val18 INT, val19 INT,
//                                   val20 INT, val21 INT, val22 INT, val23 INT, val24 INT, val25 INT, val26 INT,
//                                   val27 INT, val28 INT, val29 INT,
//                                   val30 INT, val31 INT, val32 INT, val33 INT, val34 INT, val35 INT, val36 INT,
//                                   val37 INT, val38 INT, val39 INT,
//                                   val40 INT, val41 INT, val42 INT, val43 INT, val44 INT, val45 INT, val46 INT,
//                                   val47 INT, val48 INT, val49 INT,
//                                   val50 INT, val51 INT, val52 INT, val53 INT, val54 INT, val55 INT, val56 INT,
//                                   val57 INT, val58 INT, val59 INT,
//                                   val60 INT, val61 INT, val62 INT, val63 INT, val64 INT,
//                                   PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, val1, val2, val3, val4, val5, val6, val7, val8, val9,
//                                    val10, val11, val12, val13, val14, val15, val16, val17, val18, val19,
//                                    val20, val21, val22, val23, val24, val25, val26, val27, val28,
//                                    val30, val31, val32, val33, val34, val35, val36, val37, val38, val39,
//                                    val40, val41, val42, val43, val44, val45, val46, val47, val48, val49,
//                                    val50, val51, val52, val53, val54, val55, val56, val57, val58, val59,
//                                    val60, val61, val62, val63, val64)
//                         VALUES(1, 1, 2, 3, 4, 5, 6, 7, 8, 9,
//                                   10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
//                                   20, 21, 22, 23, 24, 25, 26, 27, 28,
//                                   30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
//                                   40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
//                                   50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
//                                   60, 61, 62, 63, 64);
// INSERT INTO test_ks.test_table(pk, val2, val3, val4, val5, val6, val7, val8, val9,
//                                    val10, val11, val12, val13, val14, val15, val16, val17, val18, val19,
//                                    val20, val21, val22, val23, val24, val25, val26, val27, val28, val29,
//                                    val30, val31, val32, val33, val34, val35, val36, val37, val38, val39,
//                                    val40, val41, val42, val43, val44, val45, val46, val47, val48, val49,
//                                    val50, val51, val52, val53, val54, val55, val56, val57, val58, val59,
//                                    val60, val61, val62, val63, val64)
//                         VALUES(2, 2, 3, 4, 5, 6, 7, 8, 9,
//                                   10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
//                                   20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
//                                   30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
//                                   40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
//                                   50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
//                                   60, 61, 62, 63, 64);
// INSERT INTO test_ks.test_table(pk, val1, val2, val3, val4, val5, val6, val7, val8, val9,
//                                    val10, val11, val12, val13, val14, val15, val16, val17, val18, val19,
//                                    val20, val21, val22, val23, val24, val25, val26, val27, val28, val29,
//                                    val30, val31, val32, val33, val34, val35, val36, val37, val38, val39,
//                                    val40, val41, val42, val43, val44, val45, val46, val47, val48, val49,
//                                    val50, val51, val52, val53, val54, val55, val56, val57, val58, val59,
//                                    val60, val61, val62, val63)
//                         VALUES(3, 1, 2, 3, 4, 5, 6, 7, 8, 9,
//                                   10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
//                                   20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
//                                   30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
//                                   40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
//                                   50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
//                                   60, 61, 62, 63);
// INSERT INTO test_ks.test_table(pk, val1, val2, val3, val4, val5, val6, val7, val8, val9,
//                                    val10, val11, val12, val13, val14, val15, val16, val17, val18, val19,
//                                    val20, val21, val22, val23, val24, val25, val26, val27, val28, val29,
//                                    val30, val31, val32)
//                         VALUES(4, 1, 2, 3, 4, 5, 6, 7, 8, 9,
//                                   10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
//                                   20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
//                                   30, 31, 32);
// INSERT INTO test_ks.test_table(pk, val33, val34, val35, val36, val37, val38, val39, val40, val41, val42,
//                                    val43, val44, val45, val46, val47, val48, val49, val50, val51, val52,
//                                    val53, val54, val55, val56, val57, val58, val59, val60, val61, val62,
//                                    val63, val64)
//                         VALUES(5, 1, 2, 3, 4, 5, 6, 7, 8, 9,
//                                   10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
//                                   20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
//                                   30, 31, 32);
//
// IMPORTANT: each column has to be covered by at least one insert otherwise it won't be present in the sstable

static thread_local const sstring UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_DENSE_PATH =
    "tests/sstables/3.x/uncompressed/large_subset_of_columns_dense";
static thread_local const schema_ptr UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_DENSE_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("val1", int32_type)
        .with_column("val2", int32_type)
        .with_column("val3", int32_type)
        .with_column("val4", int32_type)
        .with_column("val5", int32_type)
        .with_column("val6", int32_type)
        .with_column("val7", int32_type)
        .with_column("val8", int32_type)
        .with_column("val9", int32_type)
        .with_column("val10", int32_type)
        .with_column("val11", int32_type)
        .with_column("val12", int32_type)
        .with_column("val13", int32_type)
        .with_column("val14", int32_type)
        .with_column("val15", int32_type)
        .with_column("val16", int32_type)
        .with_column("val17", int32_type)
        .with_column("val18", int32_type)
        .with_column("val19", int32_type)
        .with_column("val20", int32_type)
        .with_column("val21", int32_type)
        .with_column("val22", int32_type)
        .with_column("val23", int32_type)
        .with_column("val24", int32_type)
        .with_column("val25", int32_type)
        .with_column("val26", int32_type)
        .with_column("val27", int32_type)
        .with_column("val28", int32_type)
        .with_column("val29", int32_type)
        .with_column("val30", int32_type)
        .with_column("val31", int32_type)
        .with_column("val32", int32_type)
        .with_column("val33", int32_type)
        .with_column("val34", int32_type)
        .with_column("val35", int32_type)
        .with_column("val36", int32_type)
        .with_column("val37", int32_type)
        .with_column("val38", int32_type)
        .with_column("val39", int32_type)
        .with_column("val40", int32_type)
        .with_column("val41", int32_type)
        .with_column("val42", int32_type)
        .with_column("val43", int32_type)
        .with_column("val44", int32_type)
        .with_column("val45", int32_type)
        .with_column("val46", int32_type)
        .with_column("val47", int32_type)
        .with_column("val48", int32_type)
        .with_column("val49", int32_type)
        .with_column("val50", int32_type)
        .with_column("val51", int32_type)
        .with_column("val52", int32_type)
        .with_column("val53", int32_type)
        .with_column("val54", int32_type)
        .with_column("val55", int32_type)
        .with_column("val56", int32_type)
        .with_column("val57", int32_type)
        .with_column("val58", int32_type)
        .with_column("val59", int32_type)
        .with_column("val60", int32_type)
        .with_column("val61", int32_type)
        .with_column("val62", int32_type)
        .with_column("val63", int32_type)
        .with_column("val64", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_large_subset_of_columns_dense_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_DENSE_SCHEMA,
                           UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_DENSE_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_DENSE_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_DENSE_SCHEMA, pk);
    };

    std::vector<const column_definition*> column_defs(64);
    for (int i = 0; i < 64; ++i) {
        column_defs[i] = UNCOMPRESSED_LARGE_SUBSET_OF_COLUMNS_DENSE_SCHEMA->get_column_definition(to_bytes(sprint("val%d", (i + 1))));
        BOOST_REQUIRE(column_defs[i]);
    }

    auto generate = [&] (const std::vector<std::pair<int, int>>& column_values) {
        std::vector<flat_reader_assertions::expected_column> columns;

        for (auto& p : column_values) {
            columns.push_back({column_defs[p.first - 1], int32_type->decompose(p.second)});
        }

        return columns;
    };

    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{33, 1}, {34, 2}, {35, 3}, {36, 4}, {37, 5}, {38, 6}, {39, 7}, {40, 8}, {41, 9},
                                {42, 10}, {43, 11}, {44, 12}, {45, 13}, {46, 14}, {47, 15}, {48, 16}, {49, 17},
                                {50, 18}, {51, 19}, {52, 20}, {53, 21}, {54, 22}, {55, 23}, {56, 24}, {57, 25},
                                {58, 26}, {59, 27}, {60, 28}, {61, 29}, {62, 30}, {63, 31}, {64, 32}}))
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}, {8, 8}, {9, 9},
                                {10, 10}, {11, 11}, {12, 12}, {13, 13}, {14, 14}, {15, 15}, {16, 16}, {17, 17},
                                {18, 18}, {19, 19}, {20, 20}, {21, 21}, {22, 22}, {23, 23}, {24, 24}, {25, 25},
                                {26, 26}, {27, 27}, {28, 28}, {30, 30}, {31, 31}, {32, 32}, {33, 33},
                                {34, 34}, {35, 35}, {36, 36}, {37, 37}, {38, 38}, {39, 39}, {40, 40}, {41, 41},
                                {42, 42}, {43, 43}, {44, 44}, {45, 45}, {46, 46}, {47, 47}, {48, 48}, {49, 49},
                                {50, 50}, {51, 51}, {52, 52}, {53, 53}, {54, 54}, {55, 55}, {56, 56}, {57, 57},
                                {58, 58}, {59, 59}, {60, 60}, {61, 61}, {62, 62}, {63, 63}, {64, 64}}))
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}, {8, 8}, {9, 9},
                                {10, 10}, {11, 11}, {12, 12}, {13, 13}, {14, 14}, {15, 15}, {16, 16}, {17, 17},
                                {18, 18}, {19, 19}, {20, 20}, {21, 21}, {22, 22}, {23, 23}, {24, 24}, {25, 25},
                                {26, 26}, {27, 27}, {28, 28}, {29, 29}, {30, 30}, {31, 31}, {32, 32}, {33, 33},
                                {34, 34}, {35, 35}, {36, 36}, {37, 37}, {38, 38}, {39, 39}, {40, 40}, {41, 41},
                                {42, 42}, {43, 43}, {44, 44}, {45, 45}, {46, 46}, {47, 47}, {48, 48}, {49, 49},
                                {50, 50}, {51, 51}, {52, 52}, {53, 53}, {54, 54}, {55, 55}, {56, 56}, {57, 57},
                                {58, 58}, {59, 59}, {60, 60}, {61, 61}, {62, 62}, {63, 63}, {64, 64}}))
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}, {8, 8}, {9, 9},
                                {10, 10}, {11, 11}, {12, 12}, {13, 13}, {14, 14}, {15, 15}, {16, 16}, {17, 17},
                                {18, 18}, {19, 19}, {20, 20}, {21, 21}, {22, 22}, {23, 23}, {24, 24}, {25, 25},
                                {26, 26}, {27, 27}, {28, 28}, {29, 29}, {30, 30}, {31, 31}, {32, 32}}))
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row(clustering_key_prefix::make_empty(),
                      generate({{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}, {8, 8}, {9, 9},
                                {10, 10}, {11, 11}, {12, 12}, {13, 13}, {14, 14}, {15, 15}, {16, 16}, {17, 17},
                                {18, 18}, {19, 19}, {20, 20}, {21, 21}, {22, 22}, {23, 23}, {24, 24}, {25, 25},
                                {26, 26}, {27, 27}, {28, 28}, {29, 29}, {30, 30}, {31, 31}, {32, 32}, {33, 33},
                                {34, 34}, {35, 35}, {36, 36}, {37, 37}, {38, 38}, {39, 39}, {40, 40}, {41, 41},
                                {42, 42}, {43, 43}, {44, 44}, {45, 45}, {46, 46}, {47, 47}, {48, 48}, {49, 49},
                                {50, 50}, {51, 51}, {52, 52}, {53, 53}, {54, 54}, {55, 55}, {56, 56}, {57, 57},
                                {58, 58}, {59, 59}, {60, 60}, {61, 61}, {62, 62}, {63, 63}}))
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/deleted_cells
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, ck INT, val INT, PRIMARY KEY(pk, ck))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 101, 1001);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 102, 1002);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 103, 1003);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 104, 1004);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 105, 1005);
// DELETE val FROM test_ks.test_table WHERE pk = 1 AND ck = 102;
// DELETE val FROM test_ks.test_table WHERE pk = 1 AND ck = 104;

static thread_local const sstring UNCOMPRESSED_DELETED_CELLS_PATH = "tests/sstables/3.x/uncompressed/deleted_cells";
static thread_local const schema_ptr UNCOMPRESSED_DELETED_CELLS_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_deleted_cells_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_DELETED_CELLS_SCHEMA, UNCOMPRESSED_DELETED_CELLS_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_DELETED_CELLS_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_DELETED_CELLS_SCHEMA, pk);
    };

    auto int_cdef =
    UNCOMPRESSED_DELETED_CELLS_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(int_cdef);

    auto generate = [&] (uint64_t timestamp, uint64_t deletion_time) {
        std::vector<flat_reader_assertions::assert_function> assertions;

        assertions.push_back([timestamp, deletion_time] (const column_definition& def,
                                 const atomic_cell_or_collection* cell) {
            auto c = cell->as_atomic_cell(def);
            BOOST_REQUIRE(!c.is_live());
            BOOST_REQUIRE_EQUAL(timestamp, c.timestamp());
            BOOST_REQUIRE_EQUAL(deletion_time, c.deletion_time().time_since_epoch().count());
        });

        return assertions;
    };

    std::vector<column_id> ids{int_cdef->id};

    assert_that(sst.read_rows_flat())
    .produces_partition_start(to_key(1))
    .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_DELETED_CELLS_SCHEMA,
                  int32_type->decompose(101)),
                  {{int_cdef, int32_type->decompose(1001)}})
    .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_DELETED_CELLS_SCHEMA,
                  int32_type->decompose(102)),
                  ids, generate(1529586065552460, 1529586065))
    .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_DELETED_CELLS_SCHEMA,
                  int32_type->decompose(103)),
                  {{int_cdef, int32_type->decompose(1003)}})
    .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_DELETED_CELLS_SCHEMA,
                  int32_type->decompose(104)),
                  ids, generate(1529586067568210, 1529586067))
    .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_DELETED_CELLS_SCHEMA,
                  int32_type->decompose(105)),
                  {{int_cdef, int32_type->decompose(1005)}})
    .produces_partition_end()
    .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/range_tombstones_simple
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, ck INT, val INT, PRIMARY KEY(pk, ck))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 101, 1001);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 102, 1002);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 103, 1003);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 104, 1004);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 105, 1005);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 106, 1006);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 107, 1007);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 108, 1008);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 109, 1009);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 110, 1010);
// DELETE FROM test_ks.test_table WHERE pk = 1 AND ck > 101 AND ck < 104;
// DELETE FROM test_ks.test_table WHERE pk = 1 AND ck >= 104 AND ck < 105;
// DELETE FROM test_ks.test_table WHERE pk = 1 AND ck > 108;

static thread_local const sstring UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_PATH = "tests/sstables/3.x/uncompressed/range_tombstones_simple";
static thread_local const schema_ptr UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_range_tombstones_simple_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_SCHEMA,
                           UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_SCHEMA, pk);
    };

    auto to_ck = [] (int ck) {
        return clustering_key::from_single_value(*UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_SCHEMA,
                                                 int32_type->decompose(ck));
    };

    auto int_cdef =
    UNCOMPRESSED_RANGE_TOMBSTONES_SIMPLE_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(int_cdef);


    assert_that(sst.read_rows_flat())
    .produces_partition_start(to_key(1))
    .produces_row(to_ck(101), {{int_cdef, int32_type->decompose(1001)}})
    .produces_range_tombstone(range_tombstone(to_ck(101),
                                              bound_kind::excl_start,
                                              to_ck(104),
                                              bound_kind::excl_end,
                                              tombstone(api::timestamp_type{1529519641211958},
                                                        gc_clock::time_point(gc_clock::duration(1529519641)))))
    .produces_range_tombstone(range_tombstone(to_ck(104),
                                              bound_kind::incl_start,
                                              to_ck(105),
                                              bound_kind::excl_end,
                                              tombstone(api::timestamp_type{1529519641215380},
                                                        gc_clock::time_point(gc_clock::duration(1529519641)))))
    .produces_row(to_ck(105), {{int_cdef, int32_type->decompose(1005)}})
    .produces_row(to_ck(106), {{int_cdef, int32_type->decompose(1006)}})
    .produces_row(to_ck(107), {{int_cdef, int32_type->decompose(1007)}})
    .produces_row(to_ck(108), {{int_cdef, int32_type->decompose(1008)}})
    .produces_range_tombstone(range_tombstone(to_ck(108),
                                              bound_kind::excl_start,
                                              clustering_key_prefix::make_empty(),
                                              bound_kind::incl_end,
                                              tombstone(api::timestamp_type{1529519643267068},
                                                        gc_clock::time_point(gc_clock::duration(1529519643)))))
    .produces_partition_end()
    .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/range_tombstones_partial
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, ck1 INT, ck2 INT, PRIMARY KEY(pk, ck1, ck2))
//      WITH compression = { 'enabled' : false };
//
// DELETE FROM test_ks.test_table WHERE pk = 1 AND ck1 > 1 AND ck1 < 3;
// INSERT INTO test_ks.test_table(pk, ck1, ck2) VALUES(1, 2, 13);
// DELETE FROM test_ks.test_table WHERE pk = 1 AND ck1 > 3;

static thread_local const sstring UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_PATH = "tests/sstables/3.x/uncompressed/range_tombstones_partial";
static thread_local const schema_ptr UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck1", int32_type, column_kind::clustering_key)
        .with_column("ck2", int32_type, column_kind::clustering_key)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_range_tombstones_partial_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA,
                           UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA, pk);
    };

    auto to_ck = [] (int ck) {
        return clustering_key::from_single_value(*UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA,
                                                 int32_type->decompose(ck));
    };

    assert_that(sst.read_rows_flat())
    .produces_partition_start(to_key(1))
    .produces_range_tombstone(range_tombstone(to_ck(1),
                              bound_kind::excl_start,
                              clustering_key::from_exploded(*UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA, {
                                  int32_type->decompose(2),
                                  int32_type->decompose(13)
                              }),
                              bound_kind::incl_end,
                              tombstone(api::timestamp_type{1530543711595401},
                              gc_clock::time_point(gc_clock::duration(1530543711)))))
    .produces_row_with_key(clustering_key::from_exploded(*UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA, {
                                                            int32_type->decompose(2),
                                                            int32_type->decompose(13)
                                                        }))
    .produces_range_tombstone(range_tombstone(clustering_key::from_exploded(*UNCOMPRESSED_RANGE_TOMBSTONES_PARTIAL_SCHEMA, {
                                  int32_type->decompose(2),
                                  int32_type->decompose(13)
                              }),
                              bound_kind::incl_start,
                              to_ck(3),
                              bound_kind::excl_end,
                              tombstone(api::timestamp_type{1530543711595401},
                              gc_clock::time_point(gc_clock::duration(1530543711)))))
    .produces_range_tombstone(range_tombstone(to_ck(3),
                              bound_kind::excl_start,
                              clustering_key_prefix::make_empty(),
                              bound_kind::incl_end,
                              tombstone(api::timestamp_type{1530543761322213},
                              gc_clock::time_point(gc_clock::duration(1530543761)))))
    .produces_partition_end()
    .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/simple
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, ck INT, val INT, PRIMARY KEY(pk, ck))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(1, 101, 1001);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(2, 102, 1002);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(3, 103, 1003);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(4, 104, 1004);
// INSERT INTO test_ks.test_table(pk, ck, val) VALUES(5, 105, 1005);

static thread_local const sstring UNCOMPRESSED_SIMPLE_PATH = "tests/sstables/3.x/uncompressed/simple";
static thread_local const schema_ptr UNCOMPRESSED_SIMPLE_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck", int32_type, column_kind::clustering_key)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_simple_read_toc) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SIMPLE_SCHEMA, UNCOMPRESSED_SIMPLE_PATH);
    sst.read_toc();
    using ct = component_type;
    sst.assert_toc({ct::Index,
                    ct::Data,
                    ct::TOC,
                    ct::Summary,
                    ct::Digest,
                    ct::CRC,
                    ct::Filter,
                    ct::Statistics});
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_simple_read_summary) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SIMPLE_SCHEMA, UNCOMPRESSED_SIMPLE_PATH);
    sst.read_toc();
    sst.read_summary();
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_simple_read_filter) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SIMPLE_SCHEMA, UNCOMPRESSED_SIMPLE_PATH);
    sst.read_toc();
    sst.read_filter();
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_simple_read_statistics) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SIMPLE_SCHEMA, UNCOMPRESSED_SIMPLE_PATH);
    sst.read_toc();
    sst.read_statistics();
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_simple_load) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SIMPLE_SCHEMA, UNCOMPRESSED_SIMPLE_PATH);
    sst.load();
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_simple_read_index) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SIMPLE_SCHEMA, UNCOMPRESSED_SIMPLE_PATH);
    auto vec = sst.read_index().get0();
    BOOST_REQUIRE_EQUAL(5, vec.size());
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_simple_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_SIMPLE_SCHEMA,
                           UNCOMPRESSED_SIMPLE_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_SIMPLE_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_SIMPLE_SCHEMA, pk);
    };

    auto int_cdef =
        UNCOMPRESSED_SIMPLE_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(int_cdef);


    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_SIMPLE_SCHEMA,
                                                        int32_type->decompose(105)),
                      {{int_cdef, int32_type->decompose(1005)}})
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_SIMPLE_SCHEMA,
                                                        int32_type->decompose(101)),
                      {{int_cdef, int32_type->decompose(1001)}})
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_SIMPLE_SCHEMA,
                                                        int32_type->decompose(102)),
                      {{int_cdef, int32_type->decompose(1002)}})
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_SIMPLE_SCHEMA,
                                                        int32_type->decompose(104)),
                      {{int_cdef, int32_type->decompose(1004)}})
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row(clustering_key::from_single_value(*UNCOMPRESSED_SIMPLE_SCHEMA,
                                                        int32_type->decompose(103)),
                      {{int_cdef, int32_type->decompose(1003)}})
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/compound_ck
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT,
//                                   ck_int INT,
//                                   ck_text TEXT,
//                                   ck_uuid UUID,
//                                   ck_inet INET,
//                                   val INT,
//                                   PRIMARY KEY(pk, ck_int, ck_text, ck_uuid, ck_inet))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, ck_int, ck_text, ck_uuid, ck_inet, val)
//        VALUES(1, 101, 'This is a string for 1', f7fdcbd2-4544-482c-85fd-d9572adc3cd6, '10.0.0.1', 1001);
// INSERT INTO test_ks.test_table(pk, ck_int, ck_text, ck_uuid, ck_inet, val)
//        VALUES(2, 102, 'This is a string for 2', c25ae960-07a2-467d-8f35-5bd38647b367, '10.0.0.2', 1002);
// INSERT INTO test_ks.test_table(pk, ck_int, ck_text, ck_uuid, ck_inet, val)
//        VALUES(3, 103, 'This is a string for 3', f7e8ebc0-dbae-4c06-bae0-656c23f6af6a, '10.0.0.3', 1003);
// INSERT INTO test_ks.test_table(pk, ck_int, ck_text, ck_uuid, ck_inet, val)
//        VALUES(4, 104, 'This is a string for 4', 4549e2c2-786e-4b30-90aa-5dd37ae1db8f, '10.0.0.4', 1004);
// INSERT INTO test_ks.test_table(pk, ck_int, ck_text, ck_uuid, ck_inet, val)
//        VALUES(5, 105, 'This is a string for 5',  f1badb6f-80a0-4eef-90df-b3651d9a5578, '10.0.0.5', 1005);

static thread_local const sstring UNCOMPRESSED_COMPOUND_CK_PATH = "tests/sstables/3.x/uncompressed/compound_ck";
static thread_local const schema_ptr UNCOMPRESSED_COMPOUND_CK_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("ck_int", int32_type, column_kind::clustering_key)
        .with_column("ck_text", utf8_type, column_kind::clustering_key)
        .with_column("ck_uuid", uuid_type, column_kind::clustering_key)
        .with_column("ck_inet", inet_addr_type, column_kind::clustering_key)
        .with_column("val", int32_type)
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_compound_ck_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_COMPOUND_CK_SCHEMA,
                           UNCOMPRESSED_COMPOUND_CK_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_COMPOUND_CK_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_COMPOUND_CK_SCHEMA, pk);
    };

    auto int_cdef =
        UNCOMPRESSED_SIMPLE_SCHEMA->get_column_definition(to_bytes("val"));
    BOOST_REQUIRE(int_cdef);


    assert_that(sst.read_rows_flat())
        .produces_partition_start(to_key(5))
        .produces_row(clustering_key::from_exploded(*UNCOMPRESSED_SIMPLE_SCHEMA, {
                          int32_type->decompose(105),
                          utf8_type->from_string("This is a string for 5"),
                          uuid_type->from_string("f1badb6f-80a0-4eef-90df-b3651d9a5578"),
                          inet_addr_type->from_string("10.0.0.5")
                      }),
                      {{int_cdef, int32_type->decompose(1005)}})
        .produces_partition_end()
        .produces_partition_start(to_key(1))
        .produces_row(clustering_key::from_exploded(*UNCOMPRESSED_SIMPLE_SCHEMA, {
                          int32_type->decompose(101),
                          utf8_type->from_string("This is a string for 1"),
                          uuid_type->from_string("f7fdcbd2-4544-482c-85fd-d9572adc3cd6"),
                          inet_addr_type->from_string("10.0.0.1")
                      }),
                      {{int_cdef, int32_type->decompose(1001)}})
        .produces_partition_end()
        .produces_partition_start(to_key(2))
        .produces_row(clustering_key::from_exploded(*UNCOMPRESSED_SIMPLE_SCHEMA, {
                          int32_type->decompose(102),
                          utf8_type->from_string("This is a string for 2"),
                          uuid_type->from_string("c25ae960-07a2-467d-8f35-5bd38647b367"),
                          inet_addr_type->from_string("10.0.0.2")
                      }),
                      {{int_cdef, int32_type->decompose(1002)}})
        .produces_partition_end()
        .produces_partition_start(to_key(4))
        .produces_row(clustering_key::from_exploded(*UNCOMPRESSED_SIMPLE_SCHEMA, {
                          int32_type->decompose(104),
                          utf8_type->from_string("This is a string for 4"),
                          uuid_type->from_string("4549e2c2-786e-4b30-90aa-5dd37ae1db8f"),
                          inet_addr_type->from_string("10.0.0.4")
                      }),
                      {{int_cdef, int32_type->decompose(1004)}})
        .produces_partition_end()
        .produces_partition_start(to_key(3))
        .produces_row(clustering_key::from_exploded(*UNCOMPRESSED_SIMPLE_SCHEMA, {
                          int32_type->decompose(103),
                          utf8_type->from_string("This is a string for 3"),
                          uuid_type->from_string("f7e8ebc0-dbae-4c06-bae0-656c23f6af6a"),
                          inet_addr_type->from_string("10.0.0.3")
                      }),
                      {{int_cdef, int32_type->decompose(1003)}})
        .produces_partition_end()
        .produces_end_of_stream();
}

// Following tests run on files in tests/sstables/3.x/uncompressed/collections
// They were created using following CQL statements:
//
// CREATE KEYSPACE test_ks WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1};
//
// CREATE TABLE test_ks.test_table ( pk INT, set_val set<int>, list_val list<text>, map_val map<int, text>, PRIMARY KEY(pk))
//      WITH compression = { 'enabled' : false };
//
// INSERT INTO test_ks.test_table(pk, set_val, list_val, map_val)
//                         VALUES(1, {1, 2, 3}, ['Text 1', 'Text 2', 'Text 3'], {1 : 'Text 1', 2 : 'Text 2', 3 : 'Text 3'});
// INSERT INTO test_ks.test_table(pk, set_val, list_val, map_val)
//                         VALUES(2, {4, 5, 6}, ['Text 4', 'Text 5', 'Text 6'], {4 : 'Text 4', 5 : 'Text 5', 6 : 'Text 6'});
// INSERT INTO test_ks.test_table(pk, set_val, list_val, map_val)
//                         VALUES(3, {7, 8, 9}, ['Text 7', 'Text 8', 'Text 9'], {7 : 'Text 7', 8 : 'Text 8', 9 : 'Text 9'});
// INSERT INTO test_ks.test_table(pk, set_val, list_val, map_val)
//                         VALUES(4, {10, 11, 12}, ['Text 10', 'Text 11', 'Text 12'], {10 : 'Text 10', 11 : 'Text 11', 12 : 'Text 12'});
// INSERT INTO test_ks.test_table(pk, set_val, list_val, map_val)
//                         VALUES(5, {13, 14, 15}, ['Text 13', 'Text 14', 'Text 15'], {13 : 'Text 13', 14 : 'Text 14', 15 : 'Text 15'});

static thread_local const sstring UNCOMPRESSED_COLLECTIONS_PATH =
    "tests/sstables/3.x/uncompressed/collections";
static thread_local const schema_ptr UNCOMPRESSED_COLLECTIONS_SCHEMA =
    schema_builder("test_ks", "test_table")
        .with_column("pk", int32_type, column_kind::partition_key)
        .with_column("set_val", set_type_impl::get_instance(int32_type, true))
        .with_column("list_val", list_type_impl::get_instance(utf8_type, true))
        .with_column("map_val", map_type_impl::get_instance(int32_type, utf8_type, true))
        .set_compressor_params(compression_parameters::no_compression())
        .build();

SEASTAR_THREAD_TEST_CASE(test_uncompressed_collections_read) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstable_assertions sst(UNCOMPRESSED_COLLECTIONS_SCHEMA, UNCOMPRESSED_COLLECTIONS_PATH);
    sst.load();
    auto to_key = [] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*UNCOMPRESSED_COLLECTIONS_SCHEMA, bytes);
        return dht::global_partitioner().decorate_key(*UNCOMPRESSED_COLLECTIONS_SCHEMA, pk);
    };

    auto set_cdef = UNCOMPRESSED_COLLECTIONS_SCHEMA->get_column_definition(to_bytes("set_val"));
    BOOST_REQUIRE(set_cdef);

    auto list_cdef = UNCOMPRESSED_COLLECTIONS_SCHEMA->get_column_definition(to_bytes("list_val"));
    BOOST_REQUIRE(list_cdef);

    auto map_cdef = UNCOMPRESSED_COLLECTIONS_SCHEMA->get_column_definition(to_bytes("map_val"));
    BOOST_REQUIRE(map_cdef);

    auto generate = [&] (std::vector<int> set_val, std::vector<sstring> list_val, std::vector<std::pair<int, sstring>> map_val) {
        std::vector<flat_reader_assertions::assert_function> assertions;

        assertions.push_back([val = std::move(set_val)] (const column_definition& def,
                                                         const atomic_cell_or_collection* cell) {
            BOOST_REQUIRE(def.is_multi_cell());
            auto ctype = static_pointer_cast<const collection_type_impl>(def.type);
            int idx = 0;
            cell->as_collection_mutation().data.with_linearized([&] (bytes_view c_bv) {
                auto m_view = ctype->deserialize_mutation_form(c_bv);
                for (auto&& entry : m_view.cells) {
                    auto cmp = compare_unsigned(int32_type->decompose(int32_t(val[idx])), entry.first);
                    if (cmp != 0) {
                        BOOST_FAIL(sprint("Expected row with column %s having value %s, but it has value %s",
                                          def.id,
                                          int32_type->decompose(int32_t(val[idx])),
                                          entry.first));
                    }
                    ++idx;
                }
            });
        });

        assertions.push_back([val = std::move(list_val)] (const column_definition& def,
                                                          const atomic_cell_or_collection* cell) {
            BOOST_REQUIRE(def.is_multi_cell());
            auto ctype = static_pointer_cast<const collection_type_impl>(def.type);
            int idx = 0;
            cell->as_collection_mutation().data.with_linearized([&] (bytes_view c_bv) {
                auto m_view = ctype->deserialize_mutation_form(c_bv);
                for (auto&& entry : m_view.cells) {
                    auto cmp = compare_unsigned(utf8_type->decompose(val[idx]), entry.second.value().linearize());
                    if (cmp != 0) {
                        BOOST_FAIL(sprint("Expected row with column %s having value %s, but it has value %s",
                                          def.id,
                                          utf8_type->decompose(val[idx]),
                                          entry.second.value().linearize()));
                    }
                    ++idx;
                }
            });
        });

        assertions.push_back([val = std::move(map_val)] (const column_definition& def,
                                                         const atomic_cell_or_collection* cell) {
            BOOST_REQUIRE(def.is_multi_cell());
            auto ctype = static_pointer_cast<const collection_type_impl>(def.type);
            int idx = 0;
            cell->as_collection_mutation().data.with_linearized([&] (bytes_view c_bv) {
                auto m_view = ctype->deserialize_mutation_form(c_bv);
                for (auto&& entry : m_view.cells) {
                    auto cmp1 = compare_unsigned(int32_type->decompose(int32_t(val[idx].first)), entry.first);
                    auto cmp2 = compare_unsigned(utf8_type->decompose(val[idx].second), entry.second.value().linearize());
                    if (cmp1 != 0 || cmp2 != 0) {
                        BOOST_FAIL(
                            sprint("Expected row with column %s having value (%s, %s), but it has value (%s, %s)",
                                   def.id,
                                   int32_type->decompose(int32_t(val[idx].first)),
                                   utf8_type->decompose(val[idx].second),
                                   entry.first,
                                   entry.second.value().linearize()));
                    }
                    ++idx;
                }
            });
        });

        return std::move(assertions);
    };

    std::vector<column_id> ids{set_cdef->id, list_cdef->id, map_cdef->id};

    assert_that(sst.read_rows_flat())
    .produces_partition_start(to_key(5))
    .produces_row(clustering_key_prefix::make_empty(), ids,
            generate({13, 14, 15}, {"Text 13", "Text 14", "Text 15"}, {{13,"Text 13"}, {14,"Text 14"}, {15,"Text 15"}}))
    .produces_partition_end()
    .produces_partition_start(to_key(1))
    .produces_row(clustering_key_prefix::make_empty(), ids,
            generate({1, 2, 3}, {"Text 1", "Text 2", "Text 3"}, {{1,"Text 1"}, {2,"Text 2"}, {3,"Text 3"}}))
    .produces_partition_end()
    .produces_partition_start(to_key(2))
    .produces_row(clustering_key_prefix::make_empty(), ids,
            generate({4, 5, 6}, {"Text 4", "Text 5", "Text 6"}, {{4,"Text 4"}, {5,"Text 5"}, {6, "Text 6"}}))
    .produces_partition_end()
    .produces_partition_start(to_key(4))
    .produces_row(clustering_key_prefix::make_empty(), ids,
            generate({10, 11, 12}, {"Text 10", "Text 11", "Text 12"}, {{10,"Text 10"}, {11,"Text 11"}, {12,"Text 12"}}))
    .produces_partition_end()
    .produces_partition_start(to_key(3))
    .produces_row(clustering_key_prefix::make_empty(), ids,
            generate({7, 8, 9}, {"Text 7", "Text 8", "Text 9"}, {{7,"Text 7"}, {8,"Text 8"}, {9,"Text 9"}}))
    .produces_partition_end()
    .produces_end_of_stream();
}

static sstables::shared_sstable open_sstable(schema_ptr schema, sstring dir, unsigned long generation) {
    auto sst = sstables::make_sstable(std::move(schema), dir, generation,
            sstables::sstable::version_types::mc,
            sstables::sstable::format_types::big);
    sst->load().get();
    return sst;
}

static std::vector<sstables::shared_sstable> open_sstables(schema_ptr s, sstring dir, std::vector<unsigned long> generations) {
    std::vector<sstables::shared_sstable> result;
    for(auto generation: generations) {
        result.push_back(open_sstable(s, dir, generation));
    }
    return result;
}

static flat_mutation_reader compacted_sstable_reader(schema_ptr s,
                     sstring table_name, std::vector<unsigned long> generations) {
    auto column_family_test_config = [] {
        static db::nop_large_partition_handler nop_lp_handler;
        column_family::config cfg;
        cfg.large_partition_handler = &nop_lp_handler;
        return cfg;
    };
    storage_service_for_tests ssft;

    auto cm = make_lw_shared<compaction_manager>();
    auto cl_stats = make_lw_shared<cell_locker_stats>();
    auto tracker = make_lw_shared<cache_tracker>();
    auto cf = make_lw_shared<column_family>(s, column_family_test_config(), column_family::no_commitlog(), *cm, *cl_stats, *tracker);
    cf->mark_ready_for_writes();
    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    tmpdir tmp;
    auto sstables = open_sstables(s, format("tests/sstables/3.x/uncompressed/{}", table_name), generations);
    auto new_generation = generations.back() + 1;
    auto new_sstable = [s, path = tmp.path, new_generation] {
        return sstables::test::make_test_sstable(4096, s, path, new_generation,
                         sstables::sstable_version_types::mc, sstable::format_types::big);
    };

    sstables::compact_sstables(sstables::compaction_descriptor(std::move(sstables)), *cf, new_sstable).get();

    auto compacted_sst = open_sstable(s, tmp.path, new_generation);
    return compacted_sst->as_mutation_source().make_reader(s, query::full_partition_range, s->full_slice());
}

SEASTAR_THREAD_TEST_CASE(compact_deleted_row) {
    auto abj = defer([] { await_background_jobs().get(); });
    BOOST_REQUIRE(smp::count == 1);
    sstring table_name = "compact_deleted_row";
    // CREATE TABLE test_deleted_row (pk text, ck text, rc1 text, rc2 text, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck", utf8_type, column_kind::clustering_key);
    builder.with_column("rc1", utf8_type);
    builder.with_column("rc2", utf8_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    builder.set_gc_grace_seconds(std::numeric_limits<int32_t>::max());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    /*
     * Two test SSTables are generated as follows:
     *
     * cqlsh:sst3> INSERT INTO test_deleted_row (pk, ck, rc1) VALUES ('key', 'ck', 'rc1');
     * <flush>
     * cqlsh:sst3> DELETE FROM test_deleted_row WHERE pk = 'key' AND ck = 'ck';
     * cqlsh:sst3> INSERT INTO test_deleted_row (pk, ck, rc2) VALUES ('key', 'ck', 'rc2');
     * <flush>
     *
     * cqlsh:sst3> SELECT * from test_deleted_row ;
     *
     *  pk  | ck | rc1  | rc2
     *  -----+----+------+-----
     *  key | ck | null | rc2
     *  (1 rows)
     *
     * The resulting compacted SSTables look like this:
     *
     * [
     *   {
     *     "partition" : {
     *       "key" : [ "key" ],
     *       "position" : 0
     *     },
     *     "rows" : [
     *       {
     *         "type" : "row",
     *         "position" : 40,
     *         "clustering" : [ "ck" ],
     *         "liveness_info" : { "tstamp" : "1533591593120115" },
     *         "deletion_info" : { "marked_deleted" : "1533591535618022", "local_delete_time" : "1533591535" },
     *         "cells" : [
     *           { "name" : "rc2", "value" : "rc2" }
     *         ]
     *       }
     *     ]
     *   }
     * ]
     */
    auto reader = compacted_sstable_reader(s, table_name, {1, 2});
    mutation_opt m = read_mutation_from_flat_mutation_reader(reader, db::no_timeout).get0();
    BOOST_REQUIRE(m);
    BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("key")))));
    BOOST_REQUIRE(!m->partition().partition_tombstone());
    auto& rows = m->partition().clustered_rows();
    BOOST_REQUIRE(rows.calculate_size() == 1);
    auto& row = rows.begin()->row();
    BOOST_REQUIRE(row.deleted_at());
    auto& cells = row.cells();
    auto& rc1 = *s->get_column_definition("rc1");
    auto& rc2 = *s->get_column_definition("rc2");
    BOOST_REQUIRE(cells.find_cell(rc1.id) == nullptr);
    BOOST_REQUIRE(cells.find_cell(rc2.id) != nullptr);
}

SEASTAR_THREAD_TEST_CASE(compact_deleted_cell) {
    auto abj = defer([] { await_background_jobs().get(); });
    BOOST_REQUIRE(smp::count == 1);
    sstring table_name = "compact_deleted_cell";
    //  CREATE TABLE compact_deleted_cell (pk text, ck text, rc text, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck", utf8_type, column_kind::clustering_key);
    builder.with_column("rc", utf8_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    builder.set_gc_grace_seconds(std::numeric_limits<int32_t>::max());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    /*
     * Two test SSTables are generated as follows:
     *
     * cqlsh:sst3> INSERT INTO compact_deleted_cell (pk, ck, rc) VALUES ('key', 'ck', 'rc');
     * <flush>
     * cqlsh:sst3> DELETE rc FROM compact_deleted_cell WHERE pk = 'key' AND ck = 'ck';
     * <flush>
     *
     * cqlsh:sst3> SELECT * from compact_deleted_cell7;
     *
     *    pk  | ck | rc
     *  -----+----+------
     *   key | ck | null
     *
     *  (1 rows)
     *
     * The resulting compacted SSTables look like this:
     *
     *[
     *  {
     *    "partition" : {
     *      "key" : [ "key" ],
     *      "position" : 0
     *    },
     *    "rows" : [
     *      {
     *        "type" : "row",
     *        "position" : 31,
     *        "clustering" : [ "ck" ],
     *        "liveness_info" : { "tstamp" : "1533601542229143" },
     *        "cells" : [
     *          { "name" : "rc", "deletion_info" : { "local_delete_time" : "1533601605" },
     *            "tstamp" : "1533601605680156"
     *          }
     *        ]
     *      }
     *    ]
     *  }
     *]
     *
     */
    auto reader = compacted_sstable_reader(s, table_name, {1, 2});
    mutation_opt m = read_mutation_from_flat_mutation_reader(reader, db::no_timeout).get0();
    BOOST_REQUIRE(m);
    BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("key")))));
    BOOST_REQUIRE(!m->partition().partition_tombstone());
    auto& rows = m->partition().clustered_rows();
    BOOST_REQUIRE(rows.calculate_size() == 1);
    auto& row = rows.begin()->row();
    BOOST_REQUIRE(row.is_live(*s));
    auto& cells = row.cells();
    BOOST_REQUIRE(cells.size() == 1);
}

static void compare_files(sstring filename1, sstring filename2) {
    std::ifstream ifs1(filename1);
    std::ifstream ifs2(filename2);

    std::istream_iterator<char> b1(ifs1), e1;
    std::istream_iterator<char> b2(ifs2), e2;
    BOOST_CHECK_EQUAL_COLLECTIONS(b1, e1, b2, e2);
}

static sstring get_write_test_path(sstring table_name, bool compressed = false) {
    return format("tests/sstables/3.x/{}compressed/write_{}", compressed ? "" : "un", table_name);
}

static void compare_sstables(sstring result_path, sstring table_name, bool compressed = false) {
    for (auto file_type : {component_type::Data,
                           component_type::Index,
                           component_type::Digest,
                           component_type::Filter}) {
        auto orig_filename =
                sstable::filename(get_write_test_path(table_name, compressed),
                                  "ks", table_name, sstables::sstable_version_types::mc, 1, big, file_type);
        auto result_filename =
                sstable::filename(result_path, "ks", table_name, sstables::sstable_version_types::mc, 1, big, file_type);
        compare_files(orig_filename, result_filename);
    }
}

// Can be useful if we want, e.g., to avoid range tombstones de-overlapping
// that otherwise takes place for RTs put into one and the same memtable
static tmpdir write_and_compare_sstables(schema_ptr s, lw_shared_ptr<memtable> mt1, lw_shared_ptr<memtable> mt2,
                                       sstring table_name, bool compressed = false) {
    static db::nop_large_partition_handler nop_lp_handler;
    storage_service_for_tests ssft;
    tmpdir tmp;
    auto sst = sstables::test::make_test_sstable(4096, s, tmp.path, 1, sstables::sstable_version_types::mc, sstable::format_types::big);
    sstable_writer_config cfg;
    cfg.large_partition_handler = &nop_lp_handler;
    sst->write_components(make_combined_reader(s,
        mt1->make_flat_reader(s),
        mt2->make_flat_reader(s)), 1, s, cfg, mt1->get_stats()).get();
    compare_sstables(tmp.path, table_name, compressed);
    return tmp;
}

static tmpdir write_and_compare_sstables(schema_ptr s, lw_shared_ptr<memtable> mt, sstring table_name, bool compressed = false) {
    storage_service_for_tests ssft;
    tmpdir tmp;
    auto sst = sstables::test::make_test_sstable(4096, s, tmp.path, 1, sstables::sstable_version_types::mc, sstable::format_types::big);
    write_memtable_to_sstable_for_test(*mt, sst).get();
    compare_sstables(tmp.path, table_name, compressed);
    return tmp;
}

static void validate_read(schema_ptr s, sstring path, std::vector<mutation> mutations) {
    sstable_assertions sst(s, path);
    sst.load();

    auto assertions = assert_that(sst.read_rows_flat());
    for (const auto &mut : mutations) {
        assertions.produces(mut);
    }

    assertions.produces_end_of_stream();
}

static constexpr api::timestamp_type write_timestamp = 1525385507816568;
static constexpr gc_clock::time_point write_time_point = gc_clock::time_point{} + gc_clock::duration{1525385507};

SEASTAR_THREAD_TEST_CASE(test_write_static_row) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "static_row";
    // CREATE TABLE static_row (pk text, ck int, st1 int static, st2 text static, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("st1", int32_type, column_kind::static_column);
    builder.with_column("st2", utf8_type, column_kind::static_column);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO static_row (pk, st1, st2) values ('key1', 1135, 'hello');
    auto key = make_dkey(s, {to_bytes("key1")});
    mutation mut{s, key};
    mut.set_static_cell("st1", data_value{1135}, write_timestamp);
    mut.set_static_cell("st2", data_value{"hello"}, write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_composite_partition_key) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "composite_partition_key";
    // CREATE TABLE composite_partition_key (a int , b text, c boolean, d int, e text, f int, g text, PRIMARY KEY ((a, b, c), d, e)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("a", int32_type, column_kind::partition_key);
    builder.with_column("b", utf8_type, column_kind::partition_key);
    builder.with_column("c", boolean_type, column_kind::partition_key);
    builder.with_column("d", int32_type, column_kind::clustering_key);
    builder.with_column("e", utf8_type, column_kind::clustering_key);
    builder.with_column("f", int32_type);
    builder.with_column("g", utf8_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO composite_partition_key (a,b,c,d,e,f,g) values (1, 'hello', true, 2, 'dear', 3, 'world');
    auto key = partition_key::from_deeply_exploded(*s, { data_value{1}, data_value{"hello"}, data_value{true} });
    mutation mut{s, key};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 2, "dear" });
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_cell(ckey, "f", data_value{3}, write_timestamp);
    mut.set_cell(ckey, "g", data_value{"world"}, write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_composite_clustering_key) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "composite_clustering_key";
    // CREATE TABLE composite_clustering_key (a int , b text, c int, d text, e int, f text, PRIMARY KEY (a, b, c, d)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("a", int32_type, column_kind::partition_key);
    builder.with_column("b", utf8_type, column_kind::clustering_key);
    builder.with_column("c", int32_type, column_kind::clustering_key);
    builder.with_column("d", utf8_type, column_kind::clustering_key);
    builder.with_column("e", int32_type);
    builder.with_column("f", utf8_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO composite_clustering_key (a,b,c,d,e,f) values (1, 'hello', 2, 'dear', 3, 'world');
    auto key = partition_key::from_deeply_exploded(*s, { 1 });
    mutation mut{s, key};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { "hello", 2, "dear" });
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_cell(ckey, "e", data_value{3}, write_timestamp);
    mut.set_cell(ckey, "f", data_value{"world"}, write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_wide_partitions) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "wide_partitions";
    // CREATE TABLE wide_partitions (pk text, ck text, st text, rc text, PRIMARY KEY (pk, ck) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck", utf8_type, column_kind::clustering_key);
    builder.with_column("st", utf8_type, column_kind::static_column);
    builder.with_column("rc", utf8_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);
    api::timestamp_type ts = write_timestamp;
    sstring ck_base(1024, 'a');
    sstring rc_base(1024, 'b');
    auto key1 = make_dkey(s, {to_bytes("key1")});
    mutation mut1{s, key1};
    {
        mut1.set_static_cell("st", data_value{"hello"}, ts);
        for (auto idx: boost::irange(0, 1024)) {
            clustering_key ckey = clustering_key::from_deeply_exploded(*s, {format("{}{}", ck_base, idx)});
            mut1.partition().apply_insert(*s, ckey, ts);
            mut1.set_cell(ckey, "rc", data_value{format("{}{}", rc_base, idx)}, ts);
            seastar::thread::yield();
        }
        mt->apply(mut1);
        ts += 10;
    }
    auto key2 = make_dkey(s, {to_bytes("key2")});
    mutation mut2{s, key2};
    {
        mut2.set_static_cell("st", data_value{"goodbye"}, ts);
        for (auto idx: boost::irange(0, 1024)) {
            clustering_key ckey = clustering_key::from_deeply_exploded(*s, {format("{}{}", ck_base, idx)});
            mut2.partition().apply_insert(*s, ckey, ts);
            mut2.set_cell(ckey, "rc", data_value{format("{}{}", rc_base, idx)}, ts);
            seastar::thread::yield();
        }
        mt->apply(mut2);
    }

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut1, mut2});
}

SEASTAR_THREAD_TEST_CASE(test_write_ttled_row) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "ttled_row";
    // CREATE TABLE ttled_row (pk int, ck int, rc int, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO ttled_row (pk, ck, rc) VALUES ( 1, 2, 3) USING TTL 1135;
    auto key = partition_key::from_deeply_exploded(*s, { 1 });
    mutation mut{s, key};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 2 });
    gc_clock::duration ttl{1135};

    auto column_def = s->get_column_definition("rc");
    if (!column_def) {
        throw std::runtime_error("no column definition found");
    }
    mut.partition().apply_insert(*s, ckey, write_timestamp, ttl, write_time_point + ttl);
    bytes value = column_def->type->decompose(data_value{3});
    auto cell = atomic_cell::make_live(*column_def->type, write_timestamp, value, write_time_point + ttl, ttl);
    mut.set_clustered_cell(ckey, *column_def, std::move(cell));
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_ttled_column) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "ttled_column";
    // CREATE TABLE ttled_column (pk text, rc int, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // UPDATE ttled_column USING TTL 1135 SET rc = 1 WHERE pk='key';
    auto key = make_dkey(s, {to_bytes("key")});
    mutation mut{s, key};

    gc_clock::duration ttl{1135};
    auto column_def = s->get_column_definition("rc");
    if (!column_def) {
        throw std::runtime_error("no column definition found");
    }
    bytes value = column_def->type->decompose(data_value{1});
    auto cell = atomic_cell::make_live(*column_def->type, write_timestamp, value, write_time_point + ttl, ttl);
    mut.set_clustered_cell(clustering_key::make_empty(), *column_def, std::move(cell));
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_deleted_column) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "deleted_column";
    // CREATE TABLE deleted_column (pk int, rc int, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // DELETE rc FROM deleted_column WHERE pk=1;
    auto key = partition_key::from_deeply_exploded(*s, { 1 });
    mutation mut{s, key};
    auto column_def = s->get_column_definition("rc");
    if (!column_def) {
        throw std::runtime_error("no column definition found");
    }
    mut.set_cell(clustering_key::make_empty(), *column_def, atomic_cell::make_dead(write_timestamp, write_time_point));
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_deleted_row) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "deleted_row";
    // CREATE TABLE deleted_row (pk int, ck int, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // DELETE FROM deleted_row WHERE pk=1 and ck=2;
    auto key = partition_key::from_deeply_exploded(*s, { 1 });
    mutation mut{s, key};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 2 });
    mut.partition().apply_delete(*s, ckey, tombstone{write_timestamp, write_time_point});
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_collection_wide_update) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "collection_wide_update";
    auto set_of_ints_type = set_type_impl::get_instance(int32_type, true);
    // CREATE TABLE collection_wide_update (pk int, col set<int>, PRIMARY KEY (pk)) with compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("col", set_of_ints_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO collection_wide_update (pk, col) VALUES (1, {2, 3});
    auto key = partition_key::from_deeply_exploded(*s, { 1 });
    mutation mut{s, key};

    mut.partition().apply_insert(*s, clustering_key::make_empty(), write_timestamp);
    set_type_impl::mutation set_values;
    set_values.tomb = tombstone {write_timestamp - 1, write_time_point};
    set_values.cells.emplace_back(int32_type->decompose(2), atomic_cell::make_live(*bytes_type, write_timestamp, bytes_view{}));
    set_values.cells.emplace_back(int32_type->decompose(3), atomic_cell::make_live(*bytes_type, write_timestamp, bytes_view{}));

    mut.set_clustered_cell(clustering_key::make_empty(), *s->get_column_definition("col"), set_of_ints_type->serialize_mutation_form(set_values));
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_collection_incremental_update) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "collection_incremental_update";
    auto set_of_ints_type = set_type_impl::get_instance(int32_type, true);
    // CREATE TABLE collection_incremental_update (pk int, col set<int>, PRIMARY KEY (pk)) with compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("col", set_of_ints_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // UPDATE collection_incremental_update SET col = col + {2} WHERE pk = 1;
    auto key = partition_key::from_deeply_exploded(*s, { 1 });
    mutation mut{s, key};

    set_type_impl::mutation set_values;
    set_values.cells.emplace_back(int32_type->decompose(2), atomic_cell::make_live(*bytes_type, write_timestamp, bytes_view{}));

    mut.set_clustered_cell(clustering_key::make_empty(), *s->get_column_definition("col"), set_of_ints_type->serialize_mutation_form(set_values));
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_multiple_partitions) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "multiple_partitions";
    // CREATE TABLE multiple_partitions (pk int, rc1 int, rc2 int, rc3 int, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("rc1", int32_type);
    builder.with_column("rc2", int32_type);
    builder.with_column("rc3", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    api::timestamp_type ts = write_timestamp;
    // INSERT INTO multiple_partitions (pk, rc1) VALUES (1, 10);
    // INSERT INTO multiple_partitions (pk, rc2) VALUES (2, 20);
    // INSERT INTO multiple_partitions (pk, rc3) VALUES (3, 30);
    std::vector<mutation> muts;
    for (auto i : boost::irange(1, 4)) {
        auto key = partition_key::from_deeply_exploded(*s, {i});
        muts.emplace_back(s, key);

        clustering_key ckey = clustering_key::make_empty();
        muts.back().partition().apply_insert(*s, ckey, ts);
        muts.back().set_cell(ckey, to_bytes(format("rc{}", i)), data_value{i * 10}, ts);
        mt->apply(muts.back());
        ts += 10;
    }

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, muts);
}

static void test_write_many_partitions(sstring table_name, tombstone partition_tomb, compression_parameters cp) {
    // CREATE TABLE <table_name> (pk int, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.set_compressor_params(cp);
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    std::vector<mutation> muts;
    for (auto i : boost::irange(0, 65536)) {
        auto key = partition_key::from_deeply_exploded(*s, {i});
        muts.emplace_back(s, key);
        if (partition_tomb) {
            muts.back().partition().apply(partition_tomb);
        }
        mt->apply(muts.back());
    }

    bool compressed = cp.get_compressor() != nullptr;
    std::cout << (compressed ? "compressed " : "uncompressed") << std::endl;
    tmpdir tmp = write_and_compare_sstables(s, mt, table_name, compressed);
    std::cout << "wrote and compared\n";
    boost::sort(muts, mutation_decorated_key_less_comparator());
    validate_read(s, tmp.path, muts);
}

SEASTAR_THREAD_TEST_CASE(test_write_many_live_partitions) {
    auto abj = defer([] { await_background_jobs().get(); });
    test_write_many_partitions(
            "many_live_partitions",
            tombstone{},
            compression_parameters::no_compression());
}

SEASTAR_THREAD_TEST_CASE(test_write_many_deleted_partitions) {
    auto abj = defer([] { await_background_jobs().get(); });
    test_write_many_partitions(
            "many_deleted_partitions",
            tombstone{write_timestamp, write_time_point},
            compression_parameters::no_compression());
}

SEASTAR_THREAD_TEST_CASE(test_write_many_partitions_lz4) {
    auto abj = defer([] { await_background_jobs().get(); });
    test_write_many_partitions(
            "many_partitions_lz4",
            tombstone{},
            compression_parameters{compressor::lz4});
}

SEASTAR_THREAD_TEST_CASE(test_write_many_partitions_snappy) {
    auto abj = defer([] { await_background_jobs().get(); });
    test_write_many_partitions(
            "many_partitions_snappy",
            tombstone{},
            compression_parameters{compressor::snappy});
}

SEASTAR_THREAD_TEST_CASE(test_write_many_partitions_deflate) {
    auto abj = defer([] { await_background_jobs().get(); });
    test_write_many_partitions(
            "many_partitions_deflate",
            tombstone{},
            compression_parameters{compressor::deflate});
}

SEASTAR_THREAD_TEST_CASE(test_write_multiple_rows) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "multiple_rows";
    // CREATE TABLE multiple_rows (pk int, ck int, rc1 int, rc2 int, rc3 int, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("rc1", int32_type);
    builder.with_column("rc2", int32_type);
    builder.with_column("rc3", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    api::timestamp_type ts = write_timestamp;
    mutation mut{s, key};

    // INSERT INTO multiple_rows (pk, ck, rc1) VALUES (0, 1, 10);
    // INSERT INTO multiple_rows (pk, ck, rc2) VALUES (0, 2, 20);
    // INSERT INTO multiple_rows (pk, ck, rc3) VALUES (0, 3, 30);
    for (auto i : boost::irange(1, 4)) {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, { i });
        mut.partition().apply_insert(*s, ckey, ts);
        mut.set_cell(ckey, to_bytes(format("rc{}", i)), data_value{i * 10}, ts);
        ts += 10;
    }

    mt->apply(mut);
    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

// Information on missing columns is serialized differently when the number of columns is > 64.
// This test checks that this information is encoded correctly.
SEASTAR_THREAD_TEST_CASE(test_write_missing_columns_large_set) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "missing_columns_large_set";
    // CREATE TABLE missing_columns_large_set (pk int, ck int, rc1 int, ..., rc64 int, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    for (auto idx: boost::irange(1, 65)) {
        builder.with_column(to_bytes(format("rc{}", idx)), int32_type);
    }
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    api::timestamp_type ts = write_timestamp;
    mutation mut{s, key};

    // INSERT INTO missing_columns_large_set (pk, ck, rc1, ..., rc62) VALUES (0, 0, 1, ..., 62);
    // For missing columns, the missing ones will be written as majority are present.
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {0});
        mut.partition().apply_insert(*s, ckey, ts);
        for (auto idx: boost::irange(1, 63)) {
            mut.set_cell(ckey, to_bytes(format("rc{}", idx)), data_value{idx}, ts);
        }
    }
    ts += 10;

    // INSERT INTO missing_columns_large_set (pk, ck, rc63, rc64) VALUES (0, 1, 63, 64);
    // For missing columns, the present ones will be written as majority are missing.
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {1});
        mut.partition().apply_insert(*s, ckey, ts);
        mut.set_cell(ckey, to_bytes("rc63"), data_value{63}, ts);
        mut.set_cell(ckey, to_bytes("rc64"), data_value{64}, ts);
    }
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_counter_table) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "counter_table";
    // CREATE TABLE counter_table (pk text, ck text, rc1 counter, rc2 counter, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck", utf8_type, column_kind::clustering_key);
    builder.with_column("rc1", counter_type);
    builder.with_column("rc2", counter_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);
    auto key = partition_key::from_exploded(*s, {to_bytes("key")});
    mutation mut{s, key};

    // Keep counter ids fixed to produce the same binary output on each run
    std::vector<counter_id> ids = {
        counter_id{utils::UUID{"19478feb-8e7c-4729-9a78-9b476552f13d"}},
        counter_id{utils::UUID{"e079a6fe-eb79-4bdf-97ca-c87a2c387d5c"}},
        counter_id{utils::UUID{"bbba5897-78b6-4cdc-9a0d-ea9e9a3b833f"}},
    };
    boost::range::sort(ids);

    const column_definition& cdef1 = *s->get_column_definition("rc1");
    const column_definition& cdef2 = *s->get_column_definition("rc2");

    auto ckey1 = clustering_key::from_exploded(*s, {to_bytes("ck1")});

    counter_cell_builder b1;
    b1.add_shard(counter_shard(ids[0], 5, 1));
    b1.add_shard(counter_shard(ids[1], -4, 1));
    b1.add_shard(counter_shard(ids[2], 9, 1));
    mut.set_clustered_cell(ckey1, cdef1, b1.build(write_timestamp));

    counter_cell_builder b2;
    b2.add_shard(counter_shard(ids[1], -1, 1));
    b2.add_shard(counter_shard(ids[2], 2, 1));
    mut.set_clustered_cell(ckey1, cdef2, b2.build(write_timestamp));

    auto ckey2 = clustering_key::from_exploded(*s, {to_bytes("ck2")});
    mut.set_clustered_cell(ckey2, cdef1, atomic_cell::make_dead(write_timestamp, write_time_point));

    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_different_types) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "different_types";
    // CREATE TABLE different_types (pk text, asciival ascii, bigintval bigint,
    // blobval blob, boolval boolean, dateval date, decimalval decimal,
    // doubleval double, floatval float, inetval inet, intval int,
    // smallintval smallint, timeval time, tsval timestamp, timeuuidval timeuuid,
    // tinyintval tinyint,  uuidval uuid, varcharval varchar, varintval varint,
    // durationval duration, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("asciival", ascii_type);
    builder.with_column("bigintval", long_type);
    builder.with_column("blobval", bytes_type);
    builder.with_column("boolval", boolean_type);
    builder.with_column("dateval", simple_date_type);
    builder.with_column("decimalval", decimal_type);
    builder.with_column("doubleval", double_type);
    builder.with_column("floatval", float_type);
    builder.with_column("inetval", inet_addr_type);
    builder.with_column("intval", int32_type);
    builder.with_column("smallintval", short_type);
    builder.with_column("timeval", time_type);
    builder.with_column("tsval", timestamp_type);
    builder.with_column("timeuuidval", timeuuid_type);
    builder.with_column("tinyintval", byte_type);
    builder.with_column("uuidval", uuid_type);
    builder.with_column("varcharval", utf8_type);
    builder.with_column("varintval", varint_type);
    builder.with_column("durationval", duration_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO different_types (pk, asciival, bigintval, blobval, boolval,
    // dateval, decimalval, doubleval, floatval, inetval, intval, smallintval,
    // timeval, tsval, timeuuidval, tinyintval, uuidval, varcharval, varintval,
    // durationval) VALUES ('key', 'hello', 9223372036854775807,
    // textAsBlob('great'), true, '2017-05-05',
    // 5.45, 36.6, 7.62, '192.168.0.110', -2147483648, 32767, '19:45:05.090',
    // '2015-05-01 09:30:54.234+0000', 50554d6e-29bb-11e5-b345-feff819cdc9f, 127,
    // 01234567-0123-0123-0123-0123456789ab, 'привет', 123, 1h4m48s20ms);
    auto key = make_dkey(s, {to_bytes("key")});
    mutation mut{s, key};
    clustering_key ckey = clustering_key::make_empty();
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_cell(ckey, "asciival", data_value{"hello"}, write_timestamp);
    mut.set_cell(ckey, "bigintval", data_value{std::numeric_limits<int64_t>::max()}, write_timestamp);
    mut.set_cell(ckey, "blobval", data_value{bytes{'g', 'r', 'e', 'a', 't'}}, write_timestamp);
    mut.set_cell(ckey, "boolval", data_value{true}, write_timestamp);
    mut.set_cell(ckey, "dateval", simple_date_type->deserialize(simple_date_type->from_string("2017-05-05")), write_timestamp);
    mut.set_cell(ckey, "decimalval", decimal_type->deserialize(decimal_type->from_string("5.45")), write_timestamp);
    mut.set_cell(ckey, "doubleval", data_value{36.6}, write_timestamp);
    mut.set_cell(ckey, "floatval", data_value{7.62f}, write_timestamp);
    mut.set_cell(ckey, "inetval", inet_addr_type->deserialize(inet_addr_type->from_string("192.168.0.110")), write_timestamp);
    mut.set_cell(ckey, "intval", data_value{std::numeric_limits<int32_t>::min()}, write_timestamp);
    mut.set_cell(ckey, "smallintval", data_value{int16_t(32767)}, write_timestamp);
    mut.set_cell(ckey, "timeval", time_type->deserialize(time_type->from_string("19:45:05.090")), write_timestamp);
    mut.set_cell(ckey, "tsval", timestamp_type->deserialize(timestamp_type->from_string("2015-05-01 09:30:54.234+0000")), write_timestamp);
    mut.set_cell(ckey, "timeuuidval", timeuuid_type->deserialize(timeuuid_type->from_string("50554d6e-29bb-11e5-b345-feff819cdc9f")), write_timestamp);
    mut.set_cell(ckey, "tinyintval", data_value{int8_t{127}}, write_timestamp);
    mut.set_cell(ckey, "uuidval", data_value{utils::UUID(sstring("01234567-0123-0123-0123-0123456789ab"))}, write_timestamp);
    mut.set_cell(ckey, "varcharval", data_value{"привет"}, write_timestamp);
    mut.set_cell(ckey, "varintval", varint_type->deserialize(varint_type->from_string("123")), write_timestamp);
    mut.set_cell(ckey, "durationval", duration_type->deserialize(duration_type->from_string("1h4m48s20ms")), write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_empty_clustering_values) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "empty_clustering_values";
    // CREATE TABLE empty_clustering_values (pk int, ck1 text, ck2 int, ck3 text, rc int, PRIMARY KEY (pk, ck1, ck2, ck3)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", int32_type, column_kind::clustering_key);
    builder.with_column("ck3", utf8_type, column_kind::clustering_key);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};

    // INSERT INTO empty_clustering_values (pk, ck1, ck2, ck3, rc) VALUES (0, '', 1, '', 2);
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { "", 1, "" });
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_cell(ckey, "rc", data_value{2}, write_timestamp);

    mt->apply(mut);
    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_large_clustering_key) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "large_clustering_key";
    // CREATE TABLE large_clustering_key (pk int, ck1 text, ck2 text, ..., ck35 text, rc int, PRIMARY KEY (pk, ck1, ck2, ..., ck35)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    for (auto idx: boost::irange(1, 36)) {
        builder.with_column(to_bytes(format("ck{}", idx)), utf8_type, column_kind::clustering_key);
    }
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};

    // Clustering columns ckX are filled the following way:
    //    - <empty> for even X
    //    - "X" for odd X
    // INSERT INTO large_clustering_key (pk, ck1, ck2, ck3, rc) VALUES (0, '1', '', '3',..., '', '35', 1);
    std::vector<data_value> clustering_values;
    for (auto idx: boost::irange(1, 36)) {
        clustering_values.emplace_back((idx % 2 == 1) ? std::to_string(idx) : std::string{});
    }
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, clustering_values);
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_cell(ckey, "rc", data_value{1}, write_timestamp);

    mt->apply(mut);
    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_compact_table) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "compact_table";
    // CREATE TABLE compact_table (pk int, ck1 int, ck2 int, rc int, PRIMARY KEY (pk, ck1, ck2)) WITH compression = {'sstable_compression': ''} AND COMPACT STORAGE;
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck1", int32_type, column_kind::clustering_key);
    builder.with_column("ck2", int32_type, column_kind::clustering_key);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::yes);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = partition_key::from_deeply_exploded(*s, {1});
    mutation mut{s, key};

    // INSERT INTO compact_table (pk, ck1, rc) VALUES (1, 1, 1);
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 1 });
    mut.set_cell(ckey, "rc", data_value{1}, write_timestamp);

    mt->apply(mut);
    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_user_defined_type_table) {
    auto abj = defer([] { await_background_jobs().get(); });
    // CREATE TYPE ut (my_int int, my_boolean boolean, my_text text);
    auto ut = user_type_impl::get_instance("sst3", to_bytes("ut"),
            {to_bytes("my_int"), to_bytes("my_boolean"), to_bytes("my_text")},
            {int32_type, boolean_type, utf8_type});

    sstring table_name = "user_defined_type_table";
    // CREATE TABLE user_defined_type_table (pk int, rc frozen <ut>, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("rc", ut);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};

    // INSERT INTO user_defined_type_table (pk, rc) VALUES (0, {my_int: 1703, my_boolean: true, my_text: 'Санкт-Петербург'}) USING TIMESTAMP 1525385507816568;
    clustering_key ckey = clustering_key::make_empty();
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    auto ut_val = make_user_value(ut, user_type_impl::native_type({int32_t(1703), true, sstring("Санкт-Петербург")}));
    mut.set_cell(ckey, "rc", ut_val, write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_simple_range_tombstone) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "simple_range_tombstone";
    // CREATE TABLE simple_range_tombstone (pk int, ck1 text, ck2 text, rc text, PRIMARY KEY (pk, ck1, ck2)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.with_column("rc", utf8_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // DELETE FROM simple_range_tombstone WHERE pk = 0 and ck1 = 'aaa';
    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};
    gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1528142098};
    tombstone tomb{write_timestamp, tp};
    range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")), clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb};
    mut.partition().apply_delete(*s, std::move(rt));
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

// Test the case when for RTs their adjacent bounds are written as boundary RT markers.
SEASTAR_THREAD_TEST_CASE(test_write_adjacent_range_tombstones) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "adjacent_range_tombstones";
    // CREATE TABLE adjacent_range_tombstones (pk text, ck1 text, ck2 text, ck3 text, PRIMARY KEY (pk, ck1, ck2, ck3)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.with_column("ck3", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = make_dkey(s, {to_bytes("key")});
    mutation mut{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM adjacent_range_tombstones USING TIMESTAMP 1525385507816568 WHERE pk = 'key' AND ck1 = 'aaa';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1527821291};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // DELETE FROM adjacent_range_tombstones USING TIMESTAMP 1525385507816578 WHERE pk = 'key' AND ck1 = 'aaa' AND ck2 = 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1527821308};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"}),
                           clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"}), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

// Test the case when subsequent RTs have a common clustering but those bounds are both exclusive
// so cannot be merged into a single boundary RT marker.
SEASTAR_THREAD_TEST_CASE(test_write_non_adjacent_range_tombstones) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "non_adjacent_range_tombstones";
    // CREATE TABLE non_adjacent_range_tombstones (pk text, ck1 text, ck2 text, ck3 text, PRIMARY KEY (pk, ck1, ck2, ck3)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.with_column("ck3", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = make_dkey(s, {to_bytes("key")});
    mutation mut{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM non_adjacent_range_tombstones USING TIMESTAMP 1525385507816568 WHERE pk = 'key' AND ck1 > 'aaa' AND ck1 < 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1528144611};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb, bound_kind::excl_start,
                           clustering_key_prefix::from_single_value(*s, bytes("bbb")), bound_kind::excl_end};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // DELETE FROM non_adjacent_range_tombstones USING TIMESTAMP 1525385507816578 WHERE pk = 'key' AND ck1 = 'aaa' AND ck2 = 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1528144623};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("bbb")), tomb, bound_kind::excl_start,
                           clustering_key_prefix::from_single_value(*s, bytes("ccc")), bound_kind::excl_end};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_mixed_rows_and_range_tombstones) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "mixed_rows_and_range_tombstones";
    // CREATE TABLE mixed_rows_and_range_tombstones (pk text, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = make_dkey(s, {to_bytes("key")});
    mutation mut{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM mixed_rows_and_range_tombstones USING TIMESTAMP 1525385507816568 WHERE pk = 'key' AND ck1 = 'aaa';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1528157078};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO mixed_rows_and_range_tombstones (pk, ck1, ck2) VALUES ('key', 'aaa', 'bbb') USING TIMESTAMP 1525385507816578;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"});
        mut.partition().apply_insert(*s, ckey, ts);
    }
    ts += 10;

    // DELETE FROM mixed_rows_and_range_tombstones USING TIMESTAMP 1525385507816588 WHERE pk = 'key' AND ck1 = 'bbb' AND ck2 <= 'ccc';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1528157101};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, to_bytes("bbb")),
                           clustering_key_prefix::from_deeply_exploded(*s, {"bbb", "ccc"}), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO mixed_rows_and_range_tombstones (pk, ck1, ck2) VALUES ('key', 'bbb', 'ccc') USING TIMESTAMP 1525385507816598;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"bbb", "ccc"});
        mut.partition().apply_insert(*s, ckey, ts);
    }
    ts += 10;

    // DELETE FROM mixed_rows_and_range_tombstones USING TIMESTAMP 1525385507816608 WHERE pk = 'key' AND ck1 = 'ddd' AND ck2 >= 'eee';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1528157186};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_deeply_exploded(*s, {"ddd", "eee"}),
                           clustering_key_prefix::from_single_value(*s, to_bytes("ddd")), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO mixed_rows_and_range_tombstones (pk, ck1, ck2) VALUES ('key', 'bbb', 'ccc') USING TIMESTAMP 1525385507816618;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"ddd", "eee"});
        mut.partition().apply_insert(*s, ckey, ts);
    }
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_many_range_tombstones) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "many_range_tombstones";
    // CREATE TABLE many_range_tombstones (pk text, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = make_dkey(s, {to_bytes("key1")});
    mutation mut{s, key};

    gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1528226962};
    tombstone tomb{write_timestamp, tp};
    sstring ck_base(650, 'a');
    for (auto idx: boost::irange(1000, 1100)) {
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, to_bytes(format("{}{}", ck_base, idx * 2))), tomb,
                           bound_kind::excl_start,
                           clustering_key_prefix::from_single_value(*s, to_bytes(format("{}{}", ck_base, idx * 2 + 1))),
                           bound_kind::excl_end};
        mut.partition().apply_delete(*s, std::move(rt));
        seastar::thread::yield();
    }
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_adjacent_range_tombstones_with_rows) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "adjacent_range_tombstones_with_rows";
    // CREATE TABLE adjacent_range_tombstones_with_rows (pk text, ck1 text, ck2 text, ck3 text, PRIMARY KEY (pk, ck1, ck2, ck3)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.with_column("ck3", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = make_dkey(s, {to_bytes("key")});
    mutation mut{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM adjacent_range_tombstones_with_rows USING TIMESTAMP 1525385507816568 WHERE pk = 'key' AND ck1 = 'aaa';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529007036};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO adjacent_range_tombstones_with_rows (pk, ck1, ck2, ck3) VALUES ('key', 'aaa', 'aaa', 'aaa') USING TIMESTAMP 1525385507816578;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"aaa", "aaa", "aaa"});
        mut.partition().apply_insert(*s, ckey, ts);
    }
    ts += 10;

    // DELETE FROM adjacent_range_tombstones_with_rows USING TIMESTAMP 1525385507816588 WHERE pk = 'key' AND ck1 = 'aaa' AND ck2 = 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529007103};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"}),
                           clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"}), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO adjacent_range_tombstones_with_rows (pk, ck1, ck2, ck3) VALUES ('key', 'aaa', 'ccc', 'ccc') USING TIMESTAMP 1525385507816598;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"aaa", "ccc", "ccc"});
        mut.partition().apply_insert(*s, ckey, ts);
    }
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_range_tombstone_same_start_with_row) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "range_tombstone_same_start_with_row";
    // CREATE TABLE range_tombstone_same_start_with_row (pk int, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM range_tombstone_same_start_with_row USING TIMESTAMP 1525385507816568 WHERE pk = 0 AND ck1 = 'aaa' AND ck2 >= 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529099073};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_deeply_exploded(*s, {"aaa", "bbb"}),
                           clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO range_tombstone_same_start_with_row (pk, ck1, ck2) VALUES (0, 'aaa', 'bbb') USING TIMESTAMP 1525385507816578;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"});
        mut.partition().apply_insert(*s, ckey, ts);
    }

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);
    mt->apply(mut);

    write_and_compare_sstables(s, mt, table_name);
}

SEASTAR_THREAD_TEST_CASE(test_write_range_tombstone_same_end_with_row) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "range_tombstone_same_end_with_row";
    // CREATE TABLE range_tombstone_same_end_with_row (pk int, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM range_tombstone_same_end_with_row  USING TIMESTAMP 1525385507816568 WHERE pk = 0 AND ck1 = 'aaa' AND ck2 <= 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529099073};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           clustering_key_prefix::from_deeply_exploded(*s, {"aaa", "bbb"}), tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO range_tombstone_same_end_with_row (pk, ck1, ck2) VALUES (0, 'aaa', 'bbb') USING TIMESTAMP 1525385507816578;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"});
        mut.partition().apply_insert(*s, ckey, ts);
    }

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);
    mt->apply(mut);

    write_and_compare_sstables(s, mt, table_name);
}

SEASTAR_THREAD_TEST_CASE(test_write_overlapped_start_range_tombstones) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "overlapped_start_range_tombstones";
    // CREATE TABLE overlapped_start_range_tombstones (pk int, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);


    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut1{s, key};
    mutation mut2{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM overlapped_start_range_tombstones USING TIMESTAMP 1525385507816568 WHERE pk = 0 and ck1 = 'aaa';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529099073};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb};
        mut1.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // INSERT INTO overlapped_start_range_tombstones (pk, ck1, ck2) VALUES (0, 'aaa', 'bbb') USING TIMESTAMP 1525385507816578;
    {
        clustering_key ckey = clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"});
        mut2.partition().apply_insert(*s, ckey, ts);
    }
    ts += 10;

    // DELETE FROM overlapped_start_range_tombstones USING TIMESTAMP 1525385507816588 WHERE pk = 0 AND ck1 = 'aaa' AND ck2 > 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529099152};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"}), tomb,
                           bound_kind::excl_start,
                           clustering_key::from_single_value(*s, bytes("aaa")),
                           bound_kind::incl_end};
        mut2.partition().apply_delete(*s, std::move(rt));
    }

    lw_shared_ptr<memtable> mt1 = make_lw_shared<memtable>(s);
    lw_shared_ptr<memtable> mt2 = make_lw_shared<memtable>(s);

    mt1->apply(mut1);
    mt2->apply(mut2);

    write_and_compare_sstables(s, mt1, mt2, table_name);
}

SEASTAR_THREAD_TEST_CASE(test_write_two_non_adjacent_range_tombstones) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "two_non_adjacent_range_tombstones";
    // CREATE TABLE two_non_adjacent_range_tombstones (pk int, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM two_non_adjacent_range_tombstones USING TIMESTAMP 1525385507816568 WHERE pk = 0 AND ck1 = 'aaa' AND ck2 < 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529535128};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           bound_kind::incl_start,
                           clustering_key_prefix::from_deeply_exploded(*s, {"aaa", "bbb"}),
                           bound_kind::excl_end, tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // DELETE FROM range_tombstone_same_end_with_row  USING TIMESTAMP 1525385507816568 WHERE pk = 0 AND ck1 = 'aaa' AND ck2 <= 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1529535139};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_deeply_exploded(*s, {"aaa", "bbb"}),
                           bound_kind::excl_start,
                           clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           bound_kind::incl_end, tomb};
        mut.partition().apply_delete(*s, std::move(rt));
    }

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);
    mt->apply(mut);

    write_and_compare_sstables(s, mt, table_name);
}

// The resulting files are supposed to be identical to the files
// from test_write_adjacent_range_tombstones
SEASTAR_THREAD_TEST_CASE(test_write_overlapped_range_tombstones) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "overlapped_range_tombstones";
    // CREATE TABLE overlapped_range_tombstones (pk text, ck1 text, ck2 text, ck3 text, PRIMARY KEY (pk, ck1, ck2, ck3)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.with_column("ck3", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);


    auto key = make_dkey(s, {to_bytes("key")});
    mutation mut1{s, key};
    mutation mut2{s, key};
    api::timestamp_type ts = write_timestamp;

    // DELETE FROM overlapped_range_tombstones USING TIMESTAMP 1525385507816568 WHERE pk = 'key' AND ck1 = 'aaa';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1527821291};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key_prefix::from_single_value(*s, bytes("aaa")),
                           clustering_key_prefix::from_single_value(*s, bytes("aaa")), tomb};
        mut1.partition().apply_delete(*s, std::move(rt));
    }
    ts += 10;

    // DELETE FROM overlapped_range_tombstones USING TIMESTAMP 1525385507816578 WHERE pk = 'key' AND ck1 = 'aaa' AND ck2 = 'bbb';
    {
        gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1527821308};
        tombstone tomb{ts, tp};
        range_tombstone rt{clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"}),
                           clustering_key::from_deeply_exploded(*s, {"aaa", "bbb"}), tomb};
        mut2.partition().apply_delete(*s, std::move(rt));
    }

    lw_shared_ptr<memtable> mt1 = make_lw_shared<memtable>(s);
    lw_shared_ptr<memtable> mt2 = make_lw_shared<memtable>(s);
    mt1->apply(mut1);
    mt2->apply(mut2);

    write_and_compare_sstables(s, mt1, mt2, table_name);
}

static sstring get_read_index_test_path(sstring table_name) {
    return format("tests/sstables/3.x/uncompressed/read_{}", table_name);
}

static std::unique_ptr<index_reader> get_index_reader(shared_sstable sst) {
    return std::make_unique<index_reader>(sst, default_priority_class());
}

/*
 * The SSTables read is generated using the following queries:
 *
 *  CREATE TABLE empty_index (pk text, PRIMARY KEY (pk)) WITH compression = {'sstable_compression': ''};
 *  INSERT INTO empty_index (pk) VALUES ('привет');
*/

SEASTAR_THREAD_TEST_CASE(test_read_empty_index) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "empty_index";
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto sst = sstables::make_sstable(s, get_read_index_test_path(table_name), 1, sstable_version_types::mc , sstable_format_types::big);
    sst->load().get0();
    assert_that(get_index_reader(sst)).is_empty(*s);
}

/*
 * Test files taken from write_wide_partitions test
 */
SEASTAR_THREAD_TEST_CASE(test_read_rows_only_index) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "rows_only_index";
    // CREATE TABLE rows_only_index (pk text, ck text, st text, rc text, PRIMARY KEY (pk, ck) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck", utf8_type, column_kind::clustering_key);
    builder.with_column("st", utf8_type, column_kind::static_column);
    builder.with_column("rc", utf8_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto sst = sstables::make_sstable(s, get_read_index_test_path(table_name), 1, sstable_version_types::mc , sstable_format_types::big);
    sst->load().get0();
    assert_that(get_index_reader(sst)).has_monotonic_positions(*s);
}

/*
 * Test files taken from write_many_range_tombstones test
 */
SEASTAR_THREAD_TEST_CASE(test_read_range_tombstones_only_index) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "range_tombstones_only_index";
    // CREATE TABLE range_tombstones_only_index (pk text, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto sst = sstables::make_sstable(s, get_read_index_test_path(table_name), 1, sstable_version_types::mc , sstable_format_types::big);
    sst->load().get0();
    assert_that(get_index_reader(sst)).has_monotonic_positions(*s);
}

/*
 * Generated with the following code:
 *  query = SimpleStatement("""
	DELETE FROM range_tombstone_boundaries_index USING TIMESTAMP %(ts)s
	WHERE pk = 'key1' AND ck1 > %(a)s AND ck1 <= %(b)s
        """, consistency_level=ConsistencyLevel.ONE)

    for i in range(1024):
        session.execute(query, dict(a="%s%d" % ('a' * 1024, i), b="%s%d" %('a' * 1024, i + 1), ts=(1525385507816568 + i)))

 */
SEASTAR_THREAD_TEST_CASE(test_read_range_tombstone_boundaries_index) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "range_tombstone_boundaries_index";
    // CREATE TABLE range_tombstone_boundaries_index (pk text, ck1 text, ck2 text, PRIMARY KEY (pk, ck1, ck2) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", utf8_type, column_kind::partition_key);
    builder.with_column("ck1", utf8_type, column_kind::clustering_key);
    builder.with_column("ck2", utf8_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto sst = sstables::make_sstable(s, get_read_index_test_path(table_name), 1, sstable_version_types::mc , sstable_format_types::big);
    sst->load().get0();
    assert_that(get_index_reader(sst)).has_monotonic_positions(*s);
}

SEASTAR_THREAD_TEST_CASE(test_read_table_empty_clustering_key) {
    auto abj = defer([] { await_background_jobs().get(); });
    // CREATE TABLE empty_clustering_key (pk int, v int, PRIMARY KEY (pk)) with compression = {'sstable_compression': ''};
    schema_builder builder("sst3", "empty_clustering_key");
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("v", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    sstable_assertions sst(s, "tests/sstables/3.x/uncompressed/empty_clustering_key");
    sst.load();

    std::vector<dht::decorated_key> keys;
    for (int i = 0; i < 10; i++) {
        auto pk = partition_key::from_single_value(*s, int32_type->decompose(i));
        keys.emplace_back(dht::global_partitioner().decorate_key(*s, std::move(pk)));
    }
    dht::decorated_key::less_comparator cmp(s);
    std::sort(keys.begin(), keys.end(), cmp);

    assert_that(sst.read_rows_flat()).produces(keys);
}

/*
 * Test for a bug discovered with Scylla's compaction_history tables
 * containing complex columns with zero subcolumns.
 */
SEASTAR_THREAD_TEST_CASE(test_complex_column_zero_subcolumns_read) {
    using utils::UUID;
    const sstring path =
        "tests/sstables/3.x/uncompressed/complex_column_zero_subcolumns";

    schema_ptr s = schema_builder("test_ks", "test_table")
        .with_column("id", uuid_type, column_kind::partition_key)
        .with_column("bytes_in", long_type)
        .with_column("bytes_out", long_type)
        .with_column("columnfamily_name", utf8_type)
        .with_column("compacted_at", timestamp_type)
        .with_column("keyspace_name", utf8_type)
        .with_column("rows_merged", map_type_impl::get_instance(int32_type, long_type, true))
        .set_compressor_params(compression_parameters::no_compression())
        .build();

    sstable_assertions sst(s, path);
    sst.load();

    auto to_pkey = [&s] (const UUID& key) {
        auto bytes = uuid_type->decompose(key);
        auto pk = partition_key::from_single_value(*s, bytes);
        return dht::global_partitioner().decorate_key(*s, pk);
    };

    std::vector<UUID> keys {
        UUID{"09fea990-b320-11e8-83a7-000000000000"},
        UUID{"0a310430-b320-11e8-83a7-000000000000"},
        UUID{"0a214cc0-b320-11e8-83a7-000000000000"},
        UUID{"0a00a560-b320-11e8-83a7-000000000000"},
        UUID{"0a0a6960-b320-11e8-83a7-000000000000"},
        UUID{"0a147b80-b320-11e8-83a7-000000000000"},
        UUID{"0a187320-b320-11e8-83a7-000000000000"},
    };

    auto rd = sst.read_rows_flat();
    rd.set_max_buffer_size(1);
    auto r = assert_that(std::move(rd));
    for (const auto& key : keys) {
        r.produces_partition_start(to_pkey(key))
        .produces_row_with_key(clustering_key::make_empty())
        .produces_partition_end();
    }
    r.produces_end_of_stream();
}

SEASTAR_THREAD_TEST_CASE(test_uncompressed_read_two_rows_fast_forwarding) {
    auto abj = defer([] { await_background_jobs().get(); });
    // Following tests run on files in tests/sstables/3.x/uncompressed/read_two_rows_fast_forwarding
    // They were created using following CQL statements:
    //
    // CREATE TABLE two_rows_fast_forwarding (pk int, ck int, rc int, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    // INSERT INTO two_rows_fast_forwarding (pk, ck, rc) VALUES (0, 7, 7);
    // INSERT INTO two_rows_fast_forwarding (pk, ck, rc) VALUES (0, 8, 8);

    static const sstring path = "tests/sstables/3.x/uncompressed/read_two_rows_fast_forwarding";
    static thread_local const schema_ptr s =
        schema_builder("test_ks", "two_rows_fast_forwarding")
            .with_column("pk", int32_type, column_kind::partition_key)
            .with_column("ck", int32_type, column_kind::clustering_key)
            .with_column("rc", int32_type)
            .set_compressor_params(compression_parameters::no_compression())
            .build();
    sstable_assertions sst(s, path);
    sst.load();

    auto to_pkey = [&] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        auto pk = partition_key::from_single_value(*s, bytes);
        return dht::global_partitioner().decorate_key(*s, pk);
    };

    auto to_ckey = [&] (int key) {
        auto bytes = int32_type->decompose(int32_t(key));
        return clustering_key::from_single_value(*s, bytes);
    };

    auto rc_cdef = s->get_column_definition(to_bytes("rc"));
    BOOST_REQUIRE(rc_cdef);

    auto to_expected = [rc_cdef] (int val) {
        return std::vector<flat_reader_assertions::expected_column>{{rc_cdef, int32_type->decompose(int32_t(val))}};
    };

    auto r = assert_that(sst.read_range_rows_flat(query::full_partition_range,
                                                  s->full_slice(),
                                                  default_priority_class(),
                                                  no_resource_tracking(),
                                                  streamed_mutation::forwarding::yes));
    r.produces_partition_start(to_pkey(0))
        .produces_end_of_stream();

    r.fast_forward_to(to_ckey(2), to_ckey(3));
    r.produces_end_of_stream();

    r.fast_forward_to(to_ckey(4), to_ckey(5));
    r.produces_end_of_stream();

    r.fast_forward_to(to_ckey(6), to_ckey(9));
    r.produces_row(to_ckey(7), to_expected(7))
        .produces_row(to_ckey(8), to_expected(8))
        .produces_end_of_stream();
}

SEASTAR_THREAD_TEST_CASE(test_dead_row_marker) {
    api::timestamp_type ts = 1543494402386839;
    gc_clock::time_point tp = gc_clock::time_point{} + gc_clock::duration{1543494402};
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "dead_row_marker";
    // CREATE TABLE dead_row_marker (pk int, ck int, st int static, rc int , PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("st", int32_type, column_kind::static_column);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    auto key = partition_key::from_deeply_exploded(*s, { 1 });
    mutation mut{s, key};
    mut.set_static_cell("st", data_value{1135}, ts);

    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 2 });
    auto& clustered_row = mut.partition().clustered_row(*s, ckey);
    clustered_row.apply(row_marker{ts, gc_clock::duration{90} /* TTL */, tp});

    mut.set_cell(ckey, "rc", data_value{7777}, ts);

    mt->apply(mut);

    write_and_compare_sstables(s, mt, table_name);
}

SEASTAR_THREAD_TEST_CASE(test_shadowable_deletion) {
    /* The created SSTables content should match that of
     * an MV filled with the following queries:
     *
     * CREATE TABLE cf (p int PRIMARY KEY, v int) WITH compression = {'sstable_compression': ''};
     * CREATE MATERIALIZED VIEW mv AS SELECT * FROM cf WHERE p IS NOT NULL AND v IS NOT NULL PRIMARY KEY (v, p);
     * INSERT INTO cf (p, v) VALUES (1, 0);
     * UPDATE cf SET v = 1 WHERE p = 1;
     */
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "shadowable_deletion";
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 1 });
    mutation mut1{s, partition_key::from_deeply_exploded(*s, {1})};
    {
        auto& clustered_row = mut1.partition().clustered_row(*s, ckey);
        clustered_row.apply(row_marker{api::timestamp_type{1540230880370422}});
        mt->apply(mut1);
    }

    mutation mut2{s, partition_key::from_deeply_exploded(*s, {0})};
    {
        auto& clustered_row = mut2.partition().clustered_row(*s, ckey);
        api::timestamp_type ts {1540230874370065};
        gc_clock::time_point tp {gc_clock::duration(1540230880)};
        clustered_row.apply(row_marker{api::timestamp_type{ts}});
        clustered_row.apply(shadowable_tombstone(ts, tp));
        mt->apply(mut2);
    }

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut1, mut2});
}

SEASTAR_THREAD_TEST_CASE(test_regular_and_shadowable_deletion) {
    /* The created SSTables content should match that of
     * an MV filled with the following queries:
     *
     * CREATE TABLE cf (p INT, c INT, v INT, PRIMARY KEY (p, c));
     * CREATE MATERIALIZED VIEW mvf AS SELECT * FROM cf WHERE p IS NOT NULL AND c IS NOT NULL AND v IS NOT NULL PRIMARY KEY (v, p, c);
     * INSERT INTO cf (p, c, v) VALUES (1, 1, 0) USING TIMESTAMP 1540230874370001;
     * DELETE FROM cf USING TIMESTAMP 1540230874370001 WHERE p = 1 AND c = 1;
     * UPDATE cf USING TIMESTAMP 1540230874370002 SET v = 0 WHERE p = 1 AND c = 1;
     * UPDATE cf USING TIMESTAMP 1540230874370003 SET v = 1 WHERE p = 1 AND c = 1;
     */
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "regular_and_shadowable_deletion";
    schema_builder builder("sst3", table_name);
    builder.with_column("v", int32_type, column_kind::partition_key);
    builder.with_column("p", int32_type, column_kind::clustering_key);
    builder.with_column("c", int32_type, column_kind::clustering_key);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    auto make_tombstone = [] (int64_t ts, int32_t tp) {
        return tombstone{api::timestamp_type{ts}, gc_clock::time_point(gc_clock::duration(tp))};
    };

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { {1}, {1} });
    mutation mut1{s, partition_key::from_deeply_exploded(*s, {1})};
    {
        auto& clustered_row = mut1.partition().clustered_row(*s, ckey);
        clustered_row.apply(row_marker{api::timestamp_type{1540230874370003}});
        clustered_row.apply(make_tombstone(1540230874370001, 1540251167));
        mt->apply(mut1);
    }

    mutation mut2{s, partition_key::from_deeply_exploded(*s, {0})};
    {
        auto& clustered_row = mut2.partition().clustered_row(*s, ckey);
        gc_clock::time_point tp {gc_clock::duration(1540230880)};
        clustered_row.apply(row_marker{api::timestamp_type{1540230874370002}});
        clustered_row.apply(make_tombstone(1540230874370001, 1540251167));
        clustered_row.apply(shadowable_tombstone(make_tombstone(1540230874370002, 1540251216)));
        mt->apply(mut2);
    }

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut1, mut2});
}

SEASTAR_THREAD_TEST_CASE(test_write_static_row_with_missing_columns) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "static_row_with_missing_columns";
    // CREATE TABLE static_row (pk int, ck int, st1 int static, st2 int static, rc int, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("st1", int32_type, column_kind::static_column);
    builder.with_column("st2", int32_type, column_kind::static_column);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO static_row (pk, ck, st1, rc) VALUES (0, 1, 2, 3);
    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 1 });
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_static_cell("st1", data_value{2}, write_timestamp);
    mut.set_cell(ckey, "rc", data_value{3}, write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_interleaved_atomic_and_collection_columns) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "interleaved_atomic_and_collection_columns";
    // CREATE TABLE interleaved_atomic_and_collection_columns ( pk int, ck int, rc1 int, rc2 set<int>, rc3 int, rc4 set<int>,
    //     rc5 int, rc6 set<int>, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    auto set_of_ints_type = set_type_impl::get_instance(int32_type, true);
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("rc1", int32_type);
    builder.with_column("rc2", set_of_ints_type);
    builder.with_column("rc3", int32_type);
    builder.with_column("rc4", set_of_ints_type);
    builder.with_column("rc5", int32_type);
    builder.with_column("rc6", set_of_ints_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO interleaved_atomic_and_collection_columns (pk, ck, rc1, rc4, rc5)
    //     VALUES (0, 1, 2, {3, 4}, 5) USING TIMESTAMP 1525385507816568;
    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 1 });
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_cell(ckey, "rc1", data_value{2}, write_timestamp);

    set_type_impl::mutation set_values;
    set_values.tomb = tombstone {write_timestamp - 1, write_time_point};
    set_values.cells.emplace_back(int32_type->decompose(3), atomic_cell::make_live(*bytes_type, write_timestamp, bytes_view{}));
    set_values.cells.emplace_back(int32_type->decompose(4), atomic_cell::make_live(*bytes_type, write_timestamp, bytes_view{}));
    mut.set_clustered_cell(ckey, *s->get_column_definition("rc4"), set_of_ints_type->serialize_mutation_form(set_values));

    mut.set_cell(ckey, "rc5", data_value{5}, write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_static_interleaved_atomic_and_collection_columns) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "static_interleaved_atomic_and_collection_columns";
    // CREATE TABLE static_interleaved_atomic_and_collection_columns ( pk int, ck int, st1 int static,
    //     st2 set<int> static, st3 int static, st4 set<int> static, st5 int static, st6 set<int> static,
    //     PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    auto set_of_ints_type = set_type_impl::get_instance(int32_type, true);
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("st1", int32_type, column_kind::static_column);
    builder.with_column("st2", set_of_ints_type, column_kind::static_column);
    builder.with_column("st3", int32_type, column_kind::static_column);
    builder.with_column("st4", set_of_ints_type, column_kind::static_column);
    builder.with_column("st5", int32_type, column_kind::static_column);
    builder.with_column("st6", set_of_ints_type, column_kind::static_column);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);

    // INSERT INTO static_interleaved_atomic_and_collection_columns (pk, ck, st1, st4, st5)
    //     VALUES (0, 1, 2, {3, 4}, 5) USING TIMESTAMP 1525385507816568;
    auto key = partition_key::from_deeply_exploded(*s, {0});
    mutation mut{s, key};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 1 });
    mut.partition().apply_insert(*s, ckey, write_timestamp);
    mut.set_static_cell("st1", data_value{2}, write_timestamp);

    set_type_impl::mutation set_values;
    set_values.tomb = tombstone {write_timestamp - 1, write_time_point};
    set_values.cells.emplace_back(int32_type->decompose(3), atomic_cell::make_live(*bytes_type, write_timestamp, bytes_view{}));
    set_values.cells.emplace_back(int32_type->decompose(4), atomic_cell::make_live(*bytes_type, write_timestamp, bytes_view{}));
    mut.set_static_cell(*s->get_column_definition("st4"), set_of_ints_type->serialize_mutation_form(set_values));

    mut.set_static_cell("st5", data_value{5}, write_timestamp);
    mt->apply(mut);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut});
}

SEASTAR_THREAD_TEST_CASE(test_write_empty_static_row) {
    auto abj = defer([] { await_background_jobs().get(); });
    sstring table_name = "empty_static_row";
    // CREATE TABLE empty_static_row (pk int, ck int, st int static, rc int, PRIMARY KEY (pk, ck)) WITH compression = {'sstable_compression': ''};
    schema_builder builder("sst3", table_name);
    builder.with_column("pk", int32_type, column_kind::partition_key);
    builder.with_column("ck", int32_type, column_kind::clustering_key);
    builder.with_column("st", int32_type, column_kind::static_column);
    builder.with_column("rc", int32_type);
    builder.set_compressor_params(compression_parameters::no_compression());
    schema_ptr s = builder.build(schema_builder::compact_storage::no);

    lw_shared_ptr<memtable> mt = make_lw_shared<memtable>(s);
    api::timestamp_type ts = write_timestamp;

    // INSERT INTO empty_static_row (pk, ck, rc) VALUES ( 0, 1, 2) USING TIMESTAMP 1525385507816568;
    auto key1 = partition_key::from_deeply_exploded(*s, {0});
    mutation mut1{s, key1};
    clustering_key ckey = clustering_key::from_deeply_exploded(*s, { 1 });
    mut1.partition().apply_insert(*s, ckey, ts);
    mut1.set_cell(ckey, "rc", data_value{2}, ts);
    mt->apply(mut1);

    ts += 10;

    // INSERT INTO empty_static_row (pk, ck, st, rc) VALUES ( 1, 1, 2, 3) USING TIMESTAMP 1525385507816578;
    auto key2 = partition_key::from_deeply_exploded(*s, {1});
    mutation mut2{s, key2};
    mut2.partition().apply_insert(*s, ckey, ts);
    mut2.set_static_cell("st", data_value{2}, ts);
    mut2.set_cell(ckey, "rc", data_value{3}, ts);
    mt->apply(mut2);

    tmpdir tmp = write_and_compare_sstables(s, mt, table_name);
    validate_read(s, tmp.path, {mut2, mut1}); // Mutations are re-ordered according to decorated_key order
}

SEASTAR_THREAD_TEST_CASE(test_read_missing_summary) {
    auto abj = defer([] { await_background_jobs().get(); });
    const sstring path = "tests/sstables/3.x/uncompressed/read_missing_summary";
    const schema_ptr s =
        schema_builder("test_ks", "test_table")
            .with_column("pk", utf8_type, column_kind::partition_key)
            .with_column("ck", utf8_type, column_kind::clustering_key)
            .with_column("rc", utf8_type)
            .set_compressor_params(compression_parameters::no_compression())
            .build();

    sstable_assertions sst(s, path);
    sst.load();
}

