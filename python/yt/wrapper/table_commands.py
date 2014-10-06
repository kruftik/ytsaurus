"""
Commands for table working and Map-Reduce operations.
.. seealso:: `operations on wiki <https://wiki.yandex-team.ru/yt/userdoc/operations>`_

Python wrapper has some improvements over bare YT operations:

* upload files automatically

* create or erase output table

* delete files after

**Heavy commands** have deprecated parameter `response_type`:  one of \
"iter_lines" (default), "iter_content", "raw", "string". It specifies response type.

.. _operation_parameters:

Common operations parameters
-----------------------


* **spec** : (dict) universal method to set operation parameters

* **strategy** : (`yt.wrapper.operation_commands.WaitStrategy` or \
`yt.wrapper.operation_commands.AsyncStrategy`)\
strategy of waiting result, `yt.wrapper.config.DEFAULT_STRATEGY` by default

* **replication_factor** : (integer) number of output data replicas

* **compression_codec** : (one of "none" (default for files), "lz4" (default for tables), "snappy",\
 "gzip_best_compression", "gzip_normal", "lz4_high_compresion", "quick_lz") compression \
algorithm for output data

* **job_count** : (integer) recommendation how many jobs should run

* **table_writer** : (dict) spec of `"write" operation \
<https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#write>`_.

* **table_reader** : (dict) spec of `"read" operation \
<https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#read>`_.

* **format** : (string or descendant of `yt.wrapper.format.Format`) format of input and output \
data of operation

* **memory_limit** : (integer) memory limit in Mb in *scheduler* for every *job* (512Mb by default)


Operation run under self-pinged transaction, if `yt.wrapper.config.DETACHED` is `False`.
"""

import config
import py_wrapper
from common import flatten, require, unlist, update, EMPTY_GENERATOR, parse_bool, \
                   is_prefix, get_value, compose, bool_to_string, chunk_iter_lines, get_version
from errors import YtError, YtNetworkError
from driver import read_content, get_host_for_heavy_operation, make_request, ResponseStream
from keyboard_interrupts_catcher import KeyboardInterruptsCatcher
from table import TablePath, to_table, to_name, prepare_path
from tree_commands import exists, remove, remove_with_empty_dirs, get_attribute, copy, \
                          move, mkdir, find_free_subpath, create, get, get_type, set_attribute, \
                          _make_formatted_transactional_request, has_attribute
from file_commands import smart_upload_file
from transaction_commands import _make_transactional_request, abort_transaction
from transaction import PingableTransaction, Transaction
from format import create_format, YsonFormat
from lock import lock
from heavy_commands import make_heavy_request
from http import NETWORK_ERRORS
import yt.logger as logger

from yt.yson import yson_to_json

import os
import sys
import types
import exceptions
import tempfile
import simplejson as json
from cStringIO import StringIO

# Auxiliary methods

DEFAULT_EMPTY_TABLE = TablePath("//sys/empty_yamr_table", simplify=False)

def _prepare_source_tables(tables, replace_unexisting_by_empty=True, client=None):
    result = [to_table(table, client=client) for table in flatten(tables)]
    if config.TREAT_UNEXISTING_AS_EMPTY:
        if not replace_unexisting_by_empty:
            return [table for table in result if exists(table.name, client=client)]
        def get_empty_table(table):
            logger.warning("Warning: input table '%s' does not exist", table.name)
            return DEFAULT_EMPTY_TABLE
        return [table if exists(table.name, client=client) else get_empty_table(table)
                  for table in result]

    return result

def _are_default_empty_table(tables):
    return all(table == DEFAULT_EMPTY_TABLE for table in tables)

def _check_columns(columns, type):
    if len(columns) == 1 and "," in columns:
        logger.info('Comma found in column name "%s". '
                    'Did you mean to %s by a composite key?',
                    columns[0], type)

def _prepare_reduce_by(reduce_by):
    if reduce_by is None:
        if config.USE_YAMR_SORT_REDUCE_COLUMNS:
            reduce_by = ["key"]
        else:
            raise YtError("reduce_by option is required")
    reduce_by = flatten(reduce_by)
    _check_columns(reduce_by, "reduce")
    return reduce_by

def _prepare_sort_by(sort_by):
    if sort_by is None:
        if config.USE_YAMR_SORT_REDUCE_COLUMNS:
            sort_by = ["key", "subkey"]
        else:
            raise YtError("sort_by option is required")
    sort_by = flatten(sort_by)
    _check_columns(sort_by, "sort")
    return sort_by

def _reliably_upload_files(files, client=None):
    if files is None:
        return []

    file_paths = []
    with Transaction(null=True, client=client):
        for file in flatten(files):
            file_paths.append(smart_upload_file(file, client=client))
    return file_paths

def _prepare_formats(format, input_format, output_format):
    if format is None:
        format = config.format.TABULAR_DATA_FORMAT
    if isinstance(format, str):
        format = create_format(format)
    if isinstance(input_format, str):
        input_format = create_format(input_format)
    if isinstance(output_format, str):
        output_format = create_format(output_format)

    if input_format is None:
        input_format = format
    require(input_format is not None,
            YtError("You should specify input format"))

    if output_format is None:
        output_format = format
    require(output_format is not None,
            YtError("You should specify output format"))

    return input_format, output_format

def _prepare_format(format):
    if format is None:
        format = config.format.TABULAR_DATA_FORMAT
    if isinstance(format, str):
        format = create_format(format)

    require(format is not None,
            YtError("You should specify format"))
    return format

class TempfilesManager(object):
    def __init__(self):
        self._tempfiles_pool = []

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        if config.REMOVE_TEMP_FILES:
            for file in self._tempfiles_pool:
                os.remove(file)

    def create_tempfile(self, *args, **kwargs):
        """Use syntax tempfile.mkstemp"""
        _, filepath = tempfile.mkstemp(*args, **kwargs)
        self._tempfiles_pool.append(filepath)
        return filepath

def _prepare_binary(binary, operation_type, input_format=None, output_format=None,
                    reduce_by=None, client=None):
    if isinstance(binary, types.FunctionType) or hasattr(binary, "__call__"):
        with TempfilesManager() as tempfiles_manager:
            binary, binary_file, files = py_wrapper.wrap(binary, operation_type, tempfiles_manager,
                                                         input_format, output_format, reduce_by)
            uploaded_files = _reliably_upload_files([binary_file] + files, client=client)
            return binary, uploaded_files
    else:
        return binary, []

def _prepare_destination_tables(tables, replication_factor, compression_codec, client=None):
    if tables is None:
        if config.THROW_ON_EMPTY_DST_LIST:
            raise YtError("Destination tables are absent")
        return []
    tables = map(lambda name: to_table(name, client=client), flatten(tables))
    for table in tables:
        if exists(table.name, client=client):
            compression_codec_ok = (compression_codec is None) or \
                                   (compression_codec == get_attribute(table.name,
                                                                       "compression_codec"))
            replication_factor_ok = (replication_factor is None) or \
                                    (replication_factor == get_attribute(table.name,
                                                                         "replication_factor"))
            require(compression_codec_ok and replication_factor_ok,
                    YtError("Cannot append to table %s and set replication factor "
                            "or compression codec" % table))
        else:
            create_table(table.name, ignore_existing=True, replication_factor=replication_factor,
                         compression_codec=compression_codec, client=client)
    return tables

def _remove_locks(table, client=None):
    for lock_obj in get_attribute(table, "locks", [], client=client):
        if lock_obj["mode"] != "snapshot":
            if exists("//sys/transactions/" + lock_obj["transaction_id"], client=client):
                abort_transaction(lock_obj["transaction_id"], client=client)

def _remove_tables(tables, client=None):
    for table in tables:
        if exists(table) and get_type(table) == "table" and not table.append and table != DEFAULT_EMPTY_TABLE:
            if config.FORCE_DROP_DST:
                _remove_locks(table, client=client)
            remove(table, client=client)

def _add_user_command_spec(op_type, binary, format, input_format, output_format,
                           files, file_paths, local_files, yt_files,
                           memory_limit, reduce_by, spec, client=None):
    if binary is None:
        return spec, []

    if local_files is not None:
        require(files is None, "You cannot specify files and local_files simultaneously")
        files = local_files

    if yt_files is not None:
        require(file_paths is None, "You cannot specify yt_files and file_paths simultaneously")
        file_paths = yt_files

    files = _reliably_upload_files(files, client=client)
    input_format, output_format = _prepare_formats(format, input_format, output_format)
    binary, additional_files = _prepare_binary(binary, op_type, input_format, output_format,
                                               reduce_by, client=client)
    spec = update(
        {
            op_type: {
                "input_format": input_format.json(),
                "output_format": output_format.json(),
                "command": binary,
                "file_paths": map(
                    yson_to_json,
                    flatten(files + additional_files + map(lambda path: prepare_path(path, client=client), get_value(file_paths, [])))
                ),
                "use_yamr_descriptors": bool_to_string(config.USE_MAPREDUCE_STYLE_DESTINATION_FDS),
                "check_input_fully_consumed": bool_to_string(config.CHECK_INPUT_FULLY_CONSUMED)
            }
        },
        spec)

    memory_limit = get_value(memory_limit, config.MEMORY_LIMIT)
    if memory_limit is not None:
        spec = update({op_type: {"memory_limit": int(memory_limit)}}, spec)
    return spec, files + additional_files

def _configure_spec(spec):
    spec = update({"wrapper_version": get_version()}, spec)
    if config.POOL is not None:
        spec = update({"pool": config.POOL}, spec)
    if config.INTERMEDIATE_DATA_ACCOUNT is not None:
        spec = update({"intermediate_data_account": config.INTERMEDIATE_DATA_ACCOUNT}, spec)
    return spec

def _add_input_output_spec(source_table, destination_table, spec):
    def get_input_name(table):
        return table.get_json()
    def get_output_name(table):
        return table.get_json()

    spec = update({"input_table_paths": map(get_input_name, source_table)}, spec)
    if isinstance(destination_table, TablePath):
        spec = update({"output_table_path": get_output_name(destination_table)}, spec)
    else:
        spec = update({"output_table_paths": map(get_output_name, destination_table)}, spec)
    return spec

def _add_table_writer_spec(job_types, table_writer, spec):
    if table_writer is not None:
        for job_type in flatten(job_types):
            spec = update({job_type: {"table_writer": table_writer}}, spec)
    return spec

def _make_operation_request(command_name, spec, strategy,
                            finalizer=None, verbose=False, client=None):
    def _manage_operation(finalizer):
        operation = _make_formatted_transactional_request(command_name, {"spec": spec}, format=None,
                                                          verbose=verbose, client=client)
        get_value(strategy, config.DEFAULT_STRATEGY).process_operation(command_name, operation,
                                                                       finalizer, client=client)

    if config.DETACHED:
        _manage_operation(finalizer)
    else:
        transaction = PingableTransaction(
            config.OPERATION_TRANSACTION_TIMEOUT,
            attributes={"title": "Python wrapper: envelope transaction of operation"},
            client=client)

        def finish_transaction():
            transaction.__exit__(*sys.exc_info())

        with KeyboardInterruptsCatcher(finish_transaction):
            with transaction:
                _manage_operation(finalizer)

def _get_format_from_tables(tables, ignore_unexisting_tables):
    """Try to get format from tables, raise YtError if tables have different _format attribute"""
    not_none_tables = filter(None, flatten(tables))

    if ignore_unexisting_tables:
        tables_to_extract = filter(lambda x: exists(to_name(x)), not_none_tables)
    else:
        tables_to_extract = not_none_tables

    if not tables_to_extract:
        return None

    def extract_format(table):
        table_name = to_table(table).name

        if not exists(table_name):
            return None

        if "channel" in table_name.attributes:
            return create_format("<line_prefix=tskv>dsv")
        if has_attribute(table_name, "_format"):
            format_name = get(table_name + "/@_format", format=YsonFormat())
            return create_format(format_name)
        return None
    formats = map(extract_format, tables_to_extract)

    require(len(set(repr(format) for format in formats)) == 1,
            YtError("Tables have different attribute _format: " + repr(formats)))

    return formats[0]

""" Common table methods """

def create_table(path, recursive=None, ignore_existing=False,
                 replication_factor=None, compression_codec=None, attributes=None, client=None):
    """Create empty table.

    Shortcut for `create("table", ...)`.
    :param path: (string or :py:class:`yt.wrapper.table.TablePath`) path to table
    :param recursive: (bool) create the path automatically, `config.CREATE_RECURSIVE` by default
    :param ignore_existing: (bool) if it sets to `False` and table exists, \
                            Python Wrapper raises `YtResponseError`.
    :param replication_factor: (int) number of data replicas
    :param attributes: (dict)
    """
    table = to_table(path, client=client)
    recursive = get_value(recursive, config.CREATE_RECURSIVE)
    attributes = get_value(attributes, {})
    if replication_factor is not None:
        attributes["replication_factor"] = replication_factor
    if compression_codec is not None:
        attributes["compression_codec"] = compression_codec
    create("table", table.name, recursive=recursive, ignore_existing=ignore_existing,
           attributes=attributes, client=client)

def create_temp_table(path=None, prefix=None, client=None):
    """Create temporary table by given path with given prefix and return name.

    :param path: (string or :py:class:`yt.wrapper.table.TablePath`) existing path, \
                 by default `config.TEMP_TABLES_STORAGE`
    :param prefix: (string) prefix of table name
    :return: (string) name of result table
    """
    if path is None:
        path = config.TEMP_TABLES_STORAGE
        mkdir(path, recursive=True, client=client)
    else:
        path = to_name(path, client=client)
    require(exists(path, client=client), YtError("You cannot create table in unexisting path"))
    if prefix is not None:
        path = os.path.join(path, prefix)
    else:
        if not path.endswith("/"):
            path = path + "/"
    name = find_free_subpath(path, client=client)
    create_table(name, client=client)
    return name

def write_table(table, input_stream, format=None, table_writer=None,
                replication_factor=None, compression_codec=None, client=None):
    """Write rows from input_stream to table.

    :param table: (string or :py:class:`yt.wrapper.table.TablePath`) output table. Specify \
                `TablePath` attributes for append mode or something like this. Table can not exist.
    :param input_stream: python file-like object, string, list of strings, `StringIterIO`.
    :param format: (string or subclass of `Format`) format of input data, \
                    `yt.wrapper.config.format.TABULAR_DATA_FORMAT` by default.
    :param table_writer: (dict) spec of "write" operation
    :param replication_factor: (integer) number of data replicas
    :param compression_codec: (string) standard operation parameter

    Python Wrapper try to split input stream to portions of fixed size and write its with retries.
    If splitting fails, stream is written as is through HTTP.
    Set `yt.wrapper.config.USE_RETRIES_DURING_WRITE` to ``False`` for writing \
    without splitting and retries.

    Writing is executed under self-pinged transaction.
    """
    table = to_table(table, client=client)
    format = _prepare_format(format)

    params = {}
    params["input_format"] = format.json()
    if table_writer is not None:
        params["table_writer"] = table_writer

    def split_rows(stream):
        if isinstance(input_stream, types.ListType):
            for row in input_stream:
                yield row

        if isinstance(stream, str):
            stream = StringIO(input_stream)

        while True:
            row = format.load_row(stream, unparsed=True)
            if row:
                yield row
            else:
                return

    def chunked_iter(stream):
        while True:
            data = stream.read(1024 * 1024)
            if not data:
                break
            yield data

    def prepare_table(path):
        if exists(path, client=client):
            require(replication_factor is None and compression_codec is None,
                    YtError("Cannot write to existing path %s "
                            "with set replication factor or compression codec" % path))
        else:
            create_table(path, ignore_existing=True, replication_factor=replication_factor,
                         compression_codec=compression_codec, client=client)

    can_split_input = isinstance(input_stream, types.ListType) or format.is_read_row_supported()
    if config.USE_RETRIES_DURING_WRITE and can_split_input:
        input_stream = chunk_iter_lines(split_rows(input_stream), config.CHUNK_SIZE)
    elif isinstance(input_stream, file) or hasattr(input_stream, "read"):
        input_stream = chunked_iter(input_stream)

    if config.USE_RETRIES_DURING_WRITE and not can_split_input:
        logger.warning("Cannot split input into rows. Write is processing by one request.")

    make_heavy_request(
        "write",
        input_stream,
        table,
        params,
        prepare_table,
        config.USE_RETRIES_DURING_WRITE and can_split_input,
        client=client)

    if config.TREAT_UNEXISTING_AS_EMPTY and is_empty(table):
        _remove_tables([table], client=client)

def read_table(table, format=None, table_reader=None, response_type=None, raw=True, client=None):
    """Read rows from table and parse (optionally).

    :param table: string or :py:class:`yt.wrapper.table.TablePath`
    :param table_reader: (dict) spec of "read" operation
    :param response_type: output type, line generator by default. ["iter_lines", "iter_content", \
                          "raw", "string"]
    :param raw: (bool) don't parse response to rows
    :return: if `raw` is specified -- string or :class:`yt.wrapper.driver.ResponseStream`,\
             else -- rows generator (python dict or :class:`yt.wrapper.yamr_record.Record`)

    If :py:data:`yt.wrapper.config.RETRY_READ` is specified,
    command is executed under self-pinged transaction with retries and snapshot lock on the table.
    """
    table = to_table(table, client=client)
    format = _prepare_format(format)
    if config.TREAT_UNEXISTING_AS_EMPTY and not exists(table.name, client=client):
        return EMPTY_GENERATOR

    params = {
        "path": table.get_json(),
        "output_format": format.json()
    }
    if table_reader is not None:
        params["table_reader"] = table_reader

    if not config.RETRY_READ:
        response = _make_transactional_request(
            "read",
            params,
            return_content=False,
            proxy=get_host_for_heavy_operation(client=client),
            client=client)

        return read_content(response, raw, format, get_value(response_type, "iter_lines"))
    else:
        if response_type is not None:
            logger.info("read_table with retries ignore response_type option")

        title = "Python wrapper: read {0}".format(to_name(table, client=client))
        tx = PingableTransaction(timeout=config.http.REQUEST_TIMEOUT,
                                 attributes={"title": title},
                                 client=client)
        tx.__enter__()

        class Index(object):
            def __init__(self, index):
                self.index = index

            def get(self):
                return self.index

            def increment(self):
                self.index += 1

        def run_with_retries(func):
            for attempt in xrange(config.http.REQUEST_RETRY_COUNT):
                try:
                    return func()
                except YtNetworkError as err:
                    if attempt + 1 == config.http.REQUEST_RETRY_COUNT:
                        raise
                    logger.warning(str(err))
                    logger.warning("New retry (%d) ...", attempt + 2)

        def iter_with_retries(iter):
            try:
                for attempt in xrange(config.http.REQUEST_RETRY_COUNT):
                    try:
                        for elem in iter():
                            yield elem
                    except tuple(list(NETWORK_ERRORS) + [YtNetworkError]) as err:
                        if attempt + 1 == config.http.REQUEST_RETRY_COUNT:
                            raise
                        logger.warning(str(err))
                        logger.warning("New retry (%d) ...", attempt + 2)
            except exceptions.GeneratorExit:
                tx.__exit__(None, None, None)
            except:
                tx.__exit__(*sys.exc_info())
                raise
            else:
                tx.__exit__(None, None, None)

        def get_start_row_index():
            response = _make_transactional_request(
                "read",
                params,
                return_content=False,
                proxy=get_host_for_heavy_operation(),
                client=client)
            rsp_params = json.loads(response.headers()["X-YT-Response-Parameters"])
            return rsp_params.get("start_row_index", None)


        def read_iter(index):
            table.name.attributes["lower_limit"] = {"row_index": index.get()}
            params["path"] = table.get_json()
            response = _make_transactional_request(
                "read",
                params,
                return_content=False,
                proxy=get_host_for_heavy_operation(client=client))
            response_stream = ResponseStream(response, None)
            while True:
                row = format.load_row(response_stream, unparsed=raw)
                if row is None or row == "":
                    break
                yield row
                index.increment()

        try:
            lock(table, mode="snapshot", client=client)
            index = Index(run_with_retries(get_start_row_index))
            if index.get() is None:
                tx.__exit__(None, None, None)
                return (_ for _ in [])
            return iter_with_retries(lambda: read_iter(index))
        except:
            tx.__exit__(*sys.exc_info())
            raise

def _are_nodes(source_tables, destination_table):
    return len(source_tables) == 1 and \
           not source_tables[0].has_delimiters() and \
           not destination_table.append

def copy_table(source_table, destination_table, replace=True, client=None):
    """Copy table(s).

    :param source_table: string, `TablePath` or list of them
    :param destination_table: string or `TablePath`
    :param replace: (bool) override `destination_table`

    .. note:: param `replace` is overridden by setted \
              `yt.wrapper.config.REPLACE_TABLES_WHILE_COPY_OR_MOVE`
    If `source_table` is a list of tables, tables would be merged.
    """
    if config.REPLACE_TABLES_WHILE_COPY_OR_MOVE: replace = True
    source_tables = _prepare_source_tables(source_table, client=client)
    if config.TREAT_UNEXISTING_AS_EMPTY and _are_default_empty_table(source_tables):
        return
    destination_table = to_table(destination_table, client=client)
    if _are_nodes(source_tables, destination_table):
        if replace and exists(destination_table.name, client=client) and \
           to_name(source_tables[0], client=client) != to_name(destination_table, client=client):
            # in copy destination should be absent
            remove(destination_table.name, client=client)
        dirname = os.path.dirname(destination_table.name)
        if dirname != "//":
            mkdir(dirname, recursive=True, client=client)
        copy(source_tables[0].name, destination_table.name, client=client)
    else:
        source_names = [table.name for table in source_tables]
        mode = "sorted" if (all(map(is_sorted, source_names)) and not destination_table.append) \
               else "ordered"
        run_merge(source_tables, destination_table, mode, client=client)

def move_table(source_table, destination_table, replace=True, client=None):
    """Move table.

    :param source_table: string, `TablePath` or list of them
    :param destination_table: string or `TablePath`
    :param replace: (bool) override `destination_table`

    .. note:: param `replace` is overridden by `yt.wrapper.config.REPLACE_TABLES_WHILE_COPY_OR_MOVE`

    If `source_table` is a list of tables, tables would be merged.
    """
    if config.REPLACE_TABLES_WHILE_COPY_OR_MOVE: replace = True
    source_tables = _prepare_source_tables(source_table, client=client)
    if config.TREAT_UNEXISTING_AS_EMPTY and _are_default_empty_table(source_tables):
        return
    destination_table = to_table(destination_table, client=client)
    if _are_nodes(source_tables, destination_table):
        if source_tables[0] == destination_table:
            return
        if replace and exists(destination_table.name, client=client):
            remove(destination_table.name, client=client)
        move(source_tables[0].name, destination_table.name, client=client)
    else:
        copy_table(source_table, destination_table, client=client)
        for table in source_tables:
            if to_name(table, client=client) == to_name(destination_table, client=client):
                continue
            if table == DEFAULT_EMPTY_TABLE:
                continue
            remove(table.name, client=client)


def records_count(table, client=None):
    """Return number of records in the table.

    :param table: string or `TablePath`
    :return: integer
    """
    table = to_name(table, client=client)
    if config.TREAT_UNEXISTING_AS_EMPTY and not exists(table, client=client):
        return 0
    return get_attribute(table, "row_count", client=client)

def is_empty(table, client=None):
    """Is table empty?

    :param table: (string or `TablePath`)
    :return: (bool)
    """
    return records_count(to_name(table, client=client), client=client) == 0

def get_sorted_by(table, default=None, client=None):
    """Get 'sorted_by' table attribute or `default` if attribute doesn't exist.

    :param table: string or `TablePath`
    :param default: whatever
    :return: string or list of string
    """
    if default is None:
        default = [] if config.TREAT_UNEXISTING_AS_EMPTY else None
    return get_attribute(to_name(table, client=client), "sorted_by", default=default, client=client)

def is_sorted(table, client=None):
    """Is table sorted?

    :param table: string or `TablePath`
    :return: bool
    """
    if config.USE_YAMR_SORT_REDUCE_COLUMNS:
        return get_sorted_by(table, [], client=client) == ["key", "subkey"]
    else:
        return parse_bool(get_attribute(to_name(table, client=client),
                          "sorted", default="false", client=client))

def mount_table(path, first_tablet_index=None, last_tablet_index=None, cell_id=None, client=None):
    """Mount table (or a part of it).  NB! This command is not currently supported! The feature is coming with 0.17+ version!

    description is coming with tablets
    TODO
    """
    # TODO(ignat): Add path preparetion
    params = {"path": path}
    if first_tablet_index is not None:
        params["first_tablet_index"] = first_tablet_index
    if last_tablet_index is not None:
        params["last_tablet_index"] = last_tablet_index
    if cell_id is not None:
        params["cell_id"] = cell_id

    make_request("mount_table", params, client=client)

def unmount_table(path, first_tablet_index=None, last_tablet_index=None, force=None, client=None):
    """Unmount table (or a part of it).  NB! This command is not currently supported! The feature is coming with 0.17+ version!

    description is coming with tablets
    TODO
    """
    params = {"path": path}
    if first_tablet_index is not None:
        params["first_tablet_index"] = first_tablet_index
    if last_tablet_index is not None:
        params["last_tablet_index"] = last_tablet_index
    if force is not None:
        params["force"] = bool_to_string(force)

    make_request("unmount_table", params, client=client)

def remount_table(path, first_tablet_index=None, last_tablet_index=None, client=None):
    """Remount table (or a part of it).  NB! This command is not currently supported! The feature is coming with 0.17+ version!

    description is coming with tablets
    TODO
    """
    params = {"path": path}
    if first_tablet_index is not None:
        params["first_tablet_index"] = first_tablet_index
    if last_tablet_index is not None:
        params["last_tablet_index"] = last_tablet_index

    make_request("remount_table", params, client=client)

def reshard_table(path, pivot_keys, first_tablet_index=None, last_tablet_index=None, client=None):
    """Change pivot keys separating tablets of a given table.  NB! This command is not currently supported! The feature is coming with 0.17+ version!

    description is coming with tablets
    TODO
    """
    params = {"path": path,
              "pivot_keys": pivot_keys}
    if first_tablet_index is not None:
        params["first_tablet_index"] = first_tablet_index
    if last_tablet_index is not None:
        params["last_tablet_index"] = last_tablet_index

    make_request("reshard_table", params, client=client)

def select(query, timestamp=None, format=None, response_type=None, raw=True, client=None):
    """Execute a SQL-like query. NB! This command is not currently supported! The feature is coming with 0.17+ version!

    .. seealso:: `supported features <https://wiki.yandex-team.ru/yt/userdoc/queries>`_

    :param query: (string) for example \"<columns> [as <alias>], ... from \[<table>\] \
                  [where <predicate> [group by <columns> [as <alias>], ...]]\"
    :param timestamp: (string) TODO(veronikaiv): verify
    :param format: (string or descendant of `Format`) output format
    :param response_type: output type, line generator by default. ["iter_lines", "iter_content", \
                          "raw", "string"]
    :param raw: (bool) don't parse response to rows
    """
    format = _prepare_format(format)
    params = {
        "query": query,
        "output_format": format.json()}
    if timestamp is not None:
        params["timestamp"] = timestamp

    response = _make_transactional_request(
        "select",
        params,
        return_content=False,
        proxy=get_host_for_heavy_operation(),
        client=client)

    return read_content(response, raw, format, get_value(response_type, "iter_lines"))

# Operations.

def run_erase(table, spec=None, strategy=None, client=None):
    """Erase table or part of it.

    Erase differs from remove command.
    It only removes content of table (range of records or all table) and doesn't remove Cypress node.

    :param table: (string or `TablePath`)
    :param spec: (dict)
    :param strategy: standard operation parameter

    .. seealso::  :ref:`operation_parameters`.
    """
    table = to_table(table, client=client)
    if config.TREAT_UNEXISTING_AS_EMPTY and not exists(table.name, client=client):
        return
    spec = update({"table_path": table.get_json()}, get_value(spec, {}))
    spec = _configure_spec(spec)
    _make_operation_request("erase", spec, strategy, client=client)

def run_merge(source_table, destination_table, mode=None,
              strategy=None, table_writer=None,
              replication_factor=None, compression_codec=None,
              job_count=None, spec=None, client=None):
    """Merge source tables to destination table.

    :param source_table: list of string or `TablePath`, list tables names to merge
    :param destination_table: string or `TablePath`, path to result table
    :param mode: ['unordered' (default), 'ordered', or 'sorted']. Mode `sorted` keeps sortedness \
                 of output tables, mode `ordered` is about chunk magic, not for ordinary users.
    :param strategy: standard operation parameter
    :param table_writer: standard operation parameter
    :param replication_factor: (int) number of destination table replicas.
    :param compression_codec: (string) compression algorithm of destination_table.
    :param job_count: (integer) standard operation parameter.
    :param spec: (dict) standard operation parameter.


    .. seealso::  :ref:`operation_parameters`.
    """
    source_table = _prepare_source_tables(source_table, replace_unexisting_by_empty=False, client=client)
    destination_table = unlist(_prepare_destination_tables(destination_table, replication_factor,
                                                           compression_codec, client=client))

    if config.TREAT_UNEXISTING_AS_EMPTY and not source_table:
        _remove_tables([destination_table], client=client)
        return

    spec = compose(
        _configure_spec,
        lambda _: _add_table_writer_spec("job_io", table_writer, _),
        lambda _: _add_input_output_spec(source_table, destination_table, _),
        lambda _: update({"job_count": job_count}, _) if job_count is not None else _,
        lambda _: update({"mode": get_value(mode, "unordered")}, _),
        lambda _: get_value(_, {})
    )(spec)

    _make_operation_request("merge", spec, strategy, finalizer=None, client=None)

def run_sort(source_table, destination_table=None, sort_by=None,
             strategy=None, table_writer=None, replication_factor=None,
             compression_codec=None, spec=None, client=None):
    """Sort source tables to destination table.

    If destination table is not specified, than it equals to source table.

    .. seealso::  :ref:`operation_parameters`.
    """

    sort_by = _prepare_sort_by(sort_by)
    source_table = _prepare_source_tables(source_table, replace_unexisting_by_empty=False, client=client)
    for table in source_table:
        require(exists(table.name, client=client), YtError("Table %s should exist" % table))

    if destination_table is None:
        if config.TREAT_UNEXISTING_AS_EMPTY and not source_table:
            return
        require(len(source_table) == 1 and not source_table[0].has_delimiters(),
                YtError("You must specify destination sort table "
                        "in case of multiple source tables"))
        destination_table = source_table[0]
    destination_table = unlist(_prepare_destination_tables(destination_table, replication_factor,
                                                           compression_codec, client=client))

    if config.TREAT_UNEXISTING_AS_EMPTY and not source_table:
        _remove_tables([destination_table], client=client)
        return

    if all(is_prefix(sort_by, get_sorted_by(table.name, [], client=client)) for table in source_table):
        #(TODO) Hack detected: make something with it
        if len(source_table) > 0:
            run_merge(source_table, destination_table, "sorted",
                      strategy=strategy, table_writer=table_writer, spec=spec, client=client)
        return

    spec = compose(
        _configure_spec,
        lambda _: _add_table_writer_spec(["sort_job_io", "merge_job_io"], table_writer, _),
        lambda _: _add_input_output_spec(source_table, destination_table, _),
        lambda _: update({"sort_by": sort_by}, _),
        lambda _: get_value(_, {})
    )(spec)

    _make_operation_request("sort", spec, strategy, finalizer=None, client=client)

class Finalizer(object):
    """Entity for operation finalizing: checking size of result chunks, deleting of \
    empty output tables and uploaded files.
    """
    def __init__(self, files, output_tables, client=None):
        self.files = files if files is not None else []
        self.output_tables = output_tables
        self.client = client

    def __call__(self, state):
        if state == "completed":
            for table in map(lambda table: to_name(table, client=self.client), self.output_tables):
                self.check_for_merge(table)
        if config.DELETE_EMPTY_TABLES:
            for table in map(lambda table: to_name(table, client=self.client), self.output_tables):
                if is_empty(table, client=self.client):
                    remove_with_empty_dirs(table, client=self.client)
        if config.REMOVE_UPLOADED_FILES:
            for file in self.files:
                remove(file, force=True, client=self.client)

    def check_for_merge(self, table):
        chunk_count = int(get_attribute(table, "chunk_count", client=self.client))
        if  chunk_count < config.MIN_CHUNK_COUNT_FOR_MERGE_WARNING:
            return

        # We use uncompressed data size to simplify recommended command
        chunk_size = float(get_attribute(table, "compressed_data_size", client=self.client)) / chunk_count
        if chunk_size > config.MAX_CHUNK_SIZE_FOR_MERGE_WARNING:
            return

        compression_ratio = get_attribute(table, "compression_ratio", client=self.client)
        data_size_per_job = min(16 * 1024 * config.MB,
                                int(500 * config.MB / float(compression_ratio)))

        mode = "sorted" if is_sorted(table, client=self.client) else "unordered"

        if config.MERGE_INSTEAD_WARNING:
            run_merge(source_table=table, destination_table=table, mode=mode,
                      spec={"combine_chunks": "true", "data_size_per_job": data_size_per_job},
                      client=self.client)
        else:
            logger.info("Chunks of output table {0} are too small. "
                        "This may cause suboptimal system performance. "
                        "If this table is not temporary then consider running the following command:\n"
                        "yt merge --mode {1} --proxy {3} --src {0} --dst {0} "
                        "--spec '{{"
                           "combine_chunks=true;"
                           "data_size_per_job={2}"
                        "}}'".format(table, mode, data_size_per_job, config.http.PROXY))


def run_map_reduce(mapper, reducer, source_table, destination_table,
                   format=None,
                   map_input_format=None, map_output_format=None,
                   reduce_input_format=None, reduce_output_format=None,
                   strategy=None, table_writer=None, spec=None,
                   replication_factor=None, compression_codec=None,
                   map_files=None, map_file_paths=None,
                   map_local_files=None, map_yt_files=None,
                   reduce_files=None, reduce_file_paths=None,
                   reduce_local_files=None, reduce_yt_files=None,
                   mapper_memory_limit=None, reducer_memory_limit=None,
                   sort_by=None, reduce_by=None,
                   reduce_combiner=None,
                   reduce_combiner_input_format=None, reduce_combiner_output_format=None,
                   reduce_combiner_files=None, reduce_combiner_file_paths=None,
                   reduce_combiner_local_files=None, reduce_combiner_yt_files=None,
                   reduce_combiner_memory_limit=None,
                   client=None):
    """Run map (optionally), sort, reduce and reduce-combine (optionally) operations.

    :param mapper: (python generator, callable object-generator or string (with bash commands)).
    :param reducer: (python generator, callable object-generator or string (with bash commands)).
    :param source_table: (string, `TablePath` or list of them) input tables
    :param destination_table: (string, `TablePath` or list of them) output tables
    :param format: (string or descendant of `yt.wrapper.format.Format`) common format of input, \
                    intermediate and output data. More specific formats will override it.
    :param map_input_format: (string or descendant of `yt.wrapper.format.Format`)
    :param map_output_format: (string or descendant of `yt.wrapper.format.Format`)
    :param reduce_input_format: (string or descendant of `yt.wrapper.format.Format`)
    :param reduce_output_format: (string or descendant of `yt.wrapper.format.Format`)
    :param strategy:  standard operation parameter
    :param table_writer: (dict) standard operation parameter
    :param spec: (dict) standard operation parameter
    :param replication_factor: (int) standard operation parameter
    :param compression_codec: standard operation parameter
    :param map_files: Deprecated!
    :param map_file_paths: Deprecated!
    :param map_local_files: (string or list  of string) paths to map scripts on local machine.
    :param map_yt_files: (string or list  of string) paths to map scripts in Cypress.
    :param reduce_files: Deprecated!
    :param reduce_file_paths: Deprecated!
    :param reduce_local_files: (string or list  of string) paths to reduce scripts on local machine.
    :param reduce_yt_files: (string or list of string) paths to reduce scripts in Cypress.
    :param mapper_memory_limit: (integer) in Mb, map **job** memory limit.
    :param reducer_memory_limit: (integer) in Mb, reduce **job** memory limit.
    :param sort_by: (list of strings, string) list of columns for sorting by, \
                    equals to `reduce_by` by default
    :param reduce_by: (list of strings, string) list of columns for grouping by
    :param reduce_combiner: (python generator, callable object-generator or string \
                            (with bash commands)).
    :param reduce_combiner_input_format: (string or descendant of `yt.wrapper.format.Format`)
    :param reduce_combiner_output_format: (string or descendant of `yt.wrapper.format.Format`)
    :param reduce_combiner_files: Deprecated!
    :param reduce_combiner_file_paths: Deprecated!
    :param reduce_combiner_local_files: (string or list  of string) \
                                        paths to reduce combiner scripts on local machine.
    :param reduce_combiner_yt_files: (string or list  of string) \
                                     paths to reduce combiner scripts in Cypress.
    :param reduce_combiner_memory_limit: (integer) in Mb


    .. seealso::  :ref:`operation_parameters`.
    """

    run_map_reduce.files_to_remove = []
    def memorize_files(spec, files):
        run_map_reduce.files_to_remove += files
        return spec

    source_table = _prepare_source_tables(source_table, client=client)
    destination_table = _prepare_destination_tables(destination_table, replication_factor,
                                                    compression_codec, client=client)

    if config.TREAT_UNEXISTING_AS_EMPTY and _are_default_empty_table(source_table):
        _remove_tables(destination_table, client=client)
        return

    if sort_by is None:
        sort_by = reduce_by

    sort_by = _prepare_sort_by(sort_by)
    reduce_by = _prepare_reduce_by(reduce_by)

    spec = compose(
        _configure_spec,
        lambda _: _add_table_writer_spec(["map_job_io", "reduce_job_io", "sort_job_io"],
                                         table_writer, _),
        lambda _: _add_input_output_spec(source_table, destination_table, _),
        lambda _: update({"sort_by": sort_by, "reduce_by": reduce_by}, _),
        lambda _: memorize_files(*_add_user_command_spec("mapper", mapper,
            format, map_input_format, map_output_format,
            map_files, map_file_paths,
            map_local_files, map_yt_files,
            mapper_memory_limit, None, _, client=client)),
        lambda _: memorize_files(*_add_user_command_spec("reducer", reducer,
            format, reduce_input_format, reduce_output_format,
            reduce_files, reduce_file_paths,
            reduce_local_files, reduce_yt_files,
            reducer_memory_limit, reduce_by, _, client=client)),
        lambda _: memorize_files(*_add_user_command_spec("reduce_combiner", reduce_combiner,
            format, reduce_combiner_input_format, reduce_combiner_output_format,
            reduce_combiner_files, reduce_combiner_file_paths,
            reduce_combiner_local_files, reduce_combiner_yt_files,
            reduce_combiner_memory_limit, reduce_by, _, client=client)),
        lambda _: get_value(_, {})
    )(spec)

    _make_operation_request("map_reduce", spec, strategy,
                            Finalizer(run_map_reduce.files_to_remove,
                                      destination_table, client=client),
                            client=client)

def _run_operation(binary, source_table, destination_table,
                  files=None, file_paths=None,
                  local_files=None, yt_files=None,
                  format=None, input_format=None, output_format=None,
                  strategy=None,
                  table_writer=None,
                  replication_factor=None,
                  compression_codec=None,
                  job_count=None,
                  memory_limit=None,
                  spec=None,
                  op_name=None,
                  reduce_by=None,
                  client=None):
    """Run script operation.

    :param binary: (python generator, callable object-generator or string (with bash commands))
    :param files: Deprecated!
    :param file_paths: Deprecated!
    :param local_files: (string or list  of string) paths to scripts on local machine.
    :param yt_files: (string or list  of string) paths to scripts in Cypress.
    :param op_name: (one of "map" (default), "reduce", ...) TODO(veronikaiv): list it!

    .. seealso::  :ref:`operation_parameters` and :py:func:`yt.wrapper.table_commands.run_map_reduce`.
    """
    _run_operation.files = []
    def memorize_files(spec, files):
        _run_operation.files += files
        return spec
    op_name = get_value(op_name, "map")
    source_table = _prepare_source_tables(source_table, client=client)
    destination_table = _prepare_destination_tables(destination_table, replication_factor,
                                                    compression_codec, client=client)

    are_sorted_output = False
    for table in destination_table:
        if table.attributes.get("sorted_by") is not None:
            are_sorted_output = True

    finalize = None

    if op_name == "reduce":
        sort_by = _prepare_sort_by(reduce_by)
        reduce_by = _prepare_reduce_by(reduce_by)

        if config.RUN_MAP_REDUCE_IF_SOURCE_IS_NOT_SORTED:
            are_tables_not_sorted = False
            for table in source_table:
                sorted_by = get_sorted_by(table.name, [], client=client)
                if not sorted_by:
                    are_tables_not_sorted = True
                    continue
                if not is_prefix(reduce_by, sorted_by):
                    raise YtError("reduce_by parameter {0} conflicts with sorted_by attribute {1} of input table {2}".format(reduce_by, sorted_by, table.name))

            if are_tables_not_sorted and not are_sorted_output:
                if job_count is not None:
                    spec = update({"partition_count": job_count}, spec)
                run_map_reduce(
                    mapper=None,
                    reducer=binary,
                    reduce_files=files,
                    reduce_file_paths=file_paths,
                    source_table=source_table,
                    destination_table=destination_table,
                    format=format,
                    reduce_input_format=input_format,
                    reduce_output_format=output_format,
                    table_writer=table_writer,
                    reduce_by=reduce_by,
                    sort_by=sort_by,
                    replication_factor=replication_factor,
                    compression_codec=compression_codec,
                    reducer_memory_limit=memory_limit,
                    strategy=strategy,
                    spec=spec)
                return

            if are_sorted_output and are_tables_not_sorted:
                logger.info("Sorting %s", source_table)
                temp_table = create_temp_table(client=client)
                run_sort(source_table, temp_table, sort_by=reduce_by, client=client)
                finalize = lambda: remove(temp_table, client=client)
                source_table = [TablePath(temp_table)]


    if config.TREAT_UNEXISTING_AS_EMPTY and _are_default_empty_table(source_table):
        _remove_tables(destination_table, client=client)
        return

    op_type = None
    if op_name == "map": op_type = "mapper"
    if op_name == "reduce": op_type = "reducer"

    try:
        spec = compose(
            _configure_spec,
            lambda _: _add_table_writer_spec("job_io", table_writer, _),
            lambda _: _add_input_output_spec(source_table, destination_table, _),
            lambda _: update({"reduce_by": reduce_by}, _) if op_name == "reduce" else _,
            lambda _: update({"job_count": job_count}, _) if job_count is not None else _,
            lambda _: memorize_files(*_add_user_command_spec(op_type, binary,
                format, input_format, output_format,
                files, file_paths,
                local_files, yt_files,
                memory_limit, reduce_by, _, client=client)),
            lambda _: get_value(_, {})
        )(spec)

        _make_operation_request(op_name, spec, strategy,
                                Finalizer(_run_operation.files, destination_table, client=client),
                                client=client)
    finally:
        if finalize is not None:
            finalize()

def run_map(binary, source_table, destination_table, **kwargs):
    """Run map operation.
    .. seealso::  :ref:`operation_parameters` and :py:func:`yt.wrapper.table_commands.run_map_reduce`.
    """
    kwargs["op_name"] = "map"
    _run_operation(binary, source_table, destination_table, **kwargs)

def run_reduce(binary, source_table, destination_table, **kwargs):
    """Run reduce operation.
    .. seealso::  :ref:`operation_parameters` and :py:func:`yt.wrapper.table_commands.run_map_reduce`.
    """
    kwargs["op_name"] = "reduce"
    _run_operation(binary, source_table, destination_table, **kwargs)

def run_remote_copy(source_table, destination_table, cluster_name,
                    network_name=None, spec=None, copy_attributes=False, remote_cluster_token=None, strategy=None, client=None):
    """Copy source table from remote cluster to destination table on current cluster.

    :param source_table: (list of string or `TablePath`)
    :param destination_table: (string, `TablePath`)
    :param cluster_name: (string)
    :param network_name: (string)
    :param spec: (dict)
    :param copy_attributes: (bool) copy attributes source_table to destination_table
    :param strategy: standard operation parameter

    .. note:: For atomicity you should specify just one item in `source_table` \
    in case attributes copying.

    .. seealso::  :ref:`operation_parameters`.
    """
    def get_input_name(table):
        return to_table(table, client=client).get_json()

    # TODO(ignat): use base string in other places
    if isinstance(source_table, basestring):
        source_table = [source_table]

    destination_table = unlist(_prepare_destination_tables(destination_table, None, None,
                                                           client=client))

    # TODO(ignat): provide atomicity of attribute copying
    if copy_attributes:
        if len(source_table) != 1:
            raise YtError("Cannot copy attributes of multiple source tables")

        remote_proxy = get("//sys/clusters/{0}/proxy".format(cluster_name), client=client)
        current_proxy = config.http.PROXY
        current_token = config.http.TOKEN

        config.set_proxy(remote_proxy)
        config.http.TOKEN = get_value(remote_cluster_token, config.http.TOKEN)
        src_attributes = get(source_table[0] + "/@")

        config.set_proxy(current_proxy)
        config.http.TOKEN = current_token
        attributes = src_attributes.get("user_attribute_keys", []) + \
                     ["compression_codec", "erasure_codec", "replication_factor"]
        for attribute in attributes:
            set_attribute(destination_table, attribute, src_attributes[attribute], client=client)

    spec = compose(
        _configure_spec,
        lambda _: update({"network_name": network_name}, _) if network_name is not None else _,
        lambda _: update({"input_table_paths": map(get_input_name, source_table),
                          "output_table_path": destination_table.get_json(),
                          "cluster_name": cluster_name},
                          _),
        lambda _: get_value(spec, {})
    )(spec)

    _make_operation_request("remote_copy", spec, strategy, client=client)
