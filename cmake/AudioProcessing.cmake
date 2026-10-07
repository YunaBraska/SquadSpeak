# Build the pinned upstream project with its own source lists and CPU dispatch.
# A private shared library contains static Abseil, avoiding an unpinned ABI at
# runtime. Meson and Ninja are build tools only.
find_program(SQUAD_MESON meson REQUIRED)
include(ExternalProject)
set(squad_apm_prefix "${CMAKE_CURRENT_BINARY_DIR}/audio-processing")
if(WIN32)
    set(squad_apm_library "${squad_apm_prefix}/bin/webrtc-audio-processing-2-1.dll")
    set(squad_apm_implib "${squad_apm_prefix}/lib/webrtc-audio-processing-2.lib")
    set(squad_apm_byproducts "${squad_apm_library}" "${squad_apm_implib}")
else()
    set(squad_apm_library "${squad_apm_prefix}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}webrtc-audio-processing-2${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(squad_apm_byproducts "${squad_apm_library}")
endif()
file(MAKE_DIRECTORY "${squad_apm_prefix}/include/webrtc-audio-processing-2")

file(TO_CMAKE_PATH "${CMAKE_C_COMPILER}" squad_apm_c_compiler)
file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" squad_apm_cxx_compiler)
set(apm_native "[binaries]\nc = '${squad_apm_c_compiler}'\ncpp = '${squad_apm_cxx_compiler}'\n[built-in options]\n")
foreach(language IN ITEMS C CXX)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
    separate_arguments(arguments NATIVE_COMMAND "${CMAKE_${language}_FLAGS} ${CMAKE_${language}_FLAGS_${build_type}}")
    if(APPLE)
        list(APPEND arguments "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        foreach(architecture IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND arguments -arch "${architecture}")
        endforeach()
        if(CMAKE_OSX_SYSROOT)
            list(APPEND arguments -isysroot "${CMAKE_OSX_SYSROOT}")
        endif()
    endif()
    set(meson_arguments "")
    foreach(argument IN LISTS arguments)
        string(REPLACE "\\" "\\\\" argument "${argument}")
        string(REPLACE "'" "\\'" argument "${argument}")
        string(APPEND meson_arguments "'${argument}',")
    endforeach()
    if(language STREQUAL "C")
        set(meson_language c)
    else()
        set(meson_language cpp)
    endif()
    set(compile_arguments "${meson_arguments}")
    if(language STREQUAL "CXX")
        # Upstream trace_event.h relies on a transitive <cstdint> include
        # removed by libstdc++ 15. Supply its required standard declarations.
        if(MSVC)
            string(APPEND compile_arguments "'/FIcstdint',")
        else()
            string(APPEND compile_arguments "'-include','cstdint',")
        endif()
    endif()
    string(APPEND apm_native "${meson_language}_args = [${compile_arguments}]\n${meson_language}_link_args = [${meson_arguments}]\n")
endforeach()
file(CONFIGURE OUTPUT "${squad_apm_prefix}/native.ini" CONTENT "${apm_native}" @ONLY)
set(apm_options)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*address")
    # Clang supplies the ASan runtime from the executable, not shared objects.
    list(APPEND apm_options -Db_lundef=false)
endif()
ExternalProject_Add(squad_apm_build
    URL https://gstreamer.freedesktop.org/src/mirror/webrtc-audio-processing/webrtc-audio-processing-2.1.tar.gz
    URL_HASH SHA256=35e86b986d02ea15f3d04741a1a5a735ba399bc0fac0ee089c39480e35fc3253
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND "${SQUAD_MESON}" setup --wipe <BINARY_DIR> <SOURCE_DIR>
        --prefix "${squad_apm_prefix}" --libdir lib --buildtype release
        --native-file "${squad_apm_prefix}/native.ini" --wrap-mode forcefallback
        -Ddefault_library=shared -Dabseil-cpp:default_library=static
        -Dcpp_std=c++20 -Dabseil-cpp:cpp_std=c++20 ${apm_options}
    BUILD_COMMAND "${SQUAD_MESON}" compile -C <BINARY_DIR> -j 3
    INSTALL_COMMAND "${SQUAD_MESON}" install -C <BINARY_DIR>
    BUILD_BYPRODUCTS ${squad_apm_byproducts})
# Meson caches machine-file flags. Track their file and recreate its build
# configuration when compiler, architecture or sanitizer arguments change.
ExternalProject_Add_Step(squad_apm_build settings
    DEPENDEES patch DEPENDERS configure DEPENDS "${squad_apm_prefix}/native.ini")
add_library(squad_apm SHARED IMPORTED GLOBAL)
set_target_properties(squad_apm PROPERTIES IMPORTED_LOCATION "${squad_apm_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${squad_apm_prefix}/include/webrtc-audio-processing-2;${squad_apm_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "WEBRTC_LIBRARY_IMPL")
if(WIN32)
    set_target_properties(squad_apm PROPERTIES IMPORTED_IMPLIB "${squad_apm_implib}")
endif()
add_dependencies(squad_apm squad_apm_build)
if(UNIX)
    set_property(TARGET squad_apm APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS WEBRTC_POSIX)
elseif(WIN32)
    set_property(TARGET squad_apm APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS WEBRTC_WIN)
endif()
