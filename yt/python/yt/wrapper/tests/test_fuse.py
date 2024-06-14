from .conftest import authors
from .helpers import TEST_DIR

try:
    import yt.wrapper.cypress_fuse as cypress_fuse
# Process OSError('Unable for find libfuse')
except (ImportError, OSError, EnvironmentError):
    cypress_fuse = None

try:
    from yt.packages.fuse import fuse_file_info, FuseOSError
# Process OSError('Unable for find libfuse')
except (ImportError, OSError, EnvironmentError):
    fuse_file_info = None
    FuseOSError = None

import yt.wrapper as yt

import pytest
import random
import json


# TODO(ignat): YT-14959
@pytest.mark.skipif('os.environ.get("YT_OUTPUT") is not None')
@pytest.mark.usefixtures("yt_env")
class TestCachedYtClient(object):
    @authors("acid")
    def test_list(self):
        client = cypress_fuse.CachedYtClient(config=yt.config.config)
        assert sorted(client.list("/")) == sorted(yt.list("/"))

    @authors("acid")
    def test_list_nonexistent(self):
        client = cypress_fuse.CachedYtClient(config=yt.config.config)
        with pytest.raises(yt.YtError):
            client.list("//nonexistent")

    @authors("acid")
    def test_get_attributes_empty(self):
        client = cypress_fuse.CachedYtClient(config=yt.config.config)
        assert client.get_attributes("//sys", []) == {}

    @authors("acid")
    def test_get_attributes_nonexistent(self):
        client = cypress_fuse.CachedYtClient(config=yt.config.config)
        assert client.get_attributes("//sys", ["nonexistent"]) == {}
        # Get cached attribute again.
        assert client.get_attributes("//sys", ["nonexistent"]) == {}

    @authors("acid")
    def test_get_attributes_list(self, yt_env):
        client = cypress_fuse.CachedYtClient(config=yt.config.config)

        real_attributes = dict(yt.get("//home/@"))
        for attribute in real_attributes:
            if isinstance(real_attributes[attribute], yt.yson.YsonEntity):
                real_attributes[attribute] = yt.get("//home/@" + attribute)

        cached_attributes = dict(client.get_attributes("//home", list(real_attributes)))
        ephemeral_attributes = ["access_time", "access_counter", "ephemeral_ref_counter", "weak_ref_counter", "estimated_creation_time"]

        for attribute in ephemeral_attributes:
            for attributes in [real_attributes, cached_attributes]:
                attributes.pop(attribute, None)
        assert real_attributes == cached_attributes

        sample_names = random.sample(list(real_attributes), len(real_attributes) // 2)
        cached_attributes = client.get_attributes("//home", sample_names)
        assert sorted(cached_attributes.keys()) == sorted(sample_names)

        for name in sample_names:
            assert real_attributes[name] == cached_attributes[name]


# TODO(ignat): YT-14959
@pytest.mark.skipif('os.environ.get("YT_OUTPUT") is not None')
@pytest.mark.usefixtures("yt_env")
class TestCypress(object):
    @authors("acid")
    def test_readdir(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=False)

        assert sorted(cypress.readdir("/", None)) == sorted(yt.list("/"))

    @authors("acid")
    def test_read_file(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=False)

        filepath = TEST_DIR + "/file"
        content = b"Hello, world!" * 100
        yt.write_file(filepath, content)

        fi = fuse_file_info()
        fuse_filepath = filepath[1:]
        cypress.open(fuse_filepath, fi)

        fuse_content = cypress.read(fuse_filepath, len(content), 0, fi)
        assert fuse_content == content

        offset = len(content) // 2
        fuse_content = cypress.read(fuse_filepath, len(content), offset, fi)
        assert fuse_content == content[offset:]

        length = len(content) // 2
        fuse_content = cypress.read(fuse_filepath, length, 0, fi)
        assert fuse_content == content[:length]

        cypress.release(fuse_filepath, fi)

    @authors("acid")
    def test_read_table(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=False)

        filepath = TEST_DIR + "/file"
        content = ""
        for i in range(100):
            data = {"a": i, "b": 2 * i, "c": 3 * i}
            content += json.dumps(data, separators=(",", ":"), sort_keys=True)
            content += "\n"
        content = content.encode("utf-8")
        yt.write_table(filepath, content, format=yt.JsonFormat(), raw=True)

        fi = fuse_file_info()
        fuse_filepath = filepath[1:]
        cypress.open(fuse_filepath, fi)

        fuse_content = cypress.read(fuse_filepath, len(content), 0, fi)
        assert fuse_content == content

        offset = len(content) // 2
        fuse_content = cypress.read(fuse_filepath, len(content), offset, fi)
        assert fuse_content == content[offset:]

        length = len(content) // 2
        fuse_content = cypress.read(fuse_filepath, length, 0, fi)
        assert fuse_content == content[:length]

        cypress.release(fuse_filepath, fi)

    @authors("acid")
    def test_create_file(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=True)

        filepath = TEST_DIR + "/file"

        fi = fuse_file_info()
        fuse_filepath = filepath[1:]
        cypress.create(fuse_filepath, 0o755, fi)
        cypress.release(fuse_filepath, fi)

        assert yt.read_file(filepath).read() == b""

    @authors("acid")
    def test_unlink_file(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=True)

        filepath = TEST_DIR + "/file"

        yt.create("file", filepath)
        fuse_filepath = filepath[1:]
        fuse_test_dir = TEST_DIR[1:]
        cypress.unlink(fuse_filepath)

        assert "file" not in yt.list(TEST_DIR)
        assert "file" not in cypress.readdir(fuse_test_dir, None)

    @authors("acid")
    def test_truncate_file(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=True)

        filepath = TEST_DIR + "/file"
        content = b"Hello, world!" * 100
        yt.write_file(filepath, content)

        fi = fuse_file_info()
        fuse_filepath = filepath[1:]

        for truncated_length in [len(content), len(content) // 2, 0]:
            cypress.open(fuse_filepath, fi)
            cypress.truncate(fuse_filepath, truncated_length)
            cypress.flush(fuse_filepath, fi)
            cypress.release(fuse_filepath, fi)

            assert yt.read_file(filepath).read() == content[:truncated_length]

    @authors("acid")
    def test_write_file(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=True)

        filepath = TEST_DIR + "/file"
        content = b"Hello, world!" * 100

        fi = fuse_file_info()
        fuse_filepath = filepath[1:]

        cypress.create(fuse_filepath, 0o755, fi)
        cypress.write(fuse_filepath, content, 0, fi)
        cypress.flush(fuse_filepath, fi)
        cypress.release(fuse_filepath, fi)

        assert yt.read_file(filepath).read() == content

    @authors("acid")
    def test_write_multipart_file(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=True)

        filepath = TEST_DIR + "/file"
        content = b"Hello, world!" * 100

        parts = []
        part_length = 17
        offset = 0
        while offset < len(content):
            length = min(part_length, len(content) - offset)
            parts.append((offset, length))
            offset += length
        random.shuffle(parts)

        fi = fuse_file_info()
        fuse_filepath = filepath[1:]

        cypress.create(fuse_filepath, 0o755, fi)

        for offset, length in parts:
            cypress.write(fuse_filepath, content[offset:offset + length], offset, fi)

        cypress.flush(fuse_filepath, fi)
        cypress.release(fuse_filepath, fi)

        assert yt.read_file(filepath).read() == content

    @authors("acid")
    def test_create_directory(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=True)

        dirpath = TEST_DIR + "/dir"
        fuse_dirpath = dirpath[1:]

        cypress.mkdir(fuse_dirpath, 0o755)
        assert "dir" in yt.list(TEST_DIR)

        cypress.rmdir(fuse_dirpath)
        assert "dir" not in yt.list(TEST_DIR)

        cypress.mkdir(fuse_dirpath, 0o755)
        assert "dir" in yt.list(TEST_DIR)

    @authors("acid")
    def test_remove_directory(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=True)

        dirpath = TEST_DIR + "/dir"
        filepath = dirpath + "/file"
        yt.create("map_node", dirpath)
        yt.create("file", filepath)

        # Try to remove non-empty directory.
        fuse_dirpath = dirpath[1:]
        fuse_test_dir = TEST_DIR[1:]
        with pytest.raises(FuseOSError):
            cypress.rmdir(fuse_dirpath)
        assert "dir" in yt.list(TEST_DIR)
        assert "dir" in cypress.readdir(fuse_test_dir, None)

        # Remove empty directory.
        yt.remove(filepath)
        cypress.rmdir(fuse_dirpath)
        assert "dir" not in yt.list(TEST_DIR)
        assert "dir" not in cypress.readdir(fuse_test_dir, None)

    @authors("acid")
    def test_write_access_guards(self):
        cypress = cypress_fuse.Cypress(
            cypress_fuse.CachedYtClient(config=yt.config.config),
            enable_write_access=False)

        filepath = TEST_DIR + "/file"
        dirpath = TEST_DIR + "/dir"
        fuse_filepath = filepath[1:]
        fuse_dirpath = dirpath[1:]

        fi = fuse_file_info()
        with pytest.raises(FuseOSError):
            cypress.create(fuse_filepath, 0o755, fi)
        assert "file" not in yt.list(TEST_DIR)

        with pytest.raises(FuseOSError):
            cypress.mkdir(fuse_dirpath, 0o755)
        assert "dir" not in yt.list(TEST_DIR)

        yt.create("file", filepath)
        with pytest.raises(FuseOSError):
            cypress.unlink(fuse_filepath)
        assert "file" in yt.list(TEST_DIR)

        yt.create("map_node", dirpath)
        with pytest.raises(FuseOSError):
            cypress.rmdir(fuse_dirpath)
        assert "dir" in yt.list(TEST_DIR)
