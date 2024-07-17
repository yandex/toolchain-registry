PROGRAM(tconts_stub)

SUBSCRIBER(halyavin g:yatool)

CXXFLAGS(
    -g
    -fno-eliminate-unused-debug-types
    -O0
)

SRCS(
    main.cpp
)

PEERDIR(
    library/cpp/coroutine/engine
)

END()
