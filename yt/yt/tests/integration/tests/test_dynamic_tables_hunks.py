from test_sorted_dynamic_tables import TestSortedDynamicTablesBase

from yt.test_helpers import assert_items_equal, wait

from yt_commands import *  # noqa

import __builtin__

################################################################################


class TestSortedDynamicTablesHunks(TestSortedDynamicTablesBase):
    SCHEMA = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value", "type": "string"},
    ]

    DELTA_DYNAMIC_MASTER_CONFIG = {
        "tablet_manager": {
            "enable_hunks": True
        }
    }

    def _get_table_schema(self, max_inline_hunk_size):
        schema = self.SCHEMA
        schema[1]["max_inline_hunk_size"] = max_inline_hunk_size
        return schema

    def _create_table(self, max_inline_hunk_size=10):
        self._create_simple_table("//tmp/t",
                                  schema=self._get_table_schema(max_inline_hunk_size),
                                  enable_dynamic_store_read=False,
                                  hunk_chunk_reader={
                                      "max_hunk_count_per_read": 2,
                                      "max_total_hunk_length_per_read": 60,
                                  },
                                  min_hunk_compaction_total_hunk_length=1,
                                  max_hunk_compaction_garbage_ratio=0.5,
                                  enable_lsm_verbose_logging=True)

    def _get_store_chunk_ids(self, path):
        chunk_ids = get(path + "/@chunk_ids")
        return [chunk_id for chunk_id in chunk_ids if get("#{}/@chunk_type".format(chunk_id)) == "table"]

    def _get_hunk_chunk_ids(self, path):
        chunk_ids = get(path + "/@chunk_ids")
        return [chunk_id for chunk_id in chunk_ids if get("#{}/@chunk_type".format(chunk_id)) == "hunk"]

    @authors("babenko")
    def test_flush_inline(self):
        sync_create_cells(1)
        self._create_table()

        sync_mount_table("//tmp/t")
        keys = [{"key": i} for i in xrange(10)]
        rows = [{"key": i, "value": "value" + str(i)} for i in xrange(10)]
        insert_rows("//tmp/t", rows)
        assert_items_equal(select_rows("* from [//tmp/t]"), rows)
        assert_items_equal(lookup_rows("//tmp/t", keys), rows)
        assert_items_equal(select_rows("* from [//tmp/t] where value = \"{}\"".format(rows[0]["value"])), [rows[0]])
        sync_unmount_table("//tmp/t")

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 0

        assert get("#{}/@hunk_chunk_refs".format(store_chunk_ids[0])) == []

        sync_mount_table("//tmp/t")

        assert_items_equal(select_rows("* from [//tmp/t]"), rows)
        assert_items_equal(lookup_rows("//tmp/t", keys), rows)

    @authors("babenko")
    def test_flush_to_hunk_chunk(self):
        sync_create_cells(1)
        self._create_table()

        sync_mount_table("//tmp/t")
        keys = [{"key": i} for i in xrange(10)]
        rows = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in xrange(10)]
        insert_rows("//tmp/t", rows)
        assert_items_equal(select_rows("* from [//tmp/t]"), rows)
        assert_items_equal(lookup_rows("//tmp/t", keys), rows)
        sync_unmount_table("//tmp/t")

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 1
        hunk_chunk_id = hunk_chunk_ids[0]

        assert get("#{}/@hunk_chunk_refs".format(store_chunk_ids[0])) == [
            {"chunk_id": hunk_chunk_ids[0], "hunk_count": 10, "total_hunk_length": 260}
        ]
        assert get("#{}/@data_weight".format(hunk_chunk_id)) == 260
        assert get("#{}/@uncompressed_data_size".format(hunk_chunk_id)) == 260
        assert get("#{}/@compressed_data_size".format(hunk_chunk_id)) == 260

        sync_mount_table("//tmp/t")

        assert_items_equal(select_rows("* from [//tmp/t]"), rows)
        assert_items_equal(lookup_rows("//tmp/t", keys), rows)
        assert_items_equal(select_rows("* from [//tmp/t] where value = \"{}\"".format(rows[0]["value"])), [rows[0]])

    @authors("babenko")
    def test_compaction(self):
        sync_create_cells(1)
        self._create_table()

        sync_mount_table("//tmp/t")
        rows1 = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in xrange(10)]
        keys1 = [{"key": i} for i in xrange(10)]
        insert_rows("//tmp/t", rows1)
        assert_items_equal(select_rows("* from [//tmp/t]"), rows1)
        assert_items_equal(lookup_rows("//tmp/t", keys1), rows1)
        sync_unmount_table("//tmp/t")

        assert len(self._get_store_chunk_ids("//tmp/t")) == 1
        assert len(self._get_hunk_chunk_ids("//tmp/t")) == 1

        sync_mount_table("//tmp/t")
        rows2 = [{"key": i, "value": "value" + str(i) + "y" * 20} for i in xrange(10, 20)]
        keys2 = [{"key": i} for i in xrange(10, 20)]
        insert_rows("//tmp/t", rows2)
        select_rows("* from [//tmp/t]")
        assert_items_equal(lookup_rows("//tmp/t", keys1 + keys2), rows1 + rows2)

        sync_unmount_table("//tmp/t")
        wait(lambda: get("//tmp/t/@chunk_count") == 4)

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 2
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 2

        store_chunk_id1 = store_chunk_ids[0]
        store_chunk_id2 = store_chunk_ids[1]
        hunk_chunk_id1 = get("#{}/@hunk_chunk_refs/0/chunk_id".format(store_chunk_id1))
        hunk_chunk_id2 = get("#{}/@hunk_chunk_refs/0/chunk_id".format(store_chunk_id2))
        assert_items_equal(hunk_chunk_ids, [hunk_chunk_id1, hunk_chunk_id2])

        if get("#{}/@hunk_chunk_refs/0/total_hunk_length".format(store_chunk_id1)) > get("#{}/@hunk_chunk_refs/0/total_hunk_length".format(store_chunk_id2)):
            store_chunk_id1, store_chunk_id2 = store_chunk_id2, store_chunk_id1
            hunk_chunk_id1, hunk_chunk_id2 = hunk_chunk_id2, hunk_chunk_id1

        assert get("#{}/@hunk_chunk_refs".format(store_chunk_id1)) == [
            {"chunk_id": hunk_chunk_id1, "hunk_count": 10, "total_hunk_length": 260}
        ]
        assert get("#{}/@hunk_chunk_refs".format(store_chunk_id2)) == [
            {"chunk_id": hunk_chunk_id2, "hunk_count": 10, "total_hunk_length": 270}
        ]

        sync_mount_table("//tmp/t")

        assert_items_equal(select_rows("* from [//tmp/t]"), rows1 + rows2)
        assert_items_equal(lookup_rows("//tmp/t", keys1 + keys2), rows1 + rows2)

        set("//tmp/t/@forced_store_compaction_revision", 1)
        remount_table("//tmp/t")

        wait(lambda: get("//tmp/t/@chunk_count") == 3)

        compacted_store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(compacted_store_chunk_ids) == 1
        compacted_store_id = compacted_store_chunk_ids[0]

        assert_items_equal(get("#{}/@hunk_chunk_refs".format(compacted_store_id)), [
            {"chunk_id": hunk_chunk_id1, "hunk_count": 10, "total_hunk_length": 260},
            {"chunk_id": hunk_chunk_id2, "hunk_count": 10, "total_hunk_length": 270},
        ])

        sync_unmount_table("//tmp/t")
        sync_mount_table("//tmp/t")

        assert_items_equal(select_rows("* from [//tmp/t]"), rows1 + rows2)
        assert_items_equal(lookup_rows("//tmp/t", keys1 + keys2), rows1 + rows2)

    @authors("babenko")
    def test_hunk_sweep(self):
        sync_create_cells(1)
        self._create_table()

        sync_mount_table("//tmp/t")
        rows1 = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in xrange(10)]
        keys1 = [{"key": i} for i in xrange(10)]
        rows2 = [{"key": i, "value": "value" + str(i)} for i in xrange(10, 20)]
        keys2 = [{"key": i} for i in xrange(10, 20)]
        insert_rows("//tmp/t", rows1 + rows2)
        assert_items_equal(select_rows("* from [//tmp/t]"), rows1 + rows2)
        assert_items_equal(lookup_rows("//tmp/t", keys1 + keys2), rows1 + rows2)
        sync_unmount_table("//tmp/t")

        assert len(self._get_store_chunk_ids("//tmp/t")) == 1
        assert len(self._get_hunk_chunk_ids("//tmp/t")) == 1

        sync_mount_table("//tmp/t")
        delete_rows("//tmp/t", keys1)
        assert_items_equal(select_rows("* from [//tmp/t]"), rows2)
        assert_items_equal(lookup_rows("//tmp/t", keys1 + keys2), rows2)
        sync_unmount_table("//tmp/t")

        assert len(self._get_store_chunk_ids("//tmp/t")) == 2
        assert len(self._get_hunk_chunk_ids("//tmp/t")) == 1

        sync_mount_table("//tmp/t")
        assert_items_equal(select_rows("* from [//tmp/t]"), rows2)
        assert_items_equal(lookup_rows("//tmp/t", keys1 + keys2), rows2)

        set("//tmp/t/@min_data_ttl", 60000)
        set("//tmp/t/@forced_store_compaction_revision", 1)
        remount_table("//tmp/t")

        wait(lambda: get("//tmp/t/@chunk_count") == 2)
        assert len(self._get_store_chunk_ids("//tmp/t")) == 1
        assert len(self._get_hunk_chunk_ids("//tmp/t")) == 1

        set("//tmp/t/@min_data_ttl", 0)
        set("//tmp/t/@min_data_versions", 1)
        set("//tmp/t/@forced_store_compaction_revision", 1)
        remount_table("//tmp/t")

        wait(lambda: get("//tmp/t/@chunk_count") == 1)
        assert len(self._get_store_chunk_ids("//tmp/t")) == 1
        assert len(self._get_hunk_chunk_ids("//tmp/t")) == 0

    @authors("babenko")
    def test_reshard(self):
        sync_create_cells(1)
        self._create_table()

        sync_mount_table("//tmp/t")
        # This chunk will intersect both of new tablets and will produce chunk views.
        rows1 = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in [0, 10, 20, 30, 40, 50]]
        insert_rows("//tmp/t", rows1)
        sync_unmount_table("//tmp/t")

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        store_chunk_id1 = store_chunk_ids[0]

        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 1
        hunk_chunk_id1 = hunk_chunk_ids[0]

        sync_mount_table("//tmp/t")
        # This chunk will be fully contained in the first tablet.
        rows2 = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in [11, 12, 13]]
        insert_rows("//tmp/t", rows2)
        assert_items_equal(select_rows("* from [//tmp/t]"), rows1 + rows2)
        sync_unmount_table("//tmp/t")

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 2
        store_chunk_id2 = list(__builtin__.set(store_chunk_ids) - __builtin__.set([store_chunk_id1]))[0]

        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 2
        hunk_chunk_id2 = list(__builtin__.set(hunk_chunk_ids) - __builtin__.set([hunk_chunk_id1]))[0]

        gc_collect()
        assert get("#{}/@ref_counter".format(store_chunk_id1)) == 1
        assert get("#{}/@ref_counter".format(hunk_chunk_id1)) == 1
        assert get("#{}/@ref_counter".format(store_chunk_id2)) == 1
        assert get("#{}/@ref_counter".format(hunk_chunk_id2)) == 1

        reshard_table("//tmp/t", [[], [30]])

        gc_collect()
        assert get("#{}/@ref_counter".format(store_chunk_id1)) == 2
        assert get("#{}/@ref_counter".format(hunk_chunk_id1)) == 2
        assert get("#{}/@ref_counter".format(store_chunk_id2)) == 1
        assert get("#{}/@ref_counter".format(hunk_chunk_id2)) == 1

        sync_mount_table("//tmp/t")

        assert_items_equal(select_rows("* from [//tmp/t]"), rows1 + rows2)

    @authors("babenko")
    def test_compaction_writes_hunk_chunk(self):
        sync_create_cells(1)
        self._create_table(max_inline_hunk_size=1000)

        sync_mount_table("//tmp/t")
        rows1 = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in xrange(10)]
        rows2 = [{"key": i, "value": "value" + str(i)} for i in xrange(10, 20)]
        insert_rows("//tmp/t", rows1 + rows2)
        sync_unmount_table("//tmp/t")

        alter_table("//tmp/t", schema=self._get_table_schema(max_inline_hunk_size=10))

        chunk_ids_before_compaction = get("//tmp/t/@chunk_ids")
        assert len(chunk_ids_before_compaction) == 1
        chunk_id_before_compaction = chunk_ids_before_compaction[0]
        assert get("#{}/@hunk_chunk_refs".format(chunk_id_before_compaction)) == []

        sync_mount_table("//tmp/t")

        set("//tmp/t/@forced_store_compaction_revision", 1)
        remount_table("//tmp/t")

        wait(lambda: get("//tmp/t/@chunk_ids") != chunk_ids_before_compaction)
        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        compacted_store_id = store_chunk_ids[0]
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 1
        hunk_chunk_id = hunk_chunk_ids[0]
        assert_items_equal(get("#{}/@hunk_chunk_refs".format(compacted_store_id)), [
            {"chunk_id": hunk_chunk_id, "hunk_count": 10, "total_hunk_length": 260},
        ])

    @authors("babenko")
    def test_compaction_inlines_hunks(self):
        sync_create_cells(1)
        self._create_table(max_inline_hunk_size=10)

        sync_mount_table("//tmp/t")
        rows1 = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in xrange(10)]
        rows2 = [{"key": i, "value": "value" + str(i)} for i in xrange(10, 20)]
        insert_rows("//tmp/t", rows1 + rows2)
        sync_unmount_table("//tmp/t")

        alter_table("//tmp/t", schema=self._get_table_schema(max_inline_hunk_size=1000))

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        store_chunk_id = store_chunk_ids[0]
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 1
        hunk_chunk_id = hunk_chunk_ids[0]
        assert_items_equal(get("#{}/@hunk_chunk_refs".format(store_chunk_id)), [
            {"chunk_id": hunk_chunk_id, "hunk_count": 10, "total_hunk_length": 260},
        ])

        sync_mount_table("//tmp/t")

        set("//tmp/t/@forced_store_compaction_revision", 1)
        remount_table("//tmp/t")

        wait(lambda: len(get("//tmp/t/@chunk_ids")) == 1)
        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        store_chunk_id = store_chunk_ids[0]
        assert get("#{}/@hunk_chunk_refs".format(store_chunk_id)) == []

    @authors("babenko")
    def test_compaction_rewrites_hunk_chunk(self):
        sync_create_cells(1)
        self._create_table(max_inline_hunk_size=10)

        sync_mount_table("//tmp/t")
        rows = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in xrange(10)]
        keys = [{"key": i} for i in xrange(10)]
        insert_rows("//tmp/t", rows)
        sync_unmount_table("//tmp/t")

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        store_chunk_id = store_chunk_ids[0]
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 1
        hunk_chunk_id0 = hunk_chunk_ids[0]
        assert_items_equal(get("#{}/@hunk_chunk_refs".format(store_chunk_id)), [
            {"chunk_id": hunk_chunk_id0, "hunk_count": 10, "total_hunk_length": 260},
        ])

        sync_mount_table("//tmp/t")
        delete_rows("//tmp/t", keys[1:])
        sync_unmount_table("//tmp/t")

        sync_mount_table("//tmp/t")

        chunk_ids_before_compaction1 = get("//tmp/t/@chunk_ids")
        assert len(chunk_ids_before_compaction1) == 3

        set("//tmp/t/@min_data_ttl", 0)
        set("//tmp/t/@min_data_versions", 1)
        set("//tmp/t/@forced_store_compaction_revision", 1)
        remount_table("//tmp/t")

        def _check1():
            chunk_ids = get("//tmp/t/@chunk_ids")
            return chunk_ids != chunk_ids_before_compaction1 and len(chunk_ids) == 2
        wait(_check1)

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        store_chunk_id = store_chunk_ids[0]
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 1
        hunk_chunk_id1 = hunk_chunk_ids[0]
        assert hunk_chunk_id0 == hunk_chunk_id1
        assert_items_equal(get("#{}/@hunk_chunk_refs".format(store_chunk_id)), [
            {"chunk_id": hunk_chunk_id1, "hunk_count": 1, "total_hunk_length": 26},
        ])

        chunk_ids_before_compaction2 = get("//tmp/t/@chunk_ids")
        assert len(chunk_ids_before_compaction2) == 2

        set("//tmp/t/@forced_store_compaction_revision", 1)
        remount_table("//tmp/t")

        def _check2():
            chunk_ids = get("//tmp/t/@chunk_ids")
            return chunk_ids != chunk_ids_before_compaction2 and len(chunk_ids) == 2
        wait(_check2)

        store_chunk_ids = self._get_store_chunk_ids("//tmp/t")
        assert len(store_chunk_ids) == 1
        store_chunk_id = store_chunk_ids[0]
        hunk_chunk_ids = self._get_hunk_chunk_ids("//tmp/t")
        assert len(hunk_chunk_ids) == 1
        hunk_chunk_id2 = hunk_chunk_ids[0]
        assert hunk_chunk_id1 != hunk_chunk_id2
        assert_items_equal(get("#{}/@hunk_chunk_refs".format(store_chunk_id)), [
            {"chunk_id": hunk_chunk_id2, "hunk_count": 1, "total_hunk_length": 26},
        ])
        assert get("#{}/@hunk_count".format(hunk_chunk_id2)) == 1
        assert get("#{}/@total_hunk_length".format(hunk_chunk_id2)) == 26
