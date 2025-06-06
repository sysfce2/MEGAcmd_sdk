macro(process_vcpkg_libraries overlays_path)

    set(VCPKG_TOOLCHAIN_PATH "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")

    if (NOT EXISTS ${VCPKG_TOOLCHAIN_PATH})
        message(FATAL_ERROR "Invalid VCPKG_ROOT path: ${VCPKG_ROOT}")
    endif()

    # Use internal VCPKG tools
    set(VCPKG_BOOTSTRAP_OPTIONS "-disableMetrics")
    foreach(path IN ITEMS ${overlays_path})
        list(APPEND VCPKG_OVERLAY_PORTS "${path}/vcpkg_overlay_ports")
        list(APPEND VCPKG_OVERLAY_TRIPLETS "${path}/vcpkg_overlay_triplets")
    endforeach()
    list(REMOVE_DUPLICATES VCPKG_OVERLAY_PORTS)
    list(REMOVE_DUPLICATES VCPKG_OVERLAY_TRIPLETS)

    if(NOT VCPKG_TARGET_TRIPLET)
        # Try to guess the triplet if it is not set.
        if(CMAKE_SYSTEM_NAME STREQUAL "Android")
            if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
                set(VCPKG_TARGET_TRIPLET "arm64-android-mega")
            elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
                set(VCPKG_TARGET_TRIPLET "arm-android-mega")
            elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
                set(VCPKG_TARGET_TRIPLET "x64-android-mega")
            elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86")
                set(VCPKG_TARGET_TRIPLET "x86-android-mega")
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
                # Support only for iphonesimulator
                if(CMAKE_OSX_SYSROOT STREQUAL "iphonesimulator")
                    set(VCPKG_TARGET_TRIPLET "x64-ios-simulator-mega")
                endif()
            else() # if CMAKE_OSX_ARCHITECTURES is arm64 or if it is empty. Empty builds for arm64 by default.
                if(CMAKE_OSX_SYSROOT STREQUAL "iphonesimulator")
                    set(VCPKG_TARGET_TRIPLET "arm64-ios-simulator-mega")
                else()
                    set(VCPKG_TARGET_TRIPLET "arm64-ios-mega")
                endif()
            endif()
        elseif(APPLE) # macOS
            if (CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"))
                set(VCPKG_TARGET_TRIPLET "x64-osx-mega")
            else()
                set(VCPKG_TARGET_TRIPLET "arm64-osx-mega")
            endif()
        elseif(WIN32)
            if(CMAKE_GENERATOR_PLATFORM MATCHES "Win32")
                set(VCPKG_TARGET_TRIPLET "x86-windows-mega")
            elseif(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
                set(VCPKG_TARGET_TRIPLET "arm64-windows-mega")
            else()
                set(VCPKG_TARGET_TRIPLET "x64-windows-mega")
            endif()
        else() # Linux
            if (CMAKE_SYSTEM_PROCESSOR MATCHES "armv7l" OR (NOT CMAKE_SYSTEM_PROCESSOR AND HOST_ARCH MATCHES "armv7l"))
                set(VCPKG_TARGET_TRIPLET "arm-linux")
            elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" OR (NOT CMAKE_SYSTEM_PROCESSOR AND HOST_ARCH MATCHES "aarch64|arm64"))
                set(VCPKG_TARGET_TRIPLET "arm64-linux-mega")
            else()
                set(VCPKG_TARGET_TRIPLET "x64-linux-mega")
            endif()
        endif()
    endif()

    if (USE_OPENSSL)
        list(APPEND VCPKG_MANIFEST_FEATURES "use-openssl")
    endif()

    if (USE_MEDIAINFO)
        list(APPEND VCPKG_MANIFEST_FEATURES "use-mediainfo")
    endif()

    if (USE_FREEIMAGE)
        list(APPEND VCPKG_MANIFEST_FEATURES "use-freeimage")
    endif()

    if (USE_FFMPEG)
        list(APPEND VCPKG_MANIFEST_FEATURES "use-ffmpeg")
        # Remove -flto[=n] from CFLAGS if set. It cause link errors in ffmpeg due to assembler code
        string(REGEX MATCH "-flto[^ \r\n]*" LTO_MATCHES "$ENV{CFLAGS}")
        if(LTO_MATCHES)
            message(STATUS "Removing ${LTO_MATCHES} from the environment CFLAGS variable")
            string(REPLACE "${LTO_MATCHES}" "" NEW_CFLAGS "$ENV{CFLAGS}")
            set(ENV{CFLAGS} "${NEW_CFLAGS}")
        endif()
    endif()

    if (USE_LIBUV)
        list(APPEND VCPKG_MANIFEST_FEATURES "use-libuv")
    endif()

    if (USE_PDFIUM)
        list(APPEND VCPKG_MANIFEST_FEATURES "use-pdfium")
    endif()

    if (USE_READLINE)
        list(APPEND VCPKG_MANIFEST_FEATURES "use-readline")
    endif()

    if (ENABLE_SDKLIB_TESTS)
        list(APPEND VCPKG_MANIFEST_FEATURES "sdk-tests")
    endif()

    if (ENABLE_C_ARES_BACKEND)
        list(APPEND VCPKG_MANIFEST_FEATURES "c-ares-backend-curl")
    endif()

    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE} ${VCPKG_TOOLCHAIN_PATH})
    message(STATUS "Using VCPKG dependencies. VCPKG base path: ${VCPKG_ROOT} and tripplet ${VCPKG_TARGET_TRIPLET}")
    message(STATUS "Overlay for VCPKG ports: ${VCPKG_OVERLAY_PORTS}")
    message(STATUS "Overlay for VCPKG triplets: ${VCPKG_OVERLAY_TRIPLETS}")

endmacro()
