# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.85.0)

ORIGINAL_SOURCE(https://github.com/boostorg/coroutine/archive/boost-1.85.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/assert
    contrib/restricted/boost/config
    contrib/restricted/boost/context/fcontext_impl
    contrib/restricted/boost/core
    contrib/restricted/boost/exception
    contrib/restricted/boost/move
    contrib/restricted/boost/system
    contrib/restricted/boost/throw_exception
    contrib/restricted/boost/type_traits
    contrib/restricted/boost/utility
)

ADDINCL(
    GLOBAL contrib/restricted/boost/coroutine/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    GLOBAL -DBOOST_COROUTINES_NO_DEPRECATION_WARNING
    -DBOOST_COROUTINES_SOURCE
)

IF (DYNAMIC_BOOST)
    CFLAGS(
        GLOBAL -DBOOST_COROUTINES_DYN_LINK
    )
ENDIF()

IF (OS_WINDOWS)
    SRCS(
        src/windows/stack_traits.cpp
    )
ELSE()
    SRCS(
        src/posix/stack_traits.cpp
    )
ENDIF()

SRCS(
    src/detail/coroutine_context.cpp
    src/exceptions.cpp
)

END()
