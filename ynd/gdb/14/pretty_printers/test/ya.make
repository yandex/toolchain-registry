PY2TEST()

SUBSCRIBER(orivej halyavin g:yatool)

DATA(
    arcadia/devtools/gdb
)

DEPENDS(
    devtools/gdb/test/gdbtest
    devtools/gdb/test/tconts_stub
)

TEST_SRCS(
    test.py
    test_tconts.py
)

FORK_SUBTESTS()
SPLIT_FACTOR(16)

END()

RECURSE(
    gdbtest
    tconts_stub
)
