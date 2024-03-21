PY3TEST()

SET(YT_SPLIT_FACTOR 70)

ENV(YT_TEST_FILTER=MEDIUM)

INCLUDE(../YaMakeDependsBoilerplate.txt)

REQUIREMENTS(
    cpu:22
)

IF (SANITIZER_TYPE)
    REQUIREMENTS(
        ram:40
    )
ELSE()
    REQUIREMENTS(
        ram:16
    )
ENDIF()

END()
