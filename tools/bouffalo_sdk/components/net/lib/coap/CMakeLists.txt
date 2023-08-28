sdk_generate_library()

sdk_add_include_directories(libcoap/include/coap3)

sdk_library_add_sources(
    libcoap/src/coap_address.c
    libcoap/src/coap_asn1.c
    libcoap/src/coap_async.c
    libcoap/src/coap_cache.c
    libcoap/src/coap_debug.c
    libcoap/src/coap_event.c
    libcoap/src/coap_gnutls.c
    libcoap/src/coap_hashkey.c
    libcoap/src/coap_io.c
    libcoap/src/coap_io_lwip.c
    libcoap/src/coap_io_riot.c
    libcoap/src/coap_mbedtls.c
    libcoap/src/coap_notls.c
    libcoap/src/coap_openssl.c
    libcoap/src/coap_option.c
    libcoap/src/coap_prng.c
    libcoap/src/coap_session.c
    libcoap/src/coap_subscribe.c
    libcoap/src/coap_tcp.c
    libcoap/src/coap_time.c
    libcoap/src/coap_tinydtls.c
    libcoap/src/block.c
    libcoap/src/encode.c
    libcoap/src/mem.c
    libcoap/src/net.c
    libcoap/src/pdu.c
    libcoap/src/resource.c
    libcoap/src/str.c
    libcoap/src/uri.c
    )

    

