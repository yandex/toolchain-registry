LIBRARY()

SUBSCRIBER(g:yatool g:cpp-contrib)

PEERDIR(
    contrib/libs/clang16/lib/AST
    contrib/libs/clang16/lib/ASTMatchers
)

NO_COMPILER_WARNINGS()
NO_UTIL()

SRCS(GLOBAL tidy_module.cpp)

SRCS(
    ascii_compare_ignore_case_check.cpp
    taxi_coroutine_unsafe_check.cpp
    taxi_dangling_config_ref_check.cpp
    taxi_async_use_after_free_check.cpp
    usage_restriction_checks.cpp
    uneeded_temporary_string_check.cpp
    util_tstring_methods.cpp
    possible_nullptr_check.cpp
)

END()
