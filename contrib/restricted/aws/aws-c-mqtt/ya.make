# Generated by devtools/yamaker from nixpkgs 23.05.

LIBRARY()

LICENSE(Apache-2.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(0.8.8)

ORIGINAL_SOURCE(https://github.com/awslabs/aws-c-mqtt/archive/v0.8.8.tar.gz)

PEERDIR(
    contrib/restricted/aws/aws-c-common
    contrib/restricted/aws/aws-c-http
    contrib/restricted/aws/aws-c-io
)

ADDINCL(
    GLOBAL contrib/restricted/aws/aws-c-mqtt/include
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DAWS_CAL_USE_IMPORT_EXPORT
    -DAWS_COMMON_USE_IMPORT_EXPORT
    -DAWS_COMPRESSION_USE_IMPORT_EXPORT
    -DAWS_HTTP_USE_IMPORT_EXPORT
    -DAWS_IO_USE_IMPORT_EXPORT
    -DAWS_MQTT_USE_IMPORT_EXPORT
    -DAWS_MQTT_WITH_WEBSOCKETS
    -DAWS_USE_EPOLL
    -DHAVE_SYSCONF
    -DS2N_CLONE_SUPPORTED
    -DS2N_CPUID_AVAILABLE
    -DS2N_FALL_THROUGH_SUPPORTED
    -DS2N_FEATURES_AVAILABLE
    -DS2N_KYBER512R3_AVX2_BMI2
    -DS2N_LIBCRYPTO_SUPPORTS_EVP_MD5_SHA1_HASH
    -DS2N_LIBCRYPTO_SUPPORTS_EVP_MD_CTX_SET_PKEY_CTX
    -DS2N_LIBCRYPTO_SUPPORTS_EVP_RC4
    -DS2N_MADVISE_SUPPORTED
    -DS2N_PLATFORM_SUPPORTS_KTLS
    -DS2N_STACKTRACE
    -DS2N___RESTRICT__SUPPORTED
)

SRCS(
    source/client.c
    source/client_channel_handler.c
    source/fixed_header.c
    source/mqtt.c
    source/packets.c
    source/shared_constants.c
    source/topic_tree.c
    source/v5/mqtt5_callbacks.c
    source/v5/mqtt5_client.c
    source/v5/mqtt5_decoder.c
    source/v5/mqtt5_encoder.c
    source/v5/mqtt5_listener.c
    source/v5/mqtt5_options_storage.c
    source/v5/mqtt5_topic_alias.c
    source/v5/mqtt5_types.c
    source/v5/mqtt5_utils.c
    source/v5/rate_limiters.c
)

END()
