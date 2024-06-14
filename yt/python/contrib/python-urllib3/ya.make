PY23_LIBRARY()

NO_LINT()

IF (PYTHON2)
    PEERDIR(yt/python_py2/contrib/python-urllib3)
ELSE()
    SRCDIR(yt/python/contrib/python-urllib3/src)

    PY_SRCS(
        NAMESPACE yt.packages

        urllib3/_version.py
        urllib3/filepost.py
        urllib3/connection.py
        urllib3/exceptions.py
        urllib3/contrib/pyopenssl.py
        urllib3/contrib/ntlmpool.py
        urllib3/contrib/__init__.py
        urllib3/contrib/_securetransport/__init__.py
        urllib3/contrib/_securetransport/bindings.py
        urllib3/contrib/_securetransport/low_level.py
        urllib3/contrib/appengine.py
        urllib3/contrib/socks.py
        urllib3/contrib/_appengine_environ.py
        urllib3/contrib/securetransport.py
        urllib3/poolmanager.py
        urllib3/__init__.py
        urllib3/fields.py
        urllib3/connectionpool.py
        urllib3/response.py
        urllib3/request.py
        urllib3/_collections.py
        urllib3/packages/six.py
        urllib3/packages/__init__.py
        urllib3/packages/backports/__init__.py
        urllib3/packages/backports/makefile.py
        urllib3/util/ssl_.py
        urllib3/util/connection.py
        urllib3/util/queue.py
        urllib3/util/wait.py
        urllib3/util/proxy.py
        urllib3/util/__init__.py
        urllib3/util/timeout.py
        urllib3/util/ssltransport.py
        urllib3/util/retry.py
        urllib3/util/response.py
        urllib3/util/ssl_match_hostname.py
        urllib3/util/request.py
        urllib3/util/url.py
    )
ENDIF()

END()
