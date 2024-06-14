RECURSE(
    build
    benchmarks
    client
    core
    experiments
    library
    python
    tools
    ytlib
)

IF (NOT OPENSOURCE)
    RECURSE(
        flow
        fuzz
        orm
        packages/tests_package
        utilities
        scripts
    )
ENDIF()

IF (OS_LINUX)
    RECURSE(server)
ENDIF()

IF (NOT SANITIZER_TYPE)
    RECURSE(
        tests
    )
ENDIF()
