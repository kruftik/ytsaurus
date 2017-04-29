from . import default_configs
from .helpers import canonize_uuid, WEB_INTERFACE_RESOURCES_PATH

from yt.wrapper.common import MB, GB
from yt.wrapper.mappings import VerifiedDict
from yt.common import YtError, update, get_value
from yt.yson import to_yson_type

from yt.packages.six import iteritems, add_metaclass
from yt.packages.six.moves import xrange

import random
import socket
import abc
import os
from copy import deepcopy

"""
TLDR: If you want to support new version of ytserver you should create your own ConfigsProvider
with overloaded necessary _build_*_configs methods and add it to dict VERSION_TO_CONFIGS_PROVIDER_CLASS
below. If you want to modify only local mode configs then modify_cluster_configuration function in
yt/local/cluster_configuration.py should be updated.

Configs for YT are generated the following way:
    1. For each version of ytserver ConfigsProvider_* is created
    2. ConfigsProvider takes provision (defines how many masters cells, nodes etc. to start and so on),
       directories where to store files for each service replica and ports generator in its build_configs()
       method and returns dict with cluster configuration.

       By default each config for each service replica is built from default config (see default_configs.py)
       by applying some patches and filling necessary (version specific) fields.

       Cluster configuration is formed by calling abstract _build_*_configs methods and then
       storing results in the dict with the following structure:
       {
           "master": {
               "primary_cell_tag": "123",
               "secondary_cell_tags": ["1", "2", "3"]  <-- only for ytserver 18.x (empty for older versions)
               "1": [
                   ...      <-- configs for master cell with tag "1"
               ],
               ...
           },
           "scheduler": [
                ...         <-- configs for each scheduler replica
           ],
           "node": [
                ...         <-- configs for each node replica
           ],
           "proxy": ...     <-- config for proxy
           "ui": ...        <-- config for ui
           "driver": {
                "1": ...    <-- connection config for cell with tag "1"
           }
       }

    3. Cluster configuration dict also can be modified before YT startup with modify_configs_func (see yt_env.py)
    3*. Local mode uses the same config providers but applies patches actual only for local mode by
        defining its own modify_configs_func.
"""

VERSION_TO_CONFIGS_PROVIDER_CLASS = {}  # this dict is filled at the end of file

def create_configs_provider(version):
    assert isinstance(version, tuple), "version must be a (MAJOR, MINOR) tuple"
    assert all(isinstance(component, int) for component in version), "version components must be integral"

    if version not in VERSION_TO_CONFIGS_PROVIDER_CLASS:
        raise YtError("Cannot create config provider for version {0}".format(version))

    return VERSION_TO_CONFIGS_PROVIDER_CLASS[version]()

_default_provision = {
    "master": {
        "primary_cell_tag": 0,
        "secondary_cell_count": 0,
        "cell_size": 1,
        "cell_nonvoting_master_count": 0
    },
    "scheduler": {
        "count": 1
    },
    "node": {
        "count": 1,
        "jobs_resource_limits": {
            "user_slots": 1,
            "cpu": 1,
            "memory": 4 * GB
        },
        "memory_limit_addition": None
    },
    "proxy": {
        "enable": False,
        "http_port": None
    },
    "enable_debug_logging": True,
    "fqdn": socket.getfqdn(),
    "enable_master_cache": False
}

def get_default_provision():
    return VerifiedDict(deepcopy(_default_provision))

@add_metaclass(abc.ABCMeta)
class ConfigsProvider(object):
    def build_configs(self, ports_generator, master_dirs, master_tmpfs_dirs=None, scheduler_dirs=None,
                      node_dirs=None, proxy_dir=None, logs_dir=None, provision=None):
        provision = get_value(provision, get_default_provision())

        # XXX(asaitgalin): All services depend on master so it is useful to make
        # connection configs with addresses and other useful info about all master cells.
        master_configs, connection_configs = self._build_master_configs(
            provision,
            master_dirs,
            master_tmpfs_dirs,
            ports_generator,
            logs_dir)

        scheduler_configs = self._build_scheduler_configs(provision, scheduler_dirs, deepcopy(connection_configs),
                                                          ports_generator, logs_dir)
        node_configs, node_addresses = \
            self._build_node_configs(provision, node_dirs, deepcopy(connection_configs), ports_generator, logs_dir)
        proxy_config = self._build_proxy_config(provision, proxy_dir, deepcopy(connection_configs), ports_generator,
                                                logs_dir, master_cache_nodes=node_addresses)
        driver_configs = self._build_driver_configs(provision, deepcopy(connection_configs),
                                                    master_cache_nodes=node_addresses)
        ui_config = self._build_ui_config(provision, deepcopy(connection_configs),
                                          "{0}:{1}".format(provision["fqdn"], proxy_config["port"]))

        cluster_configuration = {
            "master": master_configs,
            "driver": driver_configs,
            "scheduler": scheduler_configs,
            "node": node_configs,
            "proxy": proxy_config,
            "ui": ui_config
        }

        return cluster_configuration

    @abc.abstractmethod
    def _build_master_configs(self, provision, master_dirs, master_tmpfs_dirs, ports_generator, master_logs_dir):
        pass

    @abc.abstractmethod
    def _build_scheduler_configs(self, provision, scheduler_dirs, master_connection_configs,
                                 ports_generator, scheduler_logs_dir):
        pass

    @abc.abstractmethod
    def _build_node_configs(self, provision, node_dirs, master_connection_configs, ports_generator, node_logs_dir):
        pass

    @abc.abstractmethod
    def _build_proxy_config(self, provision, proxy_dir, master_connection_configs, ports_generator, proxy_logs_dir, master_cache_nodes):
        pass

    @abc.abstractmethod
    def _build_driver_configs(self, provision, master_connection_configs, master_cache_nodes):
        pass

    @abc.abstractmethod
    def _build_ui_config(self, provision, master_connection_configs, proxy_address):
        pass

def init_logging(node, path, name, enable_debug_logging):
    if not node:
        node = default_configs.get_logging_config(enable_debug_logging)

    def process(node, key, value):
        if isinstance(value, str):
            node[key] = value.format(path=path, name=name)
        else:
            node[key] = traverse(value)

    def traverse(node):
        if isinstance(node, dict):
            for key, value in iteritems(node):
                process(node, key, value)
        elif isinstance(node, list):
            for i, value in enumerate(node):
                process(node, i, value)
        return node

    return traverse(node)

DEFAULT_TRANSACTION_PING_PERIOD = 500

def set_at(config, path, value, merge=False):
    """Sets value in config by path creating intermediate dict nodes."""
    parts = path.split("/")
    for index, part in enumerate(parts):
        if index != len(parts) - 1:
            config = config.setdefault(part, {})
        else:
            if merge:
                config[part] = update(config.get(part, {}), value)
            else:
                config[part] = value

def get_at(config, path, default_value=None):
    for part in path.split("/"):
        if not isinstance(config, dict):
            raise ValueError("Path should not contain non-dict intermediate values")
        if part not in config:
            return default_value
        config = config[part]
    return config

def _set_bind_retry_options(config, key=None):
    if key is None:
        key = ""
    else:
        key = key + "/"
    set_at(config, "{0}bind_retry_count".format(key), 10)
    set_at(config, "{0}bind_retry_backoff".format(key), 3000)

def _generate_common_proxy_config(proxy_dir, proxy_port, enable_debug_logging, fqdn, ports_generator, proxy_logs_dir):
    proxy_config = default_configs.get_proxy_config()
    proxy_config["port"] = proxy_port if proxy_port else next(ports_generator)
    proxy_config["fqdn"] = "{0}:{1}".format(fqdn, proxy_config["port"])
    proxy_config["static"].append(["/ui", os.path.join(proxy_dir, "ui")])

    logging_config = get_at(proxy_config, "proxy/logging")
    set_at(proxy_config, "proxy/logging",
           init_logging(logging_config, proxy_logs_dir, "http-proxy", enable_debug_logging))
    set_at(proxy_config, "logging/filename", os.path.join(proxy_logs_dir, "http-application.log"))

    _set_bind_retry_options(proxy_config)

    return proxy_config

def _get_hydra_manager_config():
    return {
        "leader_lease_check_period": 100,
        "leader_lease_timeout": 20000,
        "disable_leader_lease_grace_delay": True,
        "response_keeper": {
            "enable_warmup": False,
            "expiration_time": 25000,
            "warmup_time": 30000,
        }
    }

def _get_retrying_channel_config():
    return {
        "retry_backoff_time": 10,
        "retry_attempts": 5,
        "soft_backoff_time": 10,
        "hard_backoff_time": 10
    }

def _get_rpc_config():
    return {
        "rpc_timeout": 5000
    }

def _get_node_resource_limits_config(provision):
    FOOTPRINT_MEMORY = 1 * GB
    CHUNK_META_CACHE_MEMORY = 1 * GB
    BLOB_SESSIONS_MEMORY = 2 * GB

    memory = 0
    memory += provision["node"]["jobs_resource_limits"]["memory"]
    if provision["node"]["memory_limit_addition"] is not None:
        memory += provision["node"]["memory_limit_addition"]

    memory += FOOTPRINT_MEMORY
    memory += CHUNK_META_CACHE_MEMORY
    memory += BLOB_SESSIONS_MEMORY

    return {"memory": memory}

class ConfigsProvider_18(ConfigsProvider):
    def _build_master_configs(self, provision, master_dirs, master_tmpfs_dirs, ports_generator, master_logs_dir):
        ports = []

        cell_tags = [str(provision["master"]["primary_cell_tag"] + index)
                     for index in xrange(provision["master"]["secondary_cell_count"] + 1)]
        random_part = random.randint(0, 2 ** 32 - 1)
        cell_ids = [canonize_uuid("%x-ffffffff-%x0259-ffffffff" % (random_part, int(tag)))
                    for tag in cell_tags]

        nonvoting_master_count = provision["master"]["cell_nonvoting_master_count"]

        connection_configs = {}
        for cell_index in xrange(provision["master"]["secondary_cell_count"] + 1):
            cell_ports = []
            cell_addresses = []

            for i in xrange(provision["master"]["cell_size"]):
                rpc_port, monitoring_port = next(ports_generator), next(ports_generator)
                address = to_yson_type("{0}:{1}".format(provision["fqdn"], rpc_port))
                if i >= provision["master"]["cell_size"] - nonvoting_master_count:
                    address.attributes["voting"] = False
                cell_addresses.append(address)
                cell_ports.append((rpc_port, monitoring_port))

            ports.append(cell_ports)

            connection_config = {
                "addresses": cell_addresses,
                "cell_id": cell_ids[cell_index]
            }
            connection_configs[cell_tags[cell_index]] = connection_config

        connection_configs["primary_cell_tag"] = cell_tags[0]
        connection_configs["secondary_cell_tags"] = cell_tags[1:]

        configs = {}
        for cell_index in xrange(provision["master"]["secondary_cell_count"] + 1):
            cell_configs = []

            for master_index in xrange(provision["master"]["cell_size"]):
                config = default_configs.get_master_config()

                set_at(config, "address_resolver/localhost_fqdn", provision["fqdn"])

                config["hydra_manager"] = _get_hydra_manager_config()

                set_at(config, "security_manager/user_statistics_gossip_period", 150)
                set_at(config, "security_manager/account_statistics_gossip_period", 150)
                set_at(config, "node_tracker/node_states_gossip_period", 150)

                config["rpc_port"], config["monitoring_port"] = ports[cell_index][master_index]

                config["primary_master"] = connection_configs[cell_tags[0]]
                config["secondary_masters"] = [connection_configs[tag]
                                               for tag in connection_configs["secondary_cell_tags"]]

                set_at(config, "timestamp_provider/addresses", connection_configs[cell_tags[0]]["addresses"])
                set_at(config, "snapshots/path",
                       os.path.join(master_dirs[cell_index][master_index], "snapshots"))

                if master_tmpfs_dirs is None:
                    set_at(config, "changelogs/path",
                           os.path.join(master_dirs[cell_index][master_index], "changelogs"))
                else:
                    set_at(config, "changelogs/path",
                           os.path.join(master_tmpfs_dirs[cell_index][master_index], "changelogs"))

                config["logging"] = init_logging(config.get("logging"), master_logs_dir,
                                                 "master-{0}-{1}".format(cell_index, master_index), provision["enable_debug_logging"])

                update(config, {
                    "tablet_manager": {
                        "cell_scan_period": 100
                    },
                    "multicell_manager": {
                        "cell_statistics_gossip_period": 80
                    },
                    "cell_directory_synchronizer": {
                        "sync_period": 500
                    }
                })

                _set_bind_retry_options(config, key="bus_server")

                cell_configs.append(config)

            configs[cell_tags[cell_index]] = cell_configs

        configs["primary_cell_tag"] = cell_tags[0]
        configs["secondary_cell_tags"] = cell_tags[1:]

        return configs, connection_configs

    def _build_cluster_connection_config(self, master_connection_configs, master_cache_nodes=None,
                                         config_template=None, enable_master_cache=False):
        primary_cell_tag = master_connection_configs["primary_cell_tag"]
        secondary_cell_tags = master_connection_configs["secondary_cell_tags"]

        cluster_connection = {
            "cell_directory": _get_retrying_channel_config(),
            "primary_master": master_connection_configs[primary_cell_tag],
            "transaction_manager": {
                "default_ping_period": DEFAULT_TRANSACTION_PING_PERIOD
            },
            "timestamp_provider": {
                "addresses": master_connection_configs[primary_cell_tag]["addresses"]
            }
        }

        update(cluster_connection["primary_master"], _get_retrying_channel_config())
        update(cluster_connection["primary_master"], _get_rpc_config())

        cluster_connection["secondary_masters"] = []
        for tag in secondary_cell_tags:
            config = master_connection_configs[tag]
            update(config, _get_retrying_channel_config())
            update(config, _get_rpc_config())
            cluster_connection["secondary_masters"].append(config)

        if config_template is not None:
            cluster_connection = update(config_template, cluster_connection)

        if enable_master_cache and master_cache_nodes:
            cluster_connection["master_cache"] = {
                "addresses": master_cache_nodes,
                "cell_id": master_connection_configs[primary_cell_tag]["cell_id"]}
        else:
            if "master_cache" in cluster_connection:
                del cluster_connection["master_cache"]

        return cluster_connection

    def _build_scheduler_configs(self, provision, scheduler_dirs, master_connection_configs,
                                 ports_generator, scheduler_logs_dir):
        configs = []

        for index in xrange(provision["scheduler"]["count"]):
            config = default_configs.get_scheduler_config()

            set_at(config, "address_resolver/localhost_fqdn", provision["fqdn"])
            config["cluster_connection"] = \
                self._build_cluster_connection_config(
                    master_connection_configs,
                    config_template=config["cluster_connection"])

            config["rpc_port"] = next(ports_generator)
            config["monitoring_port"] = next(ports_generator)
            set_at(config, "scheduler/snapshot_temp_path", os.path.join(scheduler_dirs[index], "snapshots"))
            set_at(config, "scheduler/orchid_cache_update_period", 0)

            config["logging"] = init_logging(config.get("logging"), scheduler_logs_dir,
                                             "scheduler-" + str(index), provision["enable_debug_logging"])
            _set_bind_retry_options(config, key="bus_server")

            # TODO(ignat): temporary solution to check that correctness of connected scheduler.
            # correct solution is to publish cell_id in some separate place in orchid.
            set_at(config, "scheduler/environment/primary_master_cell_id", config["cluster_connection"]["primary_master"]["cell_id"])

            configs.append(config)

        return configs

    def _build_proxy_config(self, provision, proxy_dir, master_connection_configs, ports_generator, proxy_logs_dir, master_cache_nodes):
        driver_config = default_configs.get_driver_config()
        update(driver_config, self._build_cluster_connection_config(
            master_connection_configs,
            master_cache_nodes=master_cache_nodes,
            enable_master_cache=provision["enable_master_cache"]))

        proxy_config = _generate_common_proxy_config(proxy_dir, provision["proxy"]["http_port"],
                                                     provision["enable_debug_logging"], provision["fqdn"],
                                                     ports_generator, proxy_logs_dir)
        proxy_config["proxy"]["driver"] = driver_config

        return proxy_config

    def _build_node_configs(self, provision, node_dirs, master_connection_configs, ports_generator, node_logs_dir):
        configs = []
        addresses = []

        for index in xrange(provision["node"]["count"]):
            config = default_configs.get_node_config(provision["enable_debug_logging"])

            set_at(config, "address_resolver/localhost_fqdn", provision["fqdn"])

            config["addresses"] = {
                "default": provision["fqdn"],
                "interconnect": provision["fqdn"]
            }

            config["rpc_port"] = next(ports_generator)
            config["monitoring_port"] = next(ports_generator)

            addresses.append("{0}:{1}".format(provision["fqdn"], config["rpc_port"]))

            config["cell_directory_synchronizer"] = {
                "sync_period": 500
            }

            config["cluster_connection"] = \
               self._build_cluster_connection_config(
                    master_connection_configs,
                    config_template=config["cluster_connection"])

            set_at(config, "data_node/read_thread_count", 2)
            set_at(config, "data_node/write_thread_count", 2)

            set_at(config, "data_node/multiplexed_changelog/path", os.path.join(node_dirs[index], "multiplexed"))

            set_at(config, "data_node/cache_locations", [])
            config["data_node"]["cache_locations"].append({
                "path": os.path.join(node_dirs[index], "chunk_cache"),
                "quota": 256 * MB,
            })

            set_at(config, "data_node/store_locations", [])
            config["data_node"]["store_locations"].append({
                "path": os.path.join(node_dirs[index], "chunk_store"),
                "low_watermark": 0,
                "high_watermark": 0
            })

            config["logging"] = init_logging(config.get("logging"), node_logs_dir, "node-{0}".format(index),
                                             provision["enable_debug_logging"])

            job_proxy_logging = get_at(config, "exec_agent/job_proxy_logging")
            log_name = "job_proxy-{0}".format(index)
            set_at(
                config,
                "exec_agent/job_proxy_logging",
                init_logging(job_proxy_logging, node_logs_dir, log_name, provision["enable_debug_logging"]))

            set_at(config, "tablet_node/hydra_manager", _get_hydra_manager_config(), merge=True)
            set_at(config, "tablet_node/hydra_manager/restart_backoff_time", 100)

            set_at(config, "exec_agent/job_controller/resource_limits",
                   deepcopy(provision["node"]["jobs_resource_limits"]), merge=True)
            set_at(config, "resource_limits", _get_node_resource_limits_config(provision), merge=True)

            _set_bind_retry_options(config, key="bus_server")

            configs.append(config)

        return configs, addresses

    def _build_driver_configs(self, provision, master_connection_configs, master_cache_nodes):
        secondary_cell_tags = master_connection_configs["secondary_cell_tags"]
        primary_cell_tag = master_connection_configs["primary_cell_tag"]

        configs = {}
        for cell_index in xrange(provision["master"]["secondary_cell_count"] + 1):
            config = default_configs.get_driver_config()
            if cell_index == 0:
                tag = primary_cell_tag
                update(config, self._build_cluster_connection_config(
                    master_connection_configs,
                    master_cache_nodes=master_cache_nodes,
                    enable_master_cache=provision["enable_master_cache"]))
            else:
                tag = secondary_cell_tags[cell_index - 1]
                cell_connection_config = {
                    "primary_master": master_connection_configs[secondary_cell_tags[cell_index - 1]],
                    "timestamp_provider": {
                        "addresses": master_connection_configs[primary_cell_tag]["addresses"]
                    },
                    "transaction_manager": {
                        "default_ping_period": DEFAULT_TRANSACTION_PING_PERIOD
                    }
                }
                update(cell_connection_config["primary_master"], _get_retrying_channel_config())
                update(cell_connection_config["primary_master"], _get_rpc_config())

                update(config, cell_connection_config)

            configs[tag] = config

        return configs

    def _build_ui_config(self, provision, master_connection_configs, proxy_address):
        address_blocks = []
        # Primary masters cell index is 0
        for cell_index in xrange(provision["master"]["secondary_cell_count"] + 1):
            if cell_index == 0:
                cell_tag = master_connection_configs["primary_cell_tag"]
                cell_addresses = master_connection_configs[cell_tag]["addresses"]
            else:
                cell_tag = master_connection_configs["secondary_cell_tags"][cell_index - 1]
                cell_addresses = master_connection_configs[cell_tag]["addresses"]
            block = "{{ addresses: [ '{0}' ], cellTag: {1} }}"\
                .format("', '".join(cell_addresses), int(cell_tag))
            address_blocks.append(block)
        masters = "primaryMaster: {0}".format(address_blocks[0])
        if provision["master"]["secondary_cell_count"]:
            masters += ", secondaryMasters: [ '{0}' ]".format("', '".join(address_blocks[1:]))

        template_conf_path = os.path.join(WEB_INTERFACE_RESOURCES_PATH, "configs/localmode.js.tmpl")
        if os.path.exists(template_conf_path):
            return open(template_conf_path).read()\
                .replace("%%proxy%%", "'{0}'".format(proxy_address))
        else:
            return default_configs.get_ui_config()\
                .replace("%%proxy_address%%", "'{0}'".format(proxy_address))\
                .replace("%%masters%%", masters)


class ConfigsProvider_18_3_18_4(ConfigsProvider_18):
    def _build_node_configs(self, provision, node_dirs, master_connection_configs, ports_generator, node_logs_dir):
        configs, addresses = super(ConfigsProvider_18_3_18_4, self)._build_node_configs(
                provision, node_dirs, master_connection_configs, ports_generator, node_logs_dir)

        current_user = 10000

        for i, config in enumerate(configs):
            set_at(config, "exec_agent/slot_manager/start_uid", current_user)
            set_at(config, "exec_agent/slot_manager/paths", [os.path.join(node_dirs[i], "slots")])

            set_at(config, "exec_agent/enable_cgroups", False)
            set_at(config, "exec_agent/environment_manager", {"environments": {"default": {"type": "unsafe"}}})

            current_user += provision["node"]["jobs_resource_limits"]["user_slots"] + 1

        return configs, addresses

class ConfigsProvider_18_3(ConfigsProvider_18_3_18_4):
    pass

class ConfigsProvider_18_4(ConfigsProvider_18_3_18_4):
    pass

class ConfigsProvider_18_5(ConfigsProvider_18):
    def _build_node_configs(self, provision, node_dirs, master_connection_configs, ports_generator, node_logs_dir):
        configs, addresses = super(ConfigsProvider_18_5, self)._build_node_configs(
                provision, node_dirs, master_connection_configs, ports_generator, node_logs_dir)

        current_user = 10000
        for i, config in enumerate(configs):
            # TODO(psushin): This is a very dirty hack to ensure that different parallel
            # test and yt_node instances do not share same uids range for user jobs.
            start_uid = current_user + config["rpc_port"]

            set_at(config, "exec_agent/slot_manager/job_environment", {
                "type": "simple",
                "start_uid" : start_uid,
            })
            set_at(config, "exec_agent/slot_manager/locations", [{"path" : os.path.join(node_dirs[i], "slots")}])

            set_at(config, "exec_agent/node_directory_prepare_backoff_time", 100)
            set_at(config, "node_directory_synchronizer/sync_period", 100)

            config["addresses"] = [
                ("interconnect", provision["fqdn"]),
                ("default", provision["fqdn"])
            ]

        return configs, addresses

    def _build_scheduler_configs(self, provision, scheduler_dirs, master_connection_configs,
                                 ports_generator, log_path):
        configs = super(ConfigsProvider_18_5, self)._build_scheduler_configs(
                provision, scheduler_dirs, master_connection_configs, ports_generator, log_path)

        for config in configs:
            # Since 18.5 scheduler Orchid consists of a static part (that is periodically cached)
            # and a dynamic part, so the name of the option was changed.
            del config["scheduler"]["orchid_cache_update_period"]
            set_at(config, "scheduler/static_orchid_cache_update_period", 0)

        return configs

class ConfigsProvider_19_0(ConfigsProvider_18_5):
    pass

class ConfigsProvider_19_1(ConfigsProvider_19_0):
    pass

class ConfigsProvider_19_2(ConfigsProvider_19_1):
    pass

VERSION_TO_CONFIGS_PROVIDER_CLASS = {
    (18, 3): ConfigsProvider_18_3,
    (18, 4): ConfigsProvider_18_4,
    (18, 5): ConfigsProvider_18_5,
    (19, 0): ConfigsProvider_19_0,
    (19, 1): ConfigsProvider_19_1,
    (19, 2): ConfigsProvider_19_2
}

