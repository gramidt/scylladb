# Copyright 2020-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# Tests for secondary indexes

import random
import itertools
import time
import tempfile
import pytest
import os
from cassandra.protocol import SyntaxException, AlreadyExists, InvalidRequest, ConfigurationException, ReadFailure, WriteFailure
from cassandra.query import SimpleStatement
from cassandra_tests.porting import assert_rows, assert_row_count, assert_rows_ignoring_order, assert_empty

from util import new_test_table, unique_name

# A reproducer for issue #7443: Normally, when the entire table is SELECTed,
# the partitions are returned sorted by the partitions' token. When there
# is filtering, this order is not expected to change. Furthermore, when this
# filtering happens to use a secondary index, again the order is not expected
# to change.
def test_partition_order_with_si(cql, test_keyspace):
    schema = 'pk int, x int, PRIMARY KEY ((pk))'
    with new_test_table(cql, test_keyspace, schema) as table:
        # Insert 20 partitions, all of them with x=1 so that filtering by x=1
        # will yield the same 20 partitions:
        N = 20
        stmt = cql.prepare('INSERT INTO '+table+' (pk, x) VALUES (?, ?)')
        for i in range(N):
            cql.execute(stmt, [i, 1])
        # SELECT all the rows, and verify they are returned in increasing
        # partition token order (note that the token is a *signed* number):
        tokens = [row.system_token_pk for row in cql.execute('SELECT token(pk) FROM '+table)]
        assert len(tokens) == N
        assert sorted(tokens) == tokens
        # Now select all the partitions with filtering of x=1. Since all
        # rows have x=1, this shouldn't change the list of matching rows, and
        # also shouldn't check their order:
        tokens1 = [row.system_token_pk for row in cql.execute('SELECT token(pk) FROM '+table+' WHERE x=1 ALLOW FILTERING')]
        assert tokens1 == tokens
        # Now add an index on x, which allows implementing the "x=1"
        # restriction differently. With the index, "ALLOW FILTERING" is
        # no longer necessary. But the order of the results should
        # still not change. Issue #7443 is about the order changing here.
        cql.execute('CREATE INDEX ON '+table+'(x)')
        # "CREATE INDEX" does not wait until the index is actually available
        # for use. Reads immediately after the CREATE INDEX may fail or return
        # partial results. So let's retry until reads resume working:
        for i in range(100):
            try:
                tokens2 = [row.system_token_pk for row in cql.execute('SELECT token(pk) FROM '+table+' WHERE x=1')]
                if len(tokens2) == N:
                    break
            except ReadFailure:
                pass
            time.sleep(0.1)
        assert tokens2 == tokens

# Test which ensures that indexes for a query are picked by the order in which
# they appear in restrictions. That way, users can deterministically pick
# which indexes are used for which queries.
# Note that the order of picking indexing is not set in stone and may be
# subject to change - in which case this test case should be amended as well.
# The order tested in this case was decided as a good first step in issue
# #7969, but it's possible that it will eventually be implemented another
# way, e.g. dynamically based on estimated query selectivity statistics.
# Ref: #7969
@pytest.mark.xfail(reason="The order of picking indexes is currently arbitrary. Issue #7969")
def test_order_of_indexes(scylla_only, cql, test_keyspace):
    schema = 'p int primary key, v1 int, v2 int, v3 int'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX my_v3_idx ON {table}(v3)")
        cql.execute(f"CREATE INDEX my_v1_idx ON {table}(v1)")
        cql.execute(f"CREATE INDEX my_v2_idx ON {table}((p),v2)")
        # All queries below should use the first index they find in the list
        # of restrictions. Tracing information will be consulted to ensure
        # it's true. Currently some of the cases below succeed, because the
        # order is not well defined (and may, for instance, change upon
        # server restart), but some of them fail. Once a proper ordering
        # is implemented, all cases below should succeed.
        def index_used(query, index_name):
            assert any([index_name in event.description for event in cql.execute(query, trace=True).one().get_query_trace().events])
        index_used(f"SELECT * FROM {table} WHERE v3 = 1", "my_v3_idx")
        index_used(f"SELECT * FROM {table} WHERE v3 = 1 and v1 = 2 allow filtering", "my_v3_idx")
        index_used(f"SELECT * FROM {table} WHERE p = 1 and v1 = 1 and v3 = 2 allow filtering", "my_v1_idx")
        index_used(f"SELECT * FROM {table} WHERE p = 1 and v3 = 1 and v1 = 2 allow filtering", "my_v3_idx")
        # Local indexes are still skipped if they cannot be used
        index_used(f"SELECT * FROM {table} WHERE v2 = 1 and v1 = 2 allow filtering", "my_v1_idx")
        index_used(f"SELECT * FROM {table} WHERE v2 = 1 and v3 = 2 and v1 = 3 allow filtering", "my_v3_idx")
        index_used(f"SELECT * FROM {table} WHERE v1 = 1 and v2 = 2 and v3 = 3 allow filtering", "my_v1_idx")
        # Local indexes are still preferred over global ones, if they can be used
        index_used(f"SELECT * FROM {table} WHERE p = 1 and v1 = 1 and v3 = 2 and v2 = 2 allow filtering", "my_v2_idx")
        index_used(f"SELECT * FROM {table} WHERE p = 1 and v2 = 1 and v1 = 2 allow filtering", "my_v2_idx")

# Indexes can be created without an explicit name, in which case a default name is chosen.
# However, due to #8620 it was possible to break the index creation mechanism by creating
# a properly named regular table, which conflicts with the generated index name.
def test_create_unnamed_index_when_its_name_is_taken(cql, test_keyspace):
    schema = 'p int primary key, v int'
    with new_test_table(cql, test_keyspace, schema) as table:
        try:
            cql.execute(f"CREATE TABLE {table}_v_idx_index (i_do_not_exist_in_the_base_table int primary key)")
            # Creating an index should succeed, even though its default name is taken
            # by the table above
            cql.execute(f"CREATE INDEX ON {table}(v)")
        finally:
            cql.execute(f"DROP TABLE {table}_v_idx_index")

# Indexed created with an explicit name cause a materialized view to be created,
# and this view has a specific name - <index-name>_index. If there happens to be
# a regular table (or another view) named just like that, index creation should fail.
def test_create_named_index_when_its_name_is_taken(scylla_only, cql, test_keyspace):
    schema = 'p int primary key, v int'
    with new_test_table(cql, test_keyspace, schema) as table:
        index_name = unique_name()
        try:
            cql.execute(f"CREATE TABLE {test_keyspace}.{index_name}_index (i_do_not_exist_in_the_base_table int primary key)")
            # Creating an index should fail, because it's impossible to create
            # its underlying materialized view, because its name is taken by a regular table
            with pytest.raises(InvalidRequest, match="already exists"):
                cql.execute(f"CREATE INDEX {index_name} ON {table}(v)")
        finally:
            cql.execute(f"DROP TABLE {test_keyspace}.{index_name}_index")

# Tests for CREATE INDEX IF NOT EXISTS
# Reproduces issue #8717.
def test_create_index_if_not_exists(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, 'p int primary key, v int') as table:
        cql.execute(f"CREATE INDEX ON {table}(v)")
        # Can't create the same index again without "IF NOT EXISTS", but can
        # do it with "IF NOT EXISTS":
        with pytest.raises(InvalidRequest, match="duplicate"):
            cql.execute(f"CREATE INDEX ON {table}(v)")
        cql.execute(f"CREATE INDEX IF NOT EXISTS ON {table}(v)")
        cql.execute(f"DROP INDEX {test_keyspace}.{table.split('.')[1]}_v_idx")

        # Now test the same thing for named indexes. This is what broke in #8717:
        cql.execute(f"CREATE INDEX xyz ON {table}(v)")
        with pytest.raises(InvalidRequest, match="already exists"):
            cql.execute(f"CREATE INDEX xyz ON {table}(v)")
        cql.execute(f"CREATE INDEX IF NOT EXISTS xyz ON {table}(v)")
        cql.execute(f"DROP INDEX {test_keyspace}.xyz")

        # Exactly the same with non-lower case name.
        cql.execute(f'CREATE INDEX "CamelCase" ON {table}(v)')
        with pytest.raises(InvalidRequest, match="already exists"):
            cql.execute(f'CREATE INDEX "CamelCase" ON {table}(v)')
        cql.execute(f'CREATE INDEX IF NOT EXISTS "CamelCase" ON {table}(v)')
        cql.execute(f'DROP INDEX {test_keyspace}."CamelCase"')

        # Trying to create an index for an attribute that's already indexed,
        # but with a different name. The "IF NOT EXISTS" appears to succeed
        # in this case, but does not actually create the new index name -
        # only the old one remains.
        cql.execute(f"CREATE INDEX xyz ON {table}(v)")
        with pytest.raises(InvalidRequest, match="duplicate"):
            cql.execute(f"CREATE INDEX abc ON {table}(v)")
        cql.execute(f"CREATE INDEX IF NOT EXISTS abc ON {table}(v)")
        with pytest.raises(InvalidRequest):
            cql.execute(f"DROP INDEX {test_keyspace}.abc")
        cql.execute(f"DROP INDEX {test_keyspace}.xyz")

# Another test for CREATE INDEX IF NOT EXISTS: Checks what happens if an index
# with the given *name* already exists, but it's a different index than the
# one requested, i.e.,
#    CREATE INDEX xyz ON tbl(a)
#    CREATE INDEX IF NOT EXIST xyz ON tbl(b)
# Should the second command
# 1. Silently do nothing (because xyz already exists),
# 2. or try to create an index (because an index on tbl(b) doesn't yet exist)
#    and visibly fail when it can't because the name is already taken?
# Cassandra chose the first behavior (silently do nothing), Scylla chose the
# second behavior. We consider Cassandra's behavior to be *wrong* and
# unhelpful - the intention of the user was ensure that an index tbl(b)
# (an index on column b of table tbl) exists, and if we can't, an error
# message is better than silently doing nothing.
# So this test is marked "cassandra_bug" - passes on Scylla and xfails on
# Cassandra.
# Reproduces issue #9182
def test_create_index_if_not_exists2(cql, test_keyspace, cassandra_bug):
    with new_test_table(cql, test_keyspace, 'p int primary key, v1 int, v2 int') as table:
        index_name = unique_name()
        cql.execute(f"CREATE INDEX {index_name} ON {table}(v1)")
        # Obviously can't create a different index with the same name:
        with pytest.raises(InvalidRequest, match="already exists"):
            cql.execute(f"CREATE INDEX {index_name} ON {table}(v2)")
        # Even with "IF NOT EXISTS" we still get a failure. An index for
        # {table}(v2) does not yet exist, so the index creation is attempted.
        with pytest.raises(InvalidRequest, match="already exists"):
            cql.execute(f"CREATE INDEX IF NOT EXISTS {index_name} ON {table}(v2)")

# Test that the paging state works properly for indexes on tables
# with descending clustering order. There was a problem with indexes
# created on clustering keys with DESC clustering order - they are represented
# as "reverse" types internally and Scylla assertions failed that the base type
# is different from the underlying view type, even though, from the perspective
# of deserialization, they're equal. Issue #8666
def test_paging_with_desc_clustering_order(cql, test_keyspace):
    schema = 'p int, c int, primary key (p,c)'
    extra = 'with clustering order by (c desc)'
    with new_test_table(cql, test_keyspace, schema, extra) as table:
        cql.execute(f"CREATE INDEX ON {table}(c)")
        for i in range(3):
            cql.execute(f"INSERT INTO {table}(p,c) VALUES ({i}, 42)")
        stmt = SimpleStatement(f"SELECT * FROM {table} WHERE c = 42", fetch_size=1)
        assert len([row for row in cql.execute(stmt)]) == 3

# Test that deleting a base partition works fine, even if it produces a large batch
# of individual view updates. Refs #8852 - view updates used to be applied with
# per-partition granularity, but after fixing the issue it's no longer the case,
# so a regression test is necessary. Scylla-only - relies on the underlying
# representation of the index table.
def test_partition_deletion(cql, test_keyspace, scylla_only):
    schema = 'p int, c1 int, c2 int, v int, primary key (p,c1,c2)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(c1)")
        prep = cql.prepare(f"INSERT INTO {table}(p,c1,c2) VALUES (1, ?, 1)")
        for i in range(1342):
            cql.execute(prep, [i])
        cql.execute(f"DELETE FROM {table} WHERE p = 1")
        res = [row for row in cql.execute(f"SELECT * FROM {table}_c1_idx_index")]
        assert len(res) == 0

# Test that deleting a clustering range works fine, even if it produces a large batch
# of individual view updates. Refs #8852 - view updates used to be applied with
# per-partition granularity, but after fixing the issue it's no longer the case,
# so a regression test is necessary. Scylla-only - relies on the underlying
# representation of the index table.
def test_range_deletion(cql, test_keyspace, scylla_only):
    schema = 'p int, c1 int, c2 int, v int, primary key (p,c1,c2)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(c1)")
        prep = cql.prepare(f"INSERT INTO {table}(p,c1,c2) VALUES (1, ?, 1)")
        for i in range(1342):
            cql.execute(prep, [i])
        cql.execute(f"DELETE FROM {table} WHERE p = 1 AND c1 > 5 and c1 < 846")
        res = [row.c1 for row in cql.execute(f"SELECT * FROM {table}_c1_idx_index")]
        assert sorted(res) == [x for x in range(1342) if x <= 5 or x >= 846]

# Reproduces #8627:
# Test that trying to insert a value for an indexed column that exceeds 64KiB fails,
# because this value is too large to be written as a key in the underlying index
@pytest.mark.xfail(reason="issue #8627")
def test_too_large_indexed_value(cql, test_keyspace):
    schema = 'p int, c int, v text, primary key (p,c)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(v)")
        big = 'x'*66536
        with pytest.raises(InvalidRequest, match='size'):
            cql.execute(f"INSERT INTO {table}(p,c,v) VALUES (0,1,'{big}')")

# Similar to the above test (test_too_large_indexed_value) but when indexing
# keys of collection. Modern Cassandra, and Scylla, allow collection keys
# and values to be up to 2GB, but the keys written to an index are limited
# to 64 KB. When a collection is indexed, the insertion of oversized elements
# should fail cleanly at the time of write.
# Reproduces #8627
@pytest.mark.xfail(reason="issue #8627")
def test_too_large_indexed_collection_value(cql, test_keyspace):
    schema = 'p int, c int, m map<text,text>, primary key (p,c)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(values(m))")
        cql.execute(f"CREATE INDEX ON {table}(keys(m))")
        big = 'x'*66536
        with pytest.raises(InvalidRequest, match='size'):
            cql.execute(f"INSERT INTO {table}(p,c,m) VALUES (0,1,{{'hi': '{big}'}})")
        with pytest.raises(InvalidRequest, match='size'):
            cql.execute(f"INSERT INTO {table}(p,c,m) VALUES (0,1,{{'{big}': 'hi'}})")

# Reproduces #8627:
# Same as test_too_large_indexed_value above, just check adding an index
# to a table with pre-existing data. The background index-building process
# cannot return an error to the user, but we do expect it to skip the
# problematic row and continue to complete the rest of the index build.
@pytest.mark.xfail(reason="issue #8627")
def test_too_large_indexed_value_build(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, 'p int primary key, v text') as table:
        # No index yet - a "big" value in v is perfectly fine:
        stmt = cql.prepare(f'INSERT INTO {table} (p,v) VALUES (?, ?)')
        for i in range(30):
            cql.execute(stmt, [i, str(i)])
        big = 'x'*66536
        cql.execute(stmt, [30, big])
        assert [(30,big)] == list(cql.execute(f'SELECT * FROM {table} WHERE p=30'))
        # Create an index on v as the new key. The background index-building
        # process should start promptly.
        cql.execute(f"CREATE INDEX ON {table}(v)")
        # If Scylla's view builder hangs or stops, there is no way to
        # tell this state apart from a view build that simply hasn't
        # completed yet (besides looking at the logs, which we don't).
        # This means, unfortunately, that a failure of this test is slow -
        # it needs to wait for a timeout.
        # However, today we are lucky (?) that the cql.execute(read, [big])
        # test also fails immediately on Scylla, so this test fails quickly.
        read = cql.prepare(f'SELECT * FROM {table} WHERE v = ?')
        start_time = time.time()
        while time.time() < start_time + 30:
            # The oversized "big" cannot be a key in the view, and
            # cannot be searched. Cassandra reports: "Index expression
            # values may not be larger than 64K".
            with pytest.raises(InvalidRequest):
                cql.execute(read, [big])
            # All the other keys should eventually be there
            c = 0
            for i in range(30):
                if list(cql.execute(read, [str(i)])):
                    c += 1
            if c == 30:
                break
            print(c)
            time.sleep(0.1)
        for i in range(30):
            assert list(cql.execute(read, [str(i)]))

# Selecting values using only clustering key should require filtering, but work correctly
# Reproduces issue #8991
def test_filter_cluster_key(cql, test_keyspace):
    schema = 'p int, c1 int, c2 int, primary key (p, c1, c2)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(c2)")
        cql.execute(f"INSERT INTO {table} (p, c1, c2) VALUES (0, 1, 1)")
        cql.execute(f"INSERT INTO {table} (p, c1, c2) VALUES (0, 0, 1)")
        
        stmt = SimpleStatement(f"SELECT c1, c2 FROM {table} WHERE c1 = 1 and c2 = 1 ALLOW FILTERING")
        rows = cql.execute(stmt)
        assert_rows(rows, [1, 1])

def test_multi_column_with_regular_index(cql, test_keyspace):
    """Reproduces #9085."""
    with new_test_table(cql, test_keyspace, 'p int, c1 int, c2 int, r int, primary key(p,c1,c2)') as tbl:
        cql.execute(f'CREATE INDEX ON {tbl}(r)')
        cql.execute(f'INSERT INTO {tbl}(p, c1, c2, r) VALUES (1, 1, 1, 0)')
        cql.execute(f'INSERT INTO {tbl}(p, c1, c2, r) VALUES (1, 1, 2, 1)')
        cql.execute(f'INSERT INTO {tbl}(p, c1, c2, r) VALUES (1, 2, 1, 0)')
        assert_rows(cql.execute(f'SELECT c1 FROM {tbl} WHERE (c1,c2)<(2,0) AND r=0 ALLOW FILTERING'), [1])
        assert_rows(cql.execute(f'SELECT c1 FROM {tbl} WHERE p=1 AND (c1,c2)<(2,0) AND r=0 ALLOW FILTERING'), [1])

# Test that indexing an *empty string* works as expected. There is nothing
# wrong or unusual about an empty string, and it should be supported just
# like any other string.
# Reproduces issue #9364
def test_index_empty_string(cql, test_keyspace):
    schema = 'p int, v text, primary key (p)'
    # Searching for v='' without an index (with ALLOW FILTERING), works
    # as expected:
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"INSERT INTO {table} (p, v) VALUES (1, 'hello')")
        cql.execute(f"INSERT INTO {table} (p, v) VALUES (2, '')")
        assert_rows(cql.execute(f"SELECT p FROM {table} WHERE v='' ALLOW FILTERING"), [2])
    # Now try the same thing with an index on v. ALLOW FILTERING should
    # no longer be needed, and the correct row should be found (in #9364
    # it wasn't). We create here a new table instead of adding an index to
    # the existing table to avoid the question of how will we know when the
    # new index is ready.
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(v)")
        cql.execute(f"INSERT INTO {table} (p, v) VALUES (1, 'hello')")
        cql.execute(f"INSERT INTO {table} (p, v) VALUES (2, '')")
        # The following assert fails in #9364:
        # Note that on a single-node cql-pytest, index updates are
        # synchronous so we don't have to retry the SELECT.
        assert_rows(cql.execute(f"SELECT p FROM {table} WHERE v=''"), [2])

# Test that trying to delete an entry based on an indexed column
# does not cause the whole partition to be wiped. Refs #9495
def test_try_deleting_based_on_index_column(cql, test_keyspace):
    schema = 'p int, c1 int, c2 int, v int, primary key (p, c1, c2)'
    with new_test_table(cql, test_keyspace, schema) as table:
        for i in range(10):
            cql.execute(f"INSERT INTO {table} (p,c1,c2,v) VALUES (0,{i},{i},{i})")
        with pytest.raises(InvalidRequest):
            cql.execute(f"DELETE FROM {table} WHERE p = 0 AND c2 = 1500")
        assert_row_count(cql.execute(f"SELECT v FROM {table}"), 10)
        cql.execute(f"CREATE INDEX ON {table}(c2)")
        # Creating an index should *not* influence the fact that deletion
        # is not allowed
        with pytest.raises(InvalidRequest):
            cql.execute(f"DELETE FROM {table} WHERE p = 0 AND c2 = 1500")
        assert_row_count(cql.execute(f"SELECT v FROM {table}"), 10)

# Reproducer for issue #3403: A column name may contain all sorts of
# non-alphanumeric characters, including even "/". When an index is created,
# its default name - composed from the column name - may contain these
# characters and lead to problems - or at worst access to unintended file
# by using things like "../../.." in the column name.
def test_index_weird_chars_in_col_name(cql, test_keyspace):
    with tempfile.TemporaryDirectory() as tmpdir:
        # When issue #3403 exists, column name ../../...../tmpdir/x_yz will
        # cause Scylla to create the new index in tmpdir!
        # Of course a more sinister attacker can cause Scylla to create
        # directories anywhere in the file system - or to crash if the
        # directory creation fails - e.g., if magic_path ends in
        # /dev/null/hello, and /dev/null is not a directory
        magic_path='/..'*20 + tmpdir + '/x_yz'
        schema = f'pk int PRIMARY KEY, "{magic_path}" int'
        with new_test_table(cql, test_keyspace, schema) as table:
            cql.execute(f'CREATE INDEX ON {table}("{magic_path}")')
            # Creating the index should not have miraculously created
            # something in tmpdir! If it has, we have issue #3403.
            assert os.listdir(tmpdir) == []
            # Check that the expected index name was chosen - based on
            # only the alphanumeric/underscore characters of the column name.
            ks, cf = table.split('.')
            index_name = list(cql.execute(f"SELECT index_name FROM system_schema.indexes WHERE keyspace_name = '{ks}' AND table_name = '{cf}'"))[0].index_name
            iswordchar = lambda x: str.isalnum(x) or x == '_'
            cleaned_up_column_name = ''.join(filter(iswordchar, magic_path))
            assert index_name == cf + '_' + cleaned_up_column_name + '_idx'

# Cassandra does not allow IN restrictions on non-primary-key columns,
# and Scylla does (see test_filtering.py::test_filter_in_restriction).
# However Scylla currently allows this only with ALLOW FILTERING.
# In theory, on an index column we could allow it also without filtering,
# just like we allow it on the partition key. But currently we don't
# so the following test is marked xfail. It's also cassandra_bug because
# Cassandra doesn't support it either (it gives the message "not yet
# supported" suggesting it may be fixed in the future).
@pytest.mark.xfail
def test_index_in_restriction(cql, test_keyspace, cassandra_bug):
    schema = 'pk int, ck int, x int, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f'CREATE INDEX ON {table}(x)')
        stmt = cql.prepare(f'INSERT INTO {table} (pk, ck, x) VALUES (?, ?, ?)')
        for i in range(3):
            cql.execute(stmt, [1, i, i*2])
        assert [(1,), (2,)] == list(cql.execute(f'SELECT ck FROM {table} WHERE x IN (2, 4)'))
        assert [(1,)] == list(cql.execute(f'SELECT ck FROM {table} WHERE x IN (2, 7)'))
        assert [] == list(cql.execute(f'SELECT ck FROM {table} WHERE x IN (3, 7)'))

# Test that a LIMIT works correctly in conjunction filtering - with an
# without a secondary index. The LIMIT is supposed to control the number
# of rows that the query returns - after filtering - not the number of
# rows before filtering.
# Reproduces #10649 - when use_index=True the test failed because LIMIT
# returned fewer than the requested number of rows.
@pytest.mark.parametrize("use_index", [
        pytest.param(True, marks=pytest.mark.xfail(reason="#10649")), False])
def test_filter_and_limit(cql, test_keyspace, use_index, driver_bug_1):
    with new_test_table(cql, test_keyspace, 'pk int primary key, x int, y int') as table:
        if use_index:
            cql.execute(f'CREATE INDEX ON {table}(x)')
        stmt = cql.prepare(f'INSERT INTO {table} (pk, x, y) VALUES (?, ?, ?)')
        cql.execute(stmt, [0, 1, 0])
        cql.execute(stmt, [1, 1, 1])
        cql.execute(stmt, [2, 1, 0])
        cql.execute(stmt, [3, 1, 1])
        cql.execute(stmt, [4, 1, 0])
        cql.execute(stmt, [5, 1, 1])
        cql.execute(stmt, [6, 1, 0])
        cql.execute(stmt, [7, 1, 1])
        results = list(cql.execute(f'SELECT pk FROM {table} WHERE x = 1 AND y = 0 ALLOW FILTERING'))
        assert sorted(results) == [(0,), (2,), (4,), (6,)]
        # Make sure that with LIMIT 3 we get back exactly 3 results - not
        # less and also not more.
        results = list(cql.execute(f'SELECT pk FROM {table} WHERE x = 1 AND y = 0 LIMIT 3 ALLOW FILTERING'))
        assert sorted(results) == [(0,), (2,), (4,)]
        # Make the test even harder (exercising more code paths) by asking
        # to fetch the 3 results in tiny one-result pages instead of one page.
        s = cql.prepare(f'SELECT pk FROM {table} WHERE x = 1 AND y = 0 LIMIT 3 ALLOW FILTERING')
        s.fetch_size = 1
        assert sorted(cql.execute(s)) == [(0,), (2,), (4,)]

# The following test is similar to the previous one (test_filter_and_limit)
# with one main difference: Whereas in the previous test the table's schema
# had only a partition key, here we also add a clustering key.
# This variantion in the test is important because Scylla's index-using code
# has a different code path for the case that the index lookup results in a
# list of matching partitions (the previous test) and when it results in a
# list of matching rows (this test).
# Reproduces #10649 - when use_index=True the test failed because LIMIT
# returned fewer than the requested number of rows.
@pytest.mark.parametrize("use_index", [
        pytest.param(True, marks=pytest.mark.xfail(reason="#10649")), False])
def test_filter_and_limit_clustering(cql, test_keyspace, use_index):
    with new_test_table(cql, test_keyspace, 'pk int, ck int, x int, PRIMARY KEY(pk, ck)') as table:
        if use_index:
            cql.execute(f'CREATE INDEX ON {table}(x)')
        stmt = cql.prepare(f'INSERT INTO {table} (pk, ck, x) VALUES (?, ?, ?)')
        cql.execute(stmt, [0, 0, 1])
        cql.execute(stmt, [1, 1, 1])
        cql.execute(stmt, [2, 0, 1])
        cql.execute(stmt, [3, 1, 1])
        cql.execute(stmt, [4, 0, 1])
        cql.execute(stmt, [5, 1, 1])
        cql.execute(stmt, [6, 0, 1])
        cql.execute(stmt, [7, 1, 1])
        results = list(cql.execute(f'SELECT pk FROM {table} WHERE x = 1 AND ck = 0 ALLOW FILTERING'))
        assert sorted(results) == [(0,), (2,), (4,), (6,)]
        # Make sure that with LIMIT 3 we get back exactly 3 results - not
        # less and also not more.
        results = list(cql.execute(f'SELECT pk FROM {table} WHERE x = 1 AND ck = 0 LIMIT 3 ALLOW FILTERING'))
        assert sorted(results) == [(0,), (2,), (4,)]
        # Make the test even harder (exercising more code paths) by asking
        # to fetch the 3 results in tiny one-result pages instead of one page.
        s = cql.prepare(f'SELECT pk FROM {table} WHERE x = 1 AND ck = 0 LIMIT 3 ALLOW FILTERING')
        s.fetch_size = 1
        assert sorted(cql.execute(s)) == [(0,), (2,), (4,)]

# Another reproducer for #10649, similar to the previous test
# (test_filter_and_limit_clustering) but indexes the clustering
# key column. This test is closer to the use case of the original user
# who discovered #10649, and involves slightly different code paths in
# Scylla (the index-driver query needs to read individual rows, not
# entire partitions, from the base table).
@pytest.mark.xfail(reason="#10649")
def test_filter_and_limit_2(cql, test_keyspace):
    schema = 'pk int, ck1 int, ck2 int, x int, PRIMARY KEY (pk, ck1, ck2)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f'CREATE INDEX ON {table}(ck2)')
        stmt = cql.prepare(f'INSERT INTO {table} (pk, ck1, ck2, x) VALUES (?, ?, ?, ?)')
        N = 10
        J = 3
        for i in range(N):
            for j in range(J):
                cql.execute(stmt, [1, i, j, j+i%2])
        results = list(cql.execute(f'SELECT ck1 FROM {table} WHERE ck2 = 2 AND x = 3 ALLOW FILTERING'))
        # Note in the data-adding loop above, all rows match our pk=1, and
        # when ck=2 it means j=2 and at that point - x=3 if i%2==1. So the
        # expected results are:
        assert results == [(i,) for i in range(N) if i%2==1]
        for i in [3, 1, N]:
            assert results[0:i] == list(cql.execute(f'SELECT ck1 FROM {table} WHERE ck2 = 2 AND x = 3 LIMIT {i} ALLOW FILTERING'))
        # Try exactly the same with adding pk=1, which shouldn't change
        # anything in the result (because all our rows have pk=1).
        for i in [3, 1, N]:
            assert results[0:i] == list(cql.execute(f'SELECT ck1 FROM {table} WHERE pk = 1 AND ck2 = 2 AND x = 3 LIMIT {i} ALLOW FILTERING'))

# Tests for issue #2962 - different type of indexing on collection columns.
# Note that we also have a randomized test for this feature as a C++ unit
# tests, as well as many tests translated from Cassandra's unit tests (grep
# the issue number #2962 to find them). Unlike the randomized test, the goal
# here is to try to cover as many corner cases we can think of, explicitly.
#
# Note that we can assume that on a single-node cql-pytest, materialized view
# (and therefore index) updates are synchronous, so none of these tests need
# loops to wait for a change to be indexed.

def test_index_list(cql, test_keyspace):
    schema = 'pk int, ck int, l list<int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        # Without an index, the "CONTAINS" restriction requires filtering
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 3')
        cql.execute(f'CREATE INDEX ON {table}(l)')
        # The list is still empty, nothing should match
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 3'))
        # Add a values to the list, check they can be found
        cql.execute(f'UPDATE {table} set l = l + [7] WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 7'))
        cql.execute(f'UPDATE {table} set l = l + [8] WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 8'))
        # Remove values from the list, check they can no longer be found
        cql.execute(f'UPDATE {table} set l = l - [7] WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 8'))
        cql.execute(f'UPDATE {table} set l = l - [8] WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 8'))
        # Replace entire value of list, everything in it should be indexed
        cql.execute(f'INSERT INTO {table} (pk, ck, l) VALUES (1, 2, [4, 5])')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 4'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 5'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 8'))
        cql.execute(f'UPDATE {table} set l = [2, 5] WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 2'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 5'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 4'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 8'))
        # A list can have the same value more than once, here we append
        # another 2 to the list which will now contain [2, 5, 2]. Searching
        # for 2, we should find this row, but only once.
        cql.execute(f'UPDATE {table} set l = l + [2] WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 2'))
        # If we remove the first element from the list [2, 5, 2], there is
        # still a 2 remaining, so it should still be found:
        cql.execute(f'DELETE l[0] FROM {table} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 2'))
        # Removing the other 2 (now l=[5,2]) should leaving a search for 2
        # returning nothing:
        cql.execute(f'DELETE l[1] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 2'))
        # The list is now [5]. Replace the 5 with 2 and see 2 is found, 5 not:
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 5'))
        cql.execute(f'UPDATE {table} set l[0] = 2 WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 2'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 5'))
        # Replacing the list [2] with an empty list works as expected:
        cql.execute(f'UPDATE {table} SET l = [] WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE l CONTAINS 2'))

def test_index_set(cql, test_keyspace):
    schema = 'pk int, ck int, s set<int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        # Without an index, the "CONTAINS" restriction requires filtering
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 3')
        cql.execute(f'CREATE INDEX ON {table}(s)')
        # The set is still empty, nothing should match
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 3'))
        # Add a values to the set, check they can be found
        cql.execute(f'UPDATE {table} set s = s + {{7}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 7'))
        cql.execute(f'UPDATE {table} set s = s + {{8}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 8'))
        # Remove values from the set, check they can no longer be found
        cql.execute(f'UPDATE {table} set s = s - {{7}} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 8'))
        cql.execute(f'UPDATE {table} set s = s - {{8}} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 8'))
        # Replace entire value of set, everything in it should be indexed
        cql.execute(f'INSERT INTO {table} (pk, ck, s) VALUES (1, 2, {{4, 5}})')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 4'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 5'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 8'))
        cql.execute(f'UPDATE {table} set s = {{2, 5}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 2'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 5'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 4'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 8'))
        # A set can only have the same value once. The set is now {2,5},
        # trying to add 2 again makes no diffence - and removing it just once
        # will remove this value:
        cql.execute(f'UPDATE {table} set s = s + {{2}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 2'))
        cql.execute(f'UPDATE {table} set s = s - {{2}} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 2'))
        # The set is now {5}. Replacing it with an empty set works as expected:
        cql.execute(f'UPDATE {table} SET s = {{}} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 5'))
        # The DELETE operaiton does the same thing:
        cql.execute(f'UPDATE {table} SET s = {{17,18}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 17'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 18'))
        cql.execute(f'DELETE s FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 17'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE s CONTAINS 18'))

def test_index_map_values(cql, test_keyspace):
    schema = 'pk int, ck int, m map<int,int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        # Without an index, the "CONTAINS" restriction requires filtering
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3')
        cql.execute(f'CREATE INDEX ON {table}(m)')
        # indexing m (same as values(m)) will allow CONTAINS but not CONTAINS KEY
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 3')
        # The map is still empty, nothing should match
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3'))
        # Add a elements to the map, check their values can be found
        cql.execute(f'UPDATE {table} set m = m + {{10: 7}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 7'))
        cql.execute(f'UPDATE {table} set m = m + {{11: 8}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 8'))
        # Remove elements from the map, check they can no longer be found
        cql.execute(f'DELETE m[10] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 8'))
        cql.execute(f'DELETE m[11] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 8'))
        # Replace entire value of map, everything in it should be indexed
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 2, {{17: 4, 18: 5}})')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 4'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 5'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 8'))
        cql.execute(f'UPDATE {table} set m = {{3: 2, 4: 5}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 2'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 5'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 4'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 8'))
        # A map can have multiple elements with the same *value*. The list is now
        # {3:2, 4:5}, if we add another element of value 2, searching for value 2
        # will return the item only once. But we'll need to delete both elements
        # to no longer find the value 2:
        cql.execute(f'UPDATE {table} set m = m + {{14: 2}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 2'))
        cql.execute(f'DELETE m[3] FROM {table} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 2'))
        cql.execute(f'DELETE m[14] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 2'))
        # The map is now {4:5}. Change the value of 4 and see it take effect:
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 5'))
        cql.execute(f'UPDATE {table} set m[4] = 6 WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 5'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 6'))
        # The "-" operation also works as expected (it removes specific *key*,
        # not *values*, depsite what some confused documentation claims):
        cql.execute(f'UPDATE {table} set m = m - {{4}} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 6'))

# In the previous test (test_index_map_values) we noted that if one map has
# several keys with the same value, then the "values(m)" index must store
# all all them, so that we can still match this value even after removing one
# of those keys. We tested in the previous test that although the same value
# appears more than once, when we search for it, we only get the same item
# once. Under the hood, Scylla does find the same value multiple times, but
# then eliminates the duplicate matched raw and returns it only once.
# There is a complication, that this de-duplication does not easily span
# *paging*. So the purpose of this test is to check that paging does not
# cause the same row to be returned more than once.
def test_index_map_values_paging(cql, test_keyspace):
    schema = 'pk int, ck int, m map<int,int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        # index m (same as values(m)). Will allow "CONTAINS".
        cql.execute(f'CREATE INDEX ON {table}(m)')
        # Insert a map where 10 out of the 12 elements have the same value 3
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 2, {{0:4, 1:3, 2:3, 3:3, 4:3, 5:3, 6:3, 7:3, 8:3, 9:3, 10:3, 11:7}})')
        # But looking for "m CONTAINS 3" should return the row (1,2) only once:
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3'))
        # Try exactly the same with paging with small page sizes. If Scylla
        # doesn't de-duplicate the results between pages, the same row will
        # be returned multiple times.
        for page_size in [1, 2, 3, 7]:
            stmt = SimpleStatement(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3', fetch_size=page_size)
            assert [(1,2)] == list(cql.execute(stmt))

# In the previous test (test_index_map_values*) all tests involved a single
# row, which could match a search, or not. In this test we verify that the
# case of multiple matching rows also works.
def test_index_map_values_multiple_matching_rows(cql, test_keyspace, driver_bug_1):
    schema = 'pk int, ck int, m map<int,int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        # index m (same as values(m)). Will allow "CONTAINS".
        cql.execute(f'CREATE INDEX ON {table}(m)')
        # Insert several rows with several different maps, some of them have
        # the value 3 in them somewhere, others don't. One of the maps has
        # multiple occurances of the value 3, so we also reproduce here the
        # same bug that test_index_map_values_paging() reproduces.
        # Note: Scylla needs to skip a duplicate 3 value in (2,4) which
        # results in an empty page in the result set when page_size=1. We
        # need the driver to correctly support this, hence the "driver_bug_1".
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 2, {{1:2, 3:4}})')
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 3, {{1:3, 3:4}})')
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 4, {{7:3}})')
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (2, 2, {{1:3, 2:3, 3:4}})')
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (2, 4, {{}})')
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (2, 5, {{7:3}})')
        assert [(1,3),(1,4),(2,2),(2,5)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3'))
        for page_size in [1, 2, 3, 7]:
            stmt = SimpleStatement(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3', fetch_size=page_size)
            assert [(1,3),(1,4),(2,2),(2,5)] == list(cql.execute(stmt))

# In the previous tests (test_index_map_values*) all tests involved a base
# table with both partition keys and clustering keys. Because some of the
# implementation is different depending the schema has clustering keys,
# let's also write a similar test with just a partition key:
def test_index_map_values_partition_key_only(cql, test_keyspace, driver_bug_1):
    schema = 'pk int, m map<int,int>, PRIMARY KEY (pk)'
    with new_test_table(cql, test_keyspace, schema) as table:
        # index m (same as values(m)). Will allow "CONTAINS".
        cql.execute(f'CREATE INDEX ON {table}(m)')
        # Insert several rows with several different maps, some of them have
        # the value 3 in them somewhere, others don't. One of the maps has
        # multiple occurances of the value 3, so we also reproduce here the
        # same bug that test_index_map_values_paging() reproduces (and here
        # test its intersection with the case of no clustering key).
        cql.execute(f'INSERT INTO {table} (pk, m) VALUES (1, {{1:2, 3:4}})')
        cql.execute(f'INSERT INTO {table} (pk, m) VALUES (2, {{1:3, 3:4}})')
        cql.execute(f'INSERT INTO {table} (pk, m) VALUES (3, {{7:3}})')
        cql.execute(f'INSERT INTO {table} (pk, m) VALUES (4, {{1:3, 2:3, 3:4}})')
        cql.execute(f'INSERT INTO {table} (pk, m) VALUES (5, {{}})')
        assert [(2,), (3,), (4,)] == sorted(cql.execute(f'SELECT pk FROM {table} WHERE m CONTAINS 3'))
        for page_size in [1, 2, 3, 7]:
            stmt = SimpleStatement(f'SELECT pk FROM {table} WHERE m CONTAINS 3', fetch_size=page_size)
            assert [(2,), (3,), (4,)] == sorted(cql.execute(stmt))
            # I wanted to check here that page_size is actually obeyed,
            # but we can't - when Scylla skips one of the duplicate values
            # it can result in a smaller page, and while not great (Cassandra
            # doesn't do it, all its pages are full size), it's legal.

def test_index_map_keys(cql, test_keyspace):
    schema = 'pk int, ck int, m map<int,int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        # Without an index, the "CONTAINS KEY" restriction requires filtering
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 3')
        cql.execute(f'CREATE INDEX ON {table}(keys(m))')
        # indexing keys(m) will allow CONTAINS KEY but not CONTAINS
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3')
        # The map is still empty, nothing should match
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 3'))
        # Add a elements to the map, check their keys can be found
        cql.execute(f'UPDATE {table} set m = m + {{10: 7}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 10'))
        cql.execute(f'UPDATE {table} set m = m + {{11: 8}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 11'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 10'))
        # Remove elements from the map, check they can no longer be found
        cql.execute(f'DELETE m[10] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 10'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 11'))
        cql.execute(f'DELETE m[11] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 10'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 11'))
        # Replace entire value of map, everything in it should be indexed
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 2, {{17: 4, 18: 5}})')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 17'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 18'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 10'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 11'))
        cql.execute(f'UPDATE {table} set m = {{3: 2, 4: 5}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 3'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 4'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 17'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 18'))
        # The map is now {3:2, 4:5}. Change the value of 4 and see see it has
        # no effect on keys:
        cql.execute(f'UPDATE {table} set m[4] = 6 WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 3'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 4'))
        # The "-" operation also works as expected (it removes specific *key*,
        # not *values*, depsite what some confused documentation claims):
        cql.execute(f'UPDATE {table} set m = m - {{4}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 3'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 4'))

def test_index_map_entries(cql, test_keyspace):
    schema = 'pk int, ck int, m map<int,int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f'CREATE INDEX ON {table}(entries(m))')
        # indexing entires(m) will allow neither CONTAINS KEY nor CONTAINS
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 3')
        with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
            cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 3')
        # The map is still empty, nothing should match
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[1] = 2'))
        # Add a elements to the map, check their keys can be found
        cql.execute(f'UPDATE {table} set m = m + {{10: 7}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[10] = 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[10] = 8'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[11] = 7'))
        cql.execute(f'UPDATE {table} set m = m + {{11: 8}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[10] = 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[11] = 8'))
        # Remove elements from the map, check they can no longer be found
        cql.execute(f'DELETE m[10] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[10] = 7'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[11] = 8'))
        cql.execute(f'DELETE m[11] FROM {table} WHERE pk=1 AND ck=2')
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[10] = 7'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[11] = 8'))
        # Replace entire value of map, everything in it should be indexed
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 2, {{17: 4, 18: 5}})')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[17] = 4'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[18] = 5'))
        cql.execute(f'UPDATE {table} set m = {{3: 2, 4: 5}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[3] = 2'))
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[4] = 5'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[17] = 4'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[18] = 5'))
        # The map is now {3:2, 4:5}. Change the value of 4 and see see it has
        # the expected effect
        cql.execute(f'UPDATE {table} set m[4] = 6 WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[4] = 6'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[4] = 5'))
        # The "-" operation also works as expected (it removes specific *key*,
        # not *values*, depsite what some confused documentation claims):
        cql.execute(f'UPDATE {table} set m = m - {{4}} WHERE pk=1 AND ck=2')
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[3] = 2'))
        assert [] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[4] = 6'))

# Check that it is possible to index the same map column in different ways
# (values, keys and entries) at the same time:
def test_index_map_multiple(cql, test_keyspace):
    schema = 'pk int, ck int, m map<int,int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f'CREATE INDEX ON {table}(values(m))')
        cql.execute(f'CREATE INDEX ON {table}(keys(m))')
        cql.execute(f'CREATE INDEX ON {table}(entries(m))')
        cql.execute(f'INSERT INTO {table} (pk, ck, m) VALUES (1, 2, {{17: 4, 18: 5}})')
        # values(m) can do this:
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS 4'))
        # keys(m) can do this:
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m CONTAINS KEY 18'))
        # entries(m) can do this:
        assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE m[18] = 5'))

# Test that indexing keys(x), values(x) or entries(x) is only allowed for
# specific column types (namely, specific kinds of collections).
def test_index_collection_wrong_type(cql, test_keyspace):
    schema = 'pk int primary key, x int, l list<int>, s set<int>, m map<int,int>, t tuple<int,int>'
    with new_test_table(cql, test_keyspace, schema) as table:
        # scalar type: only the bare column name is allowed:
        cql.execute(f'CREATE INDEX ON {table}(x)')
        with pytest.raises(InvalidRequest, match="keys()"):
            cql.execute(f'CREATE INDEX ON {table}(keys(x))')
        with pytest.raises(InvalidRequest, match="entries()"):
            cql.execute(f'CREATE INDEX ON {table}(entries(x))')
        with pytest.raises(InvalidRequest, match="values()"):
            cql.execute(f'CREATE INDEX ON {table}(values(x))')
        # list: bare column name and values() is allowed. Both of these
        # refer to the same index - so you can't have both at the same time.
        cql.execute(f'CREATE INDEX ON {table}(l)')
        with pytest.raises(InvalidRequest, match="keys()"):
            cql.execute(f'CREATE INDEX ON {table}(keys(l))')
        with pytest.raises(InvalidRequest, match="entries()"):
            cql.execute(f'CREATE INDEX ON {table}(entries(l))')
        with pytest.raises(InvalidRequest, match="duplicate"):
            cql.execute(f'CREATE INDEX ON {table}(values(l))')
        cql.execute(f"DROP INDEX {test_keyspace}.{table.split('.')[1]}_l_idx")
        cql.execute(f'CREATE INDEX ON {table}(values(l))')
        # set: bare column name and values() is allowed. Both of these
        # refer to the same index - so you can't have both at the same time.
        cql.execute(f'CREATE INDEX ON {table}(s)')
        with pytest.raises(InvalidRequest, match="keys()"):
            cql.execute(f'CREATE INDEX ON {table}(keys(s))')
        with pytest.raises(InvalidRequest, match="entries()"):
            cql.execute(f'CREATE INDEX ON {table}(entries(s))')
        with pytest.raises(InvalidRequest, match="duplicate"):
            cql.execute(f'CREATE INDEX ON {table}(values(s))')
        cql.execute(f"DROP INDEX {test_keyspace}.{table.split('.')[1]}_s_idx")
        cql.execute(f'CREATE INDEX ON {table}(values(s))')
        # map: bare column name, values(), keys() and entries() are all
        # allowed. The first tworefer to the same index - so you can't have
        # both at the same time.
        cql.execute(f'CREATE INDEX ON {table}(m)')
        cql.execute(f'CREATE INDEX ON {table}(keys(m))')
        cql.execute(f'CREATE INDEX ON {table}(entries(m))')
        with pytest.raises(InvalidRequest, match="duplicate"):
            cql.execute(f'CREATE INDEX ON {table}(values(m))')
        cql.execute(f"DROP INDEX {test_keyspace}.{table.split('.')[1]}_m_idx")
        cql.execute(f'CREATE INDEX ON {table}(values(m))')
        # A tuple is not a collection, and doesn't support indexing its
        # elements separately (this would not have been possible, by the way,
        # because it can have elements of different types).
        cql.execute(f'CREATE INDEX ON {table}(t)')
        with pytest.raises(InvalidRequest, match="keys()"):
            cql.execute(f'CREATE INDEX ON {table}(keys(t))')
        with pytest.raises(InvalidRequest, match="entries()"):
            cql.execute(f'CREATE INDEX ON {table}(entries(t))')
        with pytest.raises(InvalidRequest, match="values()"):
            cql.execute(f'CREATE INDEX ON {table}(values(t))')
        # None of the above types allow a FULL index - it is only
        # allowed for *frozen* collections.
        with pytest.raises(InvalidRequest, match="full()"):
            cql.execute(f'CREATE INDEX ON {table}(full(t))')
        with pytest.raises(InvalidRequest, match="full()"):
            cql.execute(f'CREATE INDEX ON {table}(full(l))')
        with pytest.raises(InvalidRequest, match="full()"):
            cql.execute(f'CREATE INDEX ON {table}(full(m))')
        with pytest.raises(InvalidRequest, match="full()"):
            cql.execute(f'CREATE INDEX ON {table}(full(s))')
        with pytest.raises(InvalidRequest, match="full()"):
            cql.execute(f'CREATE INDEX ON {table}(full(t))')

# Check the default name of collection indexes. This "default name" is
# needed to drop an index which was created without explicitly specifying
# a name for it. We want the default name to be identical to Cassandra,
# because an application may assume it is so.
def test_index_collection_default_name(cql, test_keyspace):
    schema = 'pk int primary key, m map<int,int>'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f'CREATE INDEX ON {table}(m)')
        cql.execute(f"DROP INDEX {table}_m_idx")
        # values(m) and m refer to the same thing so must have the same
        # default name.
        cql.execute(f'CREATE INDEX ON {table}(values(m))')
        cql.execute(f"DROP INDEX {table}_m_idx")
        # key(m) and entries(m) are different indexes than just m, but
        # their default index name turns out to be exactly the same one:
        cql.execute(f'CREATE INDEX ON {table}(keys(m))')
        cql.execute(f"DROP INDEX {table}_m_idx")
        cql.execute(f'CREATE INDEX ON {table}(entries(m))')
        cql.execute(f"DROP INDEX {table}_m_idx")
        # We can create multiple types of the above indexes at the same
        # time (see also test_index_map_multiple() above), so they will
        # get different default names using the standard default index
        # name mechanism (adding _1, etc.)
        cql.execute(f'CREATE INDEX ON {table}(m)')
        cql.execute(f'CREATE INDEX ON {table}(keys(m))')
        cql.execute(f'CREATE INDEX ON {table}(entries(m))')
        cql.execute(f"DROP INDEX {table}_m_idx")
        cql.execute(f"DROP INDEX {table}_m_idx_1")
        cql.execute(f"DROP INDEX {table}_m_idx_2")

# Reproducer for issue #10707 - indexing a column whose name is a quoted
# string should work fine. Even if the quoted string happens to look like
# an instruction to index a collection, e.g., "keys(m)".
def test_index_quoted_names(cql, test_keyspace):
    quoted_names = ['"hEllo"', '"x y"', '"hi""hello""yo"', '"""hi"""', '"keys(m)"', '"values(m)"', '"entries(m)"']
    schema = 'pk int, ck int, m int, ' + ','.join([name + " int" for name in quoted_names]) + ', PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        for name in quoted_names:
            cql.execute(f'CREATE INDEX ON {table}({name})')
        names = ','.join(quoted_names)
        values = ','.join(['3' for name in quoted_names])
        cql.execute(f'INSERT INTO {table} (pk, ck, {names}) VALUES (1, 2, {values})')
        for name in quoted_names:
            assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE {name} = 3'))

    # Moreover, we can have a collection with a quoted name, and can then
    # ask to index something strange-looking like keys("keys(m)").
    schema = 'pk int, ck int, m int, ' + ','.join([name + " map<int,int>" for name in quoted_names]) + ', PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        for name in quoted_names:
            cql.execute(f'CREATE INDEX ON {table}(keys({name}))')
        names = ','.join(quoted_names)
        values = ','.join(['{3:4}' for name in quoted_names])
        cql.execute(f'INSERT INTO {table} (pk, ck, {names}) VALUES (1, 2, {values})')
        for name in quoted_names:
            assert [(1,2)] == list(cql.execute(f'SELECT pk,ck FROM {table} WHERE {name} CONTAINS KEY 3'))

@pytest.mark.xfail(reason="#10713 - local collection indexing is not implemented yet")
def test_local_secondary_index_on_collection(cql, test_keyspace):
    schema = 'pk int, ck int, l list<int>, PRIMARY KEY (pk, ck)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f'CREATE INDEX ON {table}((pk), l)')

# Test that queries with the index over collection provide the same answer as it
# would be without index, but with ALLOW FILTERING.
# The operations on collections here are picked up randomly.
def test_secondary_collection_index(cql, test_keyspace):

    seed = int(time.time()*1e8)
    print(f"Seed for collection index test: {seed}")
    rand = random.Random(seed)

    schema = f'id int, m map<int, text>, primary key (id)'

    possible_ids = [100, 101]
    possible_keys = [1, 2, 3]
    possible_values = ['abc', 'def', 'ghi']

    def insert(table, id, map, **kwargs):
        a = (f'insert into {table}(id, m) values (%s, %s)', (id, map))
        print(a)
        cql.execute(*a)
    def update_cell(table, id, key, value, **kwargs):
        a = (f'update {table} set m[%s] = %s where id = %s', (key, value, id))
        print(a)
        cql.execute(*a)
    def update_delete(table, id, keys, **kwargs):
        a = (f'update {table} set m = m - %s where id = %s', (keys, id))
        print(a)
        cql.execute(*a)
    def update_add(table, id, map, **kwargs):
        a = (f'update {table} set m = m + %s where id = %s', (map, id))
        print(a)
        cql.execute(*a)
    def delete(table, id, **kwargs):
        a = (f'delete m from {table} where id = %s', (id,))
        print(a)
        cql.execute(*a)
    def delete_cell(table, id, key, **kwargs):
        a = (f'delete m[%s] from {table} where id = %s', (key, id))
        print(a)
        cql.execute(*a)

    def random_map():
        size = rand.randrange(len(possible_keys))
        keys = rand.sample(possible_keys, k=size)
        values = rand.choices(possible_values, k=size)
        return dict(zip(keys, values))
    def random_keys():
        return set(random_map())
    def random_key():
        return random.choice(possible_keys)
    def random_value():
        return random.choice(possible_values)
    def random_id():
        return random.choice(possible_ids)

    def random_operation():
        return random.choice([insert, update_cell, update_delete, update_add, delete, delete_cell])
    def random_args():
        return {
            'map': random_map(),
            'key': random_key(),
            'value': random_value(),
            'keys': random_keys(),
            'id': random_id(),
        }

    with new_test_table(cql, test_keyspace, schema) as tab1, new_test_table(cql, test_keyspace, schema) as tab2:
        def select(cql, table, where, *args):
            query = f'select id from {table} where {where}'
            if table is tab2:
                query += ' allow filtering'
            try:
                return cql.execute(query, *args)
            except:
                print('args=', args, table, where)
                raise
        def test_all_possible_selects():
            # Choose something that is not possible.
            possible_ids_ = possible_ids + [10000]
            possible_keys_ = possible_keys + [10000]
            possible_values_ = possible_values + ['aaaaa']
            for k, v in itertools.product(possible_keys_, possible_values_):
                r1 = select(cql, tab1, 'm[%s] = %s', (k, v))
                r2 = select(cql, tab2, 'm[%s] = %s', (k, v))
                assert_rows_ignoring_order(r1, *list(r2))
            for k in possible_keys_:
                r1 = select(cql, tab1, 'm contains key %s', (k,))
                r2 = select(cql, tab2, 'm contains key %s', (k,))
                assert_rows_ignoring_order(r1, *list(r2))
            for v in possible_values_:
                r1 = select(cql, tab1, 'm contains %s', (v,))
                r2 = select(cql, tab2, 'm contains %s', (v,))
                assert_rows_ignoring_order(r1, *list(r2))


        cql.execute(f'create index on {tab1}(keys(m))')
        cql.execute(f'create index on {tab1}(values(m))')
        cql.execute(f'create index on {tab1}(entries(m))')

        for _ in range(50):
            op = random_operation()
            args = random_args()
            print(f"op={op}, args={args}")
            for tab in [tab1, tab2]:
                op(tab, **args)
            test_all_possible_selects()

# Test that paging through a select using a secondary index works as
# expected, returning pages of the requested size.
# We have several tests here, for different schemas, that exercises
# different code paths and may expose different bugs.

def test_index_paging_pk_ck(cql, test_keyspace):
    schema = 'p int, c int, x int, primary key (p,c)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(x)")
        insert = cql.prepare(f"INSERT INTO {table}(p,c,x) VALUES (?,?,?)")
        for i in range(10):
            cql.execute(insert, [i, i, 3])
        cql.execute(insert, [17, 17, 2])
        for page_size in [1, 2, 3, 100]:
            stmt = SimpleStatement(f"SELECT p FROM {table} WHERE x = 3", fetch_size=page_size)
            # Check that:
            # 1. Each page of results has the expected page_size, or less in
            #    the last page. Although partial pages are theoretically
            #    allowed (and happen in other tests), in this test we don't
            #    expect Scylla or Cassandra to generate them.
            # 2. Check that all the results read over all pages are the
            #    expected ones (0...9)
            all_rows = []
            results = cql.execute(stmt)
            while len(results.current_rows) == page_size:
                all_rows.extend(results.current_rows)
                results = cql.execute(stmt, paging_state=results.paging_state)
            # After pages of page_size, the last page should be partial
            assert len(results.current_rows) < page_size
            all_rows.extend(results.current_rows)
            # Finally check that altogether, we read the right rows.
            assert sorted(all_rows) == [(i,) for i in range(10)]

def test_index_paging_pk_only(cql, test_keyspace):
    schema = 'p int, x int, primary key (p)'
    with new_test_table(cql, test_keyspace, schema) as table:
        cql.execute(f"CREATE INDEX ON {table}(x)")
        insert = cql.prepare(f"INSERT INTO {table}(p,x) VALUES (?,?)")
        for i in range(10):
            cql.execute(insert, [i, 3])
        cql.execute(insert, [17, 2])
        for page_size in [1, 2, 3, 100]:
            stmt = SimpleStatement(f"SELECT p FROM {table} WHERE x = 3", fetch_size=page_size)
            all_rows = []
            results = cql.execute(stmt)
            while len(results.current_rows) == page_size:
                all_rows.extend(results.current_rows)
                results = cql.execute(stmt, paging_state=results.paging_state)
            assert len(results.current_rows) < page_size
            all_rows.extend(results.current_rows)
            assert sorted(all_rows) == [(i,) for i in range(10)]
