add_library(Unity STATIC
    Unity/src/unity.c
)

target_include_directories(Unity PUBLIC
    Unity/src
)

target_compile_definitions(Unity PUBLIC
    UNITY_SUPPORT_64
    UNITY_INCLUDE_DOUBLE
)
