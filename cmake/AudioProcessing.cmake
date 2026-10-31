# Build the pinned upstream project with its own source lists and CPU dispatch.
# A private shared library contains static Abseil, avoiding an unpinned ABI at
# runtime. Meson and Ninja are build tools only.
find_program(SQUAD_MESON meson REQUIRED)
find_package(Git REQUIRED)
include(ExternalProject)
set(squad_apm_prefix "${CMAKE_CURRENT_BINARY_DIR}/audio-processing")
if(SQUADSPEAK_IOS OR ANDROID)
    set(squad_apm_library "${squad_apm_prefix}/lib/libwebrtc-audio-processing-2.a")
    set(squad_apm_byproducts "${squad_apm_library}")
elseif(WIN32)
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
set(apm_native "[binaries]\n")
foreach(language IN ITEMS C CXX)
    if(language STREQUAL "C")
        set(binary c)
    else()
        set(binary cpp)
    endif()
    set(command ${CMAKE_${language}_COMPILER_LAUNCHER} "${CMAKE_${language}_COMPILER}")
    list(TRANSFORM command REPLACE "\\\\" "/")
    list(TRANSFORM command REPLACE "'" "\\\\'")
    string(REPLACE ";" "','" command "${command}")
    string(APPEND apm_native "${binary} = ['${command}']\n")
endforeach()
string(APPEND apm_native "[built-in options]\n")
if(ANDROID)
    set(apm_native "[binaries]\nc = '${squad_apm_c_compiler}'\ncpp = '${squad_apm_cxx_compiler}'\nar = '${CMAKE_AR}'\nstrip = '${CMAKE_STRIP}'\n[built-in options]\n")
endif()
foreach(language IN ITEMS C CXX)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
    separate_arguments(arguments NATIVE_COMMAND "${CMAKE_${language}_FLAGS} ${CMAKE_${language}_FLAGS_${build_type}}")
    if(ANDROID)
        list(APPEND arguments "--target=${CMAKE_${language}_COMPILER_TARGET}" "--sysroot=${CMAKE_SYSROOT}" -fPIC)
    endif()
    if(APPLE)
        if(SQUADSPEAK_IOS)
            if(CMAKE_OSX_SYSROOT MATCHES "[Ii][Pp]hone[Ss]imulator")
                list(APPEND arguments "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
            else()
                list(APPEND arguments "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
            endif()
        else()
            list(APPEND arguments "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
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
if(SQUADSPEAK_IOS OR ANDROID)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
        set(squad_apm_cpu_family x86_64)
    else()
        set(squad_apm_cpu_family aarch64)
    endif()
    set(squad_apm_system darwin)
    if(ANDROID)
        set(squad_apm_system android)
    endif()
    set(apm_cross "${apm_native}[host_machine]\nsystem = '${squad_apm_system}'\ncpu_family = '${squad_apm_cpu_family}'\ncpu = '${squad_apm_cpu_family}'\nendian = 'little'\n")
    file(CONFIGURE OUTPUT "${squad_apm_prefix}/cross.ini" CONTENT "${apm_cross}" @ONLY)
    set(apm_machine_file --cross-file "${squad_apm_prefix}/cross.ini")
    set(apm_default_library static)
else()
    file(CONFIGURE OUTPUT "${squad_apm_prefix}/native.ini" CONTENT "${apm_native}" @ONLY)
    set(apm_machine_file --native-file "${squad_apm_prefix}/native.ini")
    set(apm_default_library shared)
endif()
set(apm_options)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*address")
    # Clang supplies the ASan runtime from the executable, not shared objects.
    list(APPEND apm_options -Db_lundef=false)
endif()
# Expose AEC3 residual suppression without a second processing path. The private
# library and its consumers are always built together with the patched headers.
file(CONFIGURE OUTPUT "${squad_apm_prefix}/apply-patch.cmake" CONTENT [[
execute_process(COMMAND "@GIT_EXECUTABLE@" apply --reverse --check "${patch}"
    WORKING_DIRECTORY "${source}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(NOT applied EQUAL 0)
    execute_process(COMMAND "@GIT_EXECUTABLE@" apply "${patch}"
        WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
endif()
]] @ONLY)
ExternalProject_Add(squad_apm_build
    URL https://gstreamer.freedesktop.org/src/mirror/webrtc-audio-processing/webrtc-audio-processing-2.1.tar.gz
    URL_HASH SHA256=35e86b986d02ea15f3d04741a1a5a735ba399bc0fac0ee089c39480e35fc3253
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND "${CMAKE_COMMAND}" -Dsource=<SOURCE_DIR>
        "-Dpatch=${CMAKE_CURRENT_SOURCE_DIR}/cmake/audio-processing-echo.patch"
        -P "${squad_apm_prefix}/apply-patch.cmake"
    CONFIGURE_COMMAND "${SQUAD_MESON}" setup --wipe <BINARY_DIR> <SOURCE_DIR>
        --prefix "${squad_apm_prefix}" --libdir lib --buildtype release
        ${apm_machine_file} --wrap-mode forcefallback
        -Ddefault_library=${apm_default_library} -Dabseil-cpp:default_library=static
        -Dcpp_std=c++20 -Dabseil-cpp:cpp_std=c++20 ${apm_options}
    BUILD_COMMAND "${SQUAD_MESON}" compile -C <BINARY_DIR> -j 3
    INSTALL_COMMAND "${SQUAD_MESON}" install -C <BINARY_DIR>
    BUILD_BYPRODUCTS ${squad_apm_byproducts})
# Meson caches machine-file flags. Track their file and recreate its build
# configuration when compiler, architecture or sanitizer arguments change.
if(SQUADSPEAK_IOS OR ANDROID)
    set(squad_apm_machine_file_dependency "${squad_apm_prefix}/cross.ini")
else()
    set(squad_apm_machine_file_dependency "${squad_apm_prefix}/native.ini")
endif()
ExternalProject_Add_Step(squad_apm_build settings
    DEPENDEES patch DEPENDERS configure DEPENDS "${squad_apm_machine_file_dependency}")
ExternalProject_Add_StepDependencies(squad_apm_build patch
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/audio-processing-echo.patch")
if(SQUADSPEAK_IOS OR ANDROID)
    add_library(squad_apm STATIC IMPORTED GLOBAL)
else()
    add_library(squad_apm SHARED IMPORTED GLOBAL)
endif()
set_target_properties(squad_apm PROPERTIES IMPORTED_LOCATION "${squad_apm_library}"
    # The private headers must outrank ambient Abseil installations. Treating
    # them as system headers lets CPATH silently substitute an incompatible ABI.
    IMPORTED_NO_SYSTEM TRUE
    INTERFACE_INCLUDE_DIRECTORIES "${squad_apm_prefix}/include/webrtc-audio-processing-2;${squad_apm_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "WEBRTC_LIBRARY_IMPL")
if(WIN32)
    set_target_properties(squad_apm PROPERTIES IMPORTED_IMPLIB "${squad_apm_implib}")
endif()
add_dependencies(squad_apm squad_apm_build)
if(ANDROID)
    set_property(TARGET squad_apm APPEND PROPERTY INTERFACE_LINK_LIBRARIES log)
endif()
if(UNIX)
    set_property(TARGET squad_apm APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS WEBRTC_POSIX)
elseif(WIN32)
    set_property(TARGET squad_apm APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS WEBRTC_WIN)
endif()
