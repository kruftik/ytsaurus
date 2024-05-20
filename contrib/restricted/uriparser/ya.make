# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(BSD-3-Clause)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(0.9.8)

ORIGINAL_SOURCE(https://github.com/uriparser/uriparser/archive/uriparser-0.9.8.tar.gz)

PEERDIR(
    contrib/libs/libc_compat
)

ADDINCL(
    GLOBAL contrib/restricted/uriparser/include
    contrib/restricted/uriparser
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DURI_LIBRARY_BUILD
    -DURI_VISIBILITY
)

SRCS(
    src/UriCommon.c
    src/UriCompare.c
    src/UriEscape.c
    src/UriFile.c
    src/UriIp4.c
    src/UriIp4Base.c
    src/UriMemory.c
    src/UriNormalize.c
    src/UriNormalizeBase.c
    src/UriParse.c
    src/UriParseBase.c
    src/UriQuery.c
    src/UriRecompose.c
    src/UriResolve.c
    src/UriShorten.c
)

END()

RECURSE(
    test
    tool
)
