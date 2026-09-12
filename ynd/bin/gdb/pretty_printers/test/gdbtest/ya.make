PROGRAM(gdbtest)

SUBSCRIBER(halyavin g:yatool)

CFLAGS(
    -g
)

IF (CLANG)
    # Emit complete control-block types even when libc++ has only line tables.
    CXXFLAGS(-fstandalone-debug)
ENDIF()

SRCS(
    main.cpp
)

PEERDIR(
    library/cpp/enumbitset
    taxi/uservices/userver/universal
)

END()
