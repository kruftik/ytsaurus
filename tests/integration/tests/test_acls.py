import pytest

from yt_env_setup import YTEnvSetup, unix_only
from yt_commands import *

from yt.environment.helpers import assert_items_equal


##################################################################

class TestAcls(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    def test_empty_names_fail(self):
        with pytest.raises(YtError): create_user("")
        with pytest.raises(YtError): create_group("")

    def test_default_acl_sanity(self):
        create_user("u")
        with pytest.raises(YtError): set("/", {}, user="u")
        with pytest.raises(YtError): set("//sys", {}, user="u")
        with pytest.raises(YtError): set("//sys/a", "b", user="u")
        with pytest.raises(YtError): set("/a", "b", user="u")
        with pytest.raises(YtError): remove("//sys", user="u")
        with pytest.raises(YtError): remove("//sys/tmp", user="u")
        with pytest.raises(YtError): remove("//sys/home", user="u")
        with pytest.raises(YtError): set("//sys/home/a", "b", user="u")
        set("//tmp/a", "b", user="u")
        ls("//tmp", user="guest")
        with pytest.raises(YtError): set("//tmp/c", "d", user="guest")

    def _test_denying_acl(self, rw_path, rw_user, acl_path, acl_subject):
        set(rw_path, "b", user=rw_user)
        assert get(rw_path, user=rw_user) == "b"

        set(rw_path, "c", user=rw_user)
        assert get(rw_path, user=rw_user) == "c"

        set(acl_path + "/@acl/end", make_ace("deny", acl_subject, ["write", "remove"]))
        with pytest.raises(YtError): set(rw_path, "d", user=rw_user)
        assert get(rw_path, user=rw_user) == "c"

        remove(acl_path + "/@acl/-1")
        set(acl_path + "/@acl/end", make_ace("deny", acl_subject, ["read", "write", "remove"]))
        with pytest.raises(YtError): get(rw_path, user=rw_user)
        with pytest.raises(YtError): set(rw_path, "d", user=rw_user)

    def test_denying_acl1(self):
        create_user("u")
        self._test_denying_acl("//tmp/a", "u", "//tmp/a", "u")

    def test_denying_acl2(self):
        create_user("u")
        create_group("g")
        add_member("u", "g")
        self._test_denying_acl("//tmp/a", "u", "//tmp/a", "g")

    def test_denying_acl3(self):
        create_user("u")
        set("//tmp/p", {})
        self._test_denying_acl("//tmp/p/a", "u", "//tmp/p", "u")

    def _test_allowing_acl(self, rw_path, rw_user, acl_path, acl_subject):
        set(rw_path, "a")

        with pytest.raises(YtError): set(rw_path, "b", user=rw_user)

        set(acl_path + "/@acl/end", make_ace("allow", acl_subject, ["write", "remove"]))
        set(rw_path, "c", user=rw_user)

        remove(acl_path + "/@acl/-1")
        set(acl_path + "/@acl/end", make_ace("allow", acl_subject, ["read"]))
        with pytest.raises(YtError): set(rw_path, "d", user=rw_user)

    def test_allowing_acl1(self):
        self._test_allowing_acl("//tmp/a", "guest", "//tmp/a", "guest")

    def test_allowing_acl2(self):
        create_group("g")
        add_member("guest", "g")
        self._test_allowing_acl("//tmp/a", "guest", "//tmp/a", "g")

    def test_allowing_acl3(self):
        set("//tmp/p", {})
        self._test_allowing_acl("//tmp/p/a", "guest", "//tmp/p", "guest")

    def test_schema_acl1(self):
        create_user("u")
        create("table", "//tmp/t1", user="u")
        set("//sys/schemas/table/@acl/end", make_ace("deny", "u", "create"))
        with pytest.raises(YtError): create("table", "//tmp/t2", user="u")

    def test_schema_acl2(self):
        create_user("u")
        start_transaction(user="u")
        set("//sys/schemas/transaction/@acl/end", make_ace("deny", "u", "create"))
        with pytest.raises(YtError): start_transaction(user="u")

    def test_user_destruction(self):
        old_acl = get("//tmp/@acl")

        create_user("u")
        set("//tmp/@acl/end", make_ace("deny", "u", "write"))

        remove_user("u")
        assert get("//tmp/@acl") == old_acl

    def test_group_destruction(self):
        old_acl = get("//tmp/@acl")

        create_group("g")
        set("//tmp/@acl/end", make_ace("deny", "g", "write"))

        remove_group("g")
        assert get("//tmp/@acl") == old_acl

    def test_account_acl(self):
        create_account("a")
        create_user("u")

        with pytest.raises(YtError):
            create("table", "//tmp/t", user="u", attributes={"account": "a"})

        create("table", "//tmp/t", user="u")
        assert get("//tmp/t/@account") == "tmp"

        with pytest.raises(YtError):
            set("//tmp/t/@account", "a", user="u")

        set("//sys/accounts/a/@acl/end", make_ace("allow", "u", "use"))
        with pytest.raises(YtError):
            set("//tmp/t/@account", "a", user="u")

        set("//tmp/t/@acl/end", make_ace("allow", "u", "administer"))
        set("//tmp/t/@account", "a", user="u")
        assert get("//tmp/t/@account") == "a"

    def test_init_acl_in_create(self):
        create_user("u1")
        create_user("u2")
        create("table", "//tmp/t", attributes={
            "inherit_acl": False,
            "acl" : [make_ace("allow", "u1", "write")]})
        set("//tmp/t/@x", 1, user="u1")
        with pytest.raises(YtError): set("//tmp/t/@x", 1, user="u2")

    def test_init_acl_in_set(self):
        create_user("u1")
        create_user("u2")
        value = yson.YsonInt64(10)
        value.attributes["acl"] = [make_ace("allow", "u1", "write")]
        value.attributes["inherit_acl"] = False
        set("//tmp/t", value)
        set("//tmp/t/@x", 1, user="u1")
        with pytest.raises(YtError): set("//tmp/t/@x", 1, user="u2")

    def _prepare_scheduler_test(self):
        create_user("u")
        create_account("a")

        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"a" : "b"})

        create("table", "//tmp/t2")

        # just a sanity check
        map(in_="//tmp/t1", out="//tmp/t2", command="cat", user="u")

    @unix_only
    def test_scheduler_in_acl(self):
        self._prepare_scheduler_test()
        set("//tmp/t1/@acl/end", make_ace("deny", "u", "read"))
        with pytest.raises(YtError): map(in_="//tmp/t1", out="//tmp/t2", command="cat", user="u")

    @unix_only
    def test_scheduler_out_acl(self):
        self._prepare_scheduler_test()
        set("//tmp/t2/@acl/end", make_ace("deny", "u", "write"))
        with pytest.raises(YtError): map(in_="//tmp/t1", out="//tmp/t2", command="cat", user="u")

    @unix_only
    def test_scheduler_account_quota(self):
        self._prepare_scheduler_test()
        set("//tmp/t2/@account", "a")
        set("//sys/accounts/a/@acl/end", make_ace("allow", "u", "use"))
        set("//sys/accounts/a/@resource_limits/disk_space", 0)
        with pytest.raises(YtError): map(in_="//tmp/t1", out="//tmp/t2", command="cat", user="u")

    def test_scheduler_operation_abort_acl(self):
        self._prepare_scheduler_test()
        create_user("u1")
        op = map(
            dont_track=True,
            in_="//tmp/t1",
            out="//tmp/t2",
            command="cat; while true; do sleep 1; done",
            user="u")
        with pytest.raises(YtError): op.abort(user="u1")
        op.abort(user="u")

    def test_scheduler_operation_abort_by_owners(self):
        self._prepare_scheduler_test()
        create_user("u1")
        op = map(
            dont_track=True,
            in_="//tmp/t1",
            out="//tmp/t2",
            command="cat; while true; do sleep 1; done",
            user="u",
            spec={"owners": ["u1"]})
        op.abort(user="u1")

    def test_inherit1(self):
        set("//tmp/p", {})
        set("//tmp/p/@inherit_acl", False)

        create_user("u")
        with pytest.raises(YtError): set("//tmp/p/a", "b", user="u")
        with pytest.raises(YtError): ls("//tmp/p", user="u")

        set("//tmp/p/@acl/end", make_ace("allow", "u", ["read", "write"]))
        set("//tmp/p/a", "b", user="u")
        assert ls("//tmp/p", user="u") == ["a"]
        assert get("//tmp/p/a", user="u") == "b"

    def test_create_in_tx1(self):
        create_user("u")
        tx = start_transaction()
        create("table", "//tmp/a", tx=tx, user="u")
        assert read_table("//tmp/a", tx=tx, user="u") == []

    def test_create_in_tx2(self):
        create_user("u")
        tx = start_transaction()
        create("table", "//tmp/a/b/c", recursive=True, tx=tx, user="u")
        assert read_table("//tmp/a/b/c", tx=tx, user="u") == []

    @pytest.mark.xfail(run = False, reason = "In progress")
    def test_snapshot_remove(self):
        set("//tmp/a", {"b" : {"c" : "d"}})
        path = "#" + get("//tmp/a/b/c/@id")
        create_user("u")
        assert get(path, user="u") == "d"
        tx = start_transaction()
        lock(path, mode="snapshot", tx=tx)
        assert get(path, user="u", tx=tx) == "d"
        remove("//tmp/a")
        assert get(path, user="u", tx=tx) == "d"

    @pytest.mark.xfail(run = False, reason = "In progress")
    def test_snapshot_no_inherit(self):
        set("//tmp/a", "b")
        assert get("//tmp/a/@inherit_acl")
        tx = start_transaction()
        lock("//tmp/a", mode="snapshot", tx=tx)
        assert not get("//tmp/a/@inherit_acl", tx=tx)

    def test_administer_permission1(self):
        create_user("u")
        create("table", "//tmp/t")
        with pytest.raises(YtError): set("//tmp/t/@acl", [], user="u")

    def test_administer_permission2(self):
        create_user("u")
        create("table", "//tmp/t")
        set("//tmp/t/@acl/end", make_ace("allow", "u", "administer"))
        set("//tmp/t/@acl", [], user="u")

    def test_administer_permission3(self):
        create("table", "//tmp/t")
        create_user("u")

        acl = [make_ace("allow", "u", "administer"), make_ace("deny", "u", "write")]
        set("//tmp/t/@acl", acl)

        set("//tmp/t/@account", "tmp", user="u")
        set("//tmp/t/@inherit_acl", False, user="u")
        set("//tmp/t/@acl", acl, user="u")
        remove("//tmp/t/@acl/1", user="u")

    def test_administer_permission4(self):
        create("table", "//tmp/t")
        create_user("u")

        acl = [make_ace("deny", "u", "administer")]
        set("//tmp/t/@acl", acl)

        with pytest.raises(YtError):
            set("//tmp/t/@account", "tmp", user="u")

    def test_user_rename_success(self):
        create_user("u1")
        set("//sys/users/u1/@name", "u2")
        assert get("//sys/users/u2/@name") == "u2"

    def test_user_rename_fail(self):
        create_user("u1")
        create_user("u2")
        with pytest.raises(YtError):
            set("//sys/users/u1/@name", "u2")

    def test_deny_create(self):
        create_user("u")
        with pytest.raises(YtError):
            create("account_map", "//tmp/accounts", user="u")

    def test_deny_copy_src(self):
        create_user("u")
        with pytest.raises(YtError):
            copy("//sys", "//tmp/sys", user="u")

    def test_deny_copy_dst(self):
        create_user("u")
        create("table", "//tmp/t")
        with pytest.raises(YtError):
            copy("//tmp/t", "//sys/t", user="u", preserve_account=True)

    def test_document1(self):
        create_user("u")
        create("document", "//tmp/d")
        set("//tmp/d", {"foo":{}})
        set("//tmp/d/@inherit_acl", False)

        assert get("//tmp", user="u") == {"d": None}
        with pytest.raises(YtError): get("//tmp/d", user="u") == {"foo": {}}
        with pytest.raises(YtError): get("//tmp/d/@value", user="u")
        with pytest.raises(YtError): get("//tmp/d/foo", user="u")
        with pytest.raises(YtError): set("//tmp/d/foo", {}, user="u")
        with pytest.raises(YtError): set("//tmp/d/@value", {}, user="u")
        with pytest.raises(YtError): set("//tmp/d", {"foo":{}}, user="u")
        assert ls("//tmp", user="u") == ["d"]
        with pytest.raises(YtError): ls("//tmp/d", user="u")
        with pytest.raises(YtError): ls("//tmp/d/foo", user="u")
        assert exists("//tmp/d", user="u")
        with pytest.raises(YtError): exists("//tmp/d/@value", user="u")
        with pytest.raises(YtError): exists("//tmp/d/foo", user="u")
        with pytest.raises(YtError): remove("//tmp/d/foo", user="u")
        with pytest.raises(YtError): remove("//tmp/d", user="u")

    def test_document2(self):
        create_user("u")
        create("document", "//tmp/d")
        set("//tmp/d", {"foo":{}})
        set("//tmp/d/@inherit_acl", False)
        set("//tmp/d/@acl/end", make_ace("allow", "u", "read"))

        assert get("//tmp", user="u") == {"d": None}
        assert get("//tmp/d", user="u") == {"foo": {}}
        assert get("//tmp/d/@value", user="u") == {"foo": {}}
        assert get("//tmp/d/foo", user="u") == {}
        with pytest.raises(YtError): set("//tmp/d/foo", {}, user="u")
        with pytest.raises(YtError): set("//tmp/d/@value", {}, user="u")
        with pytest.raises(YtError): set("//tmp/d", {"foo":{}}, user="u")
        assert ls("//tmp", user="u") == ["d"]
        assert ls("//tmp/d", user="u") == ["foo"]
        assert ls("//tmp/d/foo", user="u") == []
        assert exists("//tmp/d", user="u")
        assert exists("//tmp/d/@value", user="u")
        assert exists("//tmp/d/foo", user="u")
        with pytest.raises(YtError): remove("//tmp/d/foo", user="u")
        with pytest.raises(YtError): remove("//tmp/d", user="u")

    def test_document3(self):
        create_user("u")
        create("document", "//tmp/d")
        set("//tmp/d", {"foo":{}})
        set("//tmp/d/@inherit_acl", False)
        set("//tmp/d/@acl/end", make_ace("allow", "u", ["read", "write", "remove"]))

        assert get("//tmp", user="u") == {"d": None}
        assert get("//tmp/d", user="u") == {"foo": {}}
        assert get("//tmp/d/@value", user="u") == {"foo": {}}
        assert get("//tmp/d/foo", user="u") == {}
        set("//tmp/d/foo", {}, user="u")
        set("//tmp/d/@value", {}, user="u")
        set("//tmp/d", {"foo":{}}, user="u")
        assert ls("//tmp", user="u") == ["d"]
        assert ls("//tmp/d", user="u") == ["foo"]
        assert ls("//tmp/d/foo", user="u") == []
        assert exists("//tmp/d", user="u")
        assert exists("//tmp/d/@value", user="u")
        assert exists("//tmp/d/foo", user="u")
        remove("//tmp/d/foo", user="u")
        remove("//tmp/d", user="u")

    def test_copy_account1(self):
        create_account("a")
        create_user("u")

        set("//tmp/x", {})
        set("//tmp/x/@account", "a")

        with pytest.raises(YtError):
            copy("//tmp/x", "//tmp/y", user="u", preserve_account=True)

    def test_copy_account2(self):
        create_account("a")
        create_user("u")
        set("//sys/accounts/a/@acl/end", make_ace("allow", "u", "use"))

        set("//tmp/x", {})
        set("//tmp/x/@account", "a")

        copy("//tmp/x", "//tmp/y", user="u", preserve_account=True)
        assert get("//tmp/y/@account") == "a"

    def test_copy_account3(self):
        create_account("a")
        create_user("u")

        set("//tmp/x", {"u": "v"})
        set("//tmp/x/u/@account", "a")

        with pytest.raises(YtError):
            copy("//tmp/x", "//tmp/y", user="u", preserve_account=True)

    def test_copy_non_writable_src(self):
        # YT-4175
        create_user("u")
        set("//tmp/s", {"x": {"a": 1}})
        set("//tmp/s/@acl/end", make_ace("allow", "u", ["read", "write", "remove"]))
        set("//tmp/s/x/@acl", [make_ace("deny", "u", ["write", "remove"])])
        copy("//tmp/s/x", "//tmp/s/y", user="u")
        assert get("//tmp/s/y", user="u") == get("//tmp/s/x", user="u")

    def test_copy_and_move_require_read_on_source(self):
        create_user("u")
        set("//tmp/s", {"x": {}})
        set("//tmp/s/@acl/end", make_ace("allow", "u", ["read", "write", "remove"]))
        set("//tmp/s/x/@acl", [make_ace("deny", "u", "read")])
        with pytest.raises(YtError): copy("//tmp/s/x", "//tmp/s/y", user="u")
        with pytest.raises(YtError): move("//tmp/s/x", "//tmp/s/y", user="u")

    def test_copy_and_move_require_write_on_target_parent(self):
        create_user("u")
        set("//tmp/s", {"x": {}})
        set("//tmp/s/@acl/end", make_ace("allow", "u", ["read", "remove"]))
        set("//tmp/s/@acl/end", make_ace("deny", "u", ["write"]))
        with pytest.raises(YtError): copy("//tmp/s/x", "//tmp/s/y", user="u")
        with pytest.raises(YtError): move("//tmp/s/x", "//tmp/s/y", user="u")

    def test_copy_and_move_requires_remove_on_target_if_exists(self):
        create_user("u")
        set("//tmp/s", {"x": {}, "y": {}})
        set("//tmp/s/@acl/end", make_ace("allow", "u", ["read", "write", "remove"]))
        set("//tmp/s/y/@acl", [make_ace("deny", "u", "remove")])
        with pytest.raises(YtError): copy("//tmp/s/x", "//tmp/s/y", force=True, user="u")
        with pytest.raises(YtError): move("//tmp/s/x", "//tmp/s/y", force=True, user="u")

    def test_move_requires_remove_on_self_and_write_on_self_parent(self):
        create_user("u")
        set("//tmp/s", {"x": {}})
        set("//tmp/s/@acl", [make_ace("allow", "u", ["read", "write", "remove"])])
        set("//tmp/s/x/@acl", [make_ace("deny", "u", "remove")])
        with pytest.raises(YtError): move("//tmp/s/x", "//tmp/s/y", user="u")
        set("//tmp/s/x/@acl", [])
        set("//tmp/s/@acl", [make_ace("allow", "u", ["read", "remove"]), make_ace("deny", "u", "write")])
        with pytest.raises(YtError): move("//tmp/s/x", "//tmp/s/y", user="u")
        set("//tmp/s/@acl", [make_ace("allow", "u", ["read", "write", "remove"])])
        move("//tmp/s/x", "//tmp/s/y", user="u")

    def test_superusers(self):
        create("table", "//sys/protected")
        create_user("u")
        with pytest.raises(YtError):
            remove("//sys/protected", user="u")
        add_member("u", "superusers")
        remove("//sys/protected", user="u")

    def test_remove_self_requires_permission(self):
        create_user("u")
        set("//tmp/x", {})
        set("//tmp/x/y", {})

        set("//tmp/x/@inherit_acl", False)
        with pytest.raises(YtError): remove("//tmp/x", user="u")
        with pytest.raises(YtError): remove("//tmp/x/y", user="u")

        set("//tmp/x/@acl", [make_ace("allow", "u", "write")])
        set("//tmp/x/y/@acl", [make_ace("deny", "u", "remove")])
        with pytest.raises(YtError): remove("//tmp/x", user="u")
        with pytest.raises(YtError): remove("//tmp/x/y", user="u")

        set("//tmp/x/y/@acl", [make_ace("allow", "u", "remove")])
        with pytest.raises(YtError): remove("//tmp/x", user="u")
        remove("//tmp/x/y", user="u")
        with pytest.raises(YtError): remove("//tmp/x", user="u")

        set("//tmp/x/@acl", [make_ace("allow", "u", "remove")])
        remove("//tmp/x", user="u")

    def test_remove_recursive_requires_permission(self):
        create_user("u")
        set("//tmp/x", {})
        set("//tmp/x/y", {})

        set("//tmp/x/@inherit_acl", False)
        with pytest.raises(YtError): remove("//tmp/x/*", user="u")
        set("//tmp/x/@acl", [make_ace("allow", "u", "write")])
        set("//tmp/x/y/@acl", [make_ace("deny", "u", "remove")])
        with pytest.raises(YtError): remove("//tmp/x/*", user="u")
        set("//tmp/x/y/@acl", [make_ace("allow", "u", "remove")])
        remove("//tmp/x/*", user="u")

    def test_set_self_requires_remove_permission(self):
        create_user("u")
        set("//tmp/x", {})
        set("//tmp/x/y", {})

        set("//tmp/x/@inherit_acl", False)
        with pytest.raises(YtError): set("//tmp/x", {}, user="u")
        with pytest.raises(YtError): set("//tmp/x/y", {}, user="u")

        set("//tmp/x/@acl", [make_ace("allow", "u", "write")])
        set("//tmp/x/y/@acl", [make_ace("deny", "u", "remove")])
        set("//tmp/x/y", {}, user="u")
        with pytest.raises(YtError): set("//tmp/x", {}, user="u")

        set("//tmp/x/@acl", [make_ace("allow", "u", "write")])
        set("//tmp/x/y/@acl", [make_ace("allow", "u", "remove")])
        set("//tmp/x/y", {}, user="u")
        set("//tmp/x", {}, user="u")

    def test_guest_can_remove_users_groups_accounts(self):
        create_user("u")
        create_group("g")
        create_account("a")

        with pytest.raises(YtError): remove("//sys/users/u", user="guest")
        with pytest.raises(YtError): remove("//sys/groups/g", user="guest")
        with pytest.raises(YtError): remove("//sys/accounts/a", user="guest")

        set("//sys/schemas/user/@acl/end", make_ace("allow", "guest", "remove"))
        set("//sys/schemas/group/@acl/end", make_ace("allow", "guest", "remove"))
        set("//sys/schemas/account/@acl/end", make_ace("allow", "guest", "remove"))

        remove("//sys/users/u", user="guest")
        remove("//sys/groups/g", user="guest")
        remove("//sys/accounts/a", user="guest")

        remove("//sys/schemas/user/@acl/-1")
        remove("//sys/schemas/group/@acl/-1")
        remove("//sys/schemas/account/@acl/-1")

    def test_set_acl_upon_construction(self):
        create_user("u")
        create("table", "//tmp/t", attributes={
            "acl": [make_ace("allow", "u", "write")],
            "inherit_acl": False})
        assert len(get("//tmp/t/@acl")) == 1

    
    def test_group_write_acl(self):
        create_user("u")
        create_group("g")
        with pytest.raises(YtError): add_member("u", "g", user="guest")
        set("//sys/groups/g/@acl/end", make_ace("allow", "guest", "write"))
        add_member("u", "g", user="guest")

    def test_user_remove_acl(self):
        create_user("u")
        with pytest.raises(YtError): remove("//sys/users/u", user="guest")
        set("//sys/users/u/@acl/end", make_ace("allow", "guest", "remove"))
        remove("//sys/users/u", user="guest")

    def test_group_remove_acl(self):
        create_group("g")
        with pytest.raises(YtError): remove("//sys/groups/g", user="guest")
        set("//sys/groups/g/@acl/end", make_ace("allow", "guest", "remove"))
        remove("//sys/groups/g", user="guest")


    def test_default_inheritance(self):
        create("map_node", "//tmp/m", attributes={"acl": [make_ace("allow", "guest", "remove")]})
        assert get("//tmp/m/@acl/0/inheritance_mode") == "object_and_descendants"

    def test_descendants_only_inheritance(self):
        create("map_node", "//tmp/m", attributes={"acl": [make_ace("allow", "guest", "remove", "descendants_only")]})
        create("map_node", "//tmp/m/s")
        create("map_node", "//tmp/m/s/r")
        assert check_permission("guest", "remove", "//tmp/m/s/r")["action"] == "allow"
        assert check_permission("guest", "remove", "//tmp/m/s")["action"] == "allow"
        assert check_permission("guest", "remove", "//tmp/m")["action"] == "deny"

    def test_object_only_inheritance(self):
        create("map_node", "//tmp/m", attributes={"acl": [make_ace("allow", "guest", "remove", "object_only")]})
        create("map_node", "//tmp/m/s")
        assert check_permission("guest", "remove", "//tmp/m/s")["action"] == "deny"
        assert check_permission("guest", "remove", "//tmp/m")["action"] == "allow"

    def test_immediate_descendants_only_inheritance(self):
        create("map_node", "//tmp/m", attributes={"acl": [make_ace("allow", "guest", "remove", "immediate_descendants_only")]})
        create("map_node", "//tmp/m/s")
        create("map_node", "//tmp/m/s/r")
        assert check_permission("guest", "remove", "//tmp/m/s/r")["action"] == "deny"
        assert check_permission("guest", "remove", "//tmp/m/s")["action"] == "allow"
        assert check_permission("guest", "remove", "//tmp/m")["action"] == "deny"

    def test_read_from_cache(self):
        create_user("u")
        set("//tmp/a", "b")
        set("//tmp/a/@acl/end", make_ace("deny", "u", "read"))
        with pytest.raises(YtError): get("//tmp/a", user="u")
        with pytest.raises(YtError): get("//tmp/a", user="u", read_from="cache")

##################################################################

class TestAclsMulticell(TestAcls):
    NUM_SECONDARY_MASTER_CELLS = 2
