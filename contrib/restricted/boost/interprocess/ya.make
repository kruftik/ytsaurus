# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(
    BSL-1.0 AND
    Mit-Old-Style
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.85.0)

ORIGINAL_SOURCE(https://github.com/boostorg/interprocess/archive/boost-1.85.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/assert
    contrib/restricted/boost/config
    contrib/restricted/boost/container
    contrib/restricted/boost/core
    contrib/restricted/boost/integer
    contrib/restricted/boost/intrusive
    contrib/restricted/boost/move
    contrib/restricted/boost/static_assert
    contrib/restricted/boost/type_traits
    contrib/restricted/boost/unordered
    contrib/restricted/boost/winapi
)

ADDINCL(
    GLOBAL contrib/restricted/boost/interprocess/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()
