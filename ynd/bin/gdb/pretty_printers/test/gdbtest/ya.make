PROGRAM(gdbtest)

SUBSCRIBER(halyavin g:yatool)

CFLAGS(
    -g
)

SRCS(
    main.cpp
)

PEERDIR(
    library/cpp/enumbitset
    taxi/uservices/userver/universal
)

END()
