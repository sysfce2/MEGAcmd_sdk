add_executable(gfxworker_test_integration
    executable_dir.h
    executable_dir.cpp
    main.cpp
    server_client_test.cpp
)

target_link_libraries(gfxworker_test_integration
    PRIVATE
    libgfxworker
    MEGA::test_tools
    MEGA::SDKlib
)

# Look for the libraries needed for both integration and unit tests.
if(VCPKG_ROOT)
    find_package(GTest CONFIG REQUIRED)
    target_link_libraries(gfxworker_test_integration PRIVATE GTest::gtest)
else()
    pkg_check_modules(gtest REQUIRED IMPORTED_TARGET gtest)
    target_link_libraries(gfxworker_test_integration PRIVATE PkgConfig::gtest)
endif()

# Adjust compilation flags for warnings and errors for libgfxworker
target_platform_compile_options(
    TARGET gfxworker_test_integration
    WINDOWS /W4
            /we4800 # Implicit conversion from 'type' to bool. Possible information loss
    UNIX $<$<CONFIG:Debug>:-ggdb3> -Wall -Wextra -Wconversion
)

if(ENABLE_SDKLIB_WERROR)
    target_platform_compile_options(
        TARGET gfxworker_test_integration
        WINDOWS /WX
        UNIX  $<$<CONFIG:Debug>: -Werror>
    )
endif()
