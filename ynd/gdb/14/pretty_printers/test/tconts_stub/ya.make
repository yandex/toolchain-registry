PROGRAM(tconts_stub)

OWNER(orivej halyavin g:yatool)

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
