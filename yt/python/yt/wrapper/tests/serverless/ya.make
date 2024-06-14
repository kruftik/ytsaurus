PY3TEST()

INCLUDE(${ARCADIA_ROOT}/yt/opensource.inc)

SIZE(MEDIUM)

# Required for test_thread_pool.py
FORK_TEST_FILES()

REQUIREMENTS(
    cpu:4
)

PEERDIR(
    yt/python/yt/wrapper
    yt/python/yt/testlib
    yt/python/yt/yson

    contrib/python/flaky
)

IF (PYTHON2)
    PEERDIR(
        contrib/deprecated/python/ujson
    )
ELSE()
    PEERDIR(
        contrib/python/ujson
    )
ENDIF()

TEST_SRCS(
    test_common.py
    test_formats.py
    test_schema.py
    test_thread_pool.py
    test_typed.py
)

END()

RECURSE(
    py23_fork
)
