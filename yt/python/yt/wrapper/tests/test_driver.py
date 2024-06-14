from .conftest import authors
from .helpers import TEST_DIR, get_test_file_path, get_python

from yt.wrapper.common import update_inplace

import yt.yson as yson


import yt.wrapper as yt
import yt.subprocess_wrapper as subprocess

import importlib
import os
import time
import pytest
import signal
from copy import deepcopy


@authors("ignat")
def test_heavy_proxies():
    from yt.wrapper.http_driver import HeavyProxyProvider
    from socket import error as SocketError

    config = deepcopy(yt.config.config)
    try:
        yt.config["proxy"]["number_of_top_proxies_for_random_choice"] = 1

        provider = HeavyProxyProvider(None)
        provider._get_light_proxy = lambda: "light_proxy"
        provider._discover_heavy_proxies = lambda: ["host1", "host2"]
        assert provider() == "host1"

        provider.on_error_occurred(SocketError())
        assert provider() == "host2"

        provider.on_error_occurred(SocketError())
        assert provider() == "host1"

        provider._discover_heavy_proxies = lambda: ["host2", "host3"]
        yt.config["proxy"]["proxy_ban_timeout"] = 10
        time.sleep(0.01)

        assert provider() == "host2"

        provider._discover_heavy_proxies = lambda: []
        assert provider() == "light_proxy"
    finally:
        importlib.reload(yt.http_driver)
        importlib.reload(yt.config)
        update_inplace(yt.config.config, config)


@authors("ignat")
@pytest.mark.usefixtures("yt_env")
def test_sanitize_structure():
    schema = yson.YsonList([{"name": "k", "type": "int64", "sort_order": "ascending"}])
    schema.attributes["unique_keys"] = True

    table = TEST_DIR + "/dynamic_table"
    yt.create("table", table, attributes={"schema": schema})
    assert yt.get(table + "/@schema/@unique_keys")


@authors("ignat")
def test_catching_sigint():
    binary = get_test_file_path("driver_read_request_catch_sigint.py")
    process = subprocess.Popen([get_python(), binary])

    time.sleep(2)
    assert process.poll() is None

    os.kill(process.pid, signal.SIGINT)
    try:
        process.wait(5)
    except Exception:
        os.kill(process.pid, signal.SIGKILL)
        assert False, "Process hanged up for more than 5 seconds on SIGINT"
