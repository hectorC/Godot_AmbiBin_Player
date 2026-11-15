cmake_minimum_required(VERSION 3.24)
project(GodotAmbiBinPlayer LANGUAGES C CXX)

if(WIN32)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR NOT MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64")
        message(FATAL_ERROR "Windows builds use MSVC x64.")
    endif()
    set(AMBI_MKL_ROOT "C:/Program Files (x86)/Intel/oneAPI/mkl/2025.3"
        CACHE PATH "Intel oneMKL installation")
    set(INTEL_MKL_HEADER_PATH "${AMBI_MKL_ROOT}/include")
    set(INTEL_MKL_LIB
        "${AMBI_MKL_ROOT}/lib/mkl_intel_lp64.lib"
        "${AMBI_MKL_ROOT}/lib/mkl_sequential.lib"
        "${AMBI_MKL_ROOT}/lib/mkl_core.lib")
    foreach(required_file IN ITEMS "${INTEL_MKL_HEADER_PATH}/mkl.h" ${INTEL_MKL_LIB})
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR "Missing oneMKL file: ${required_file}. Set AMBI_MKL_ROOT.")
        endif()
    endforeach()
    set(SAF_PERFORMANCE_LIB SAF_USE_INTEL_MKL_LP64)
    set(ambi_warnings /W4 /EHsc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SAF_PERFORMANCE_LIB SAF_USE_APPLE_ACCELERATE_LP64)
    set(ambi_warnings -Wall -Wextra)
else()
    message(FATAL_ERROR "Supported build platforms are Windows x64 and macOS.")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
FetchContent_Declare(saf
    GIT_REPOSITORY https://github.com/leomccormack/Spatial_Audio_Framework.git
    GIT_TAG 18fd5aba46e20787b51f28f7197a68506c965c07)
FetchContent_Declare(dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG f12e197fd462e4c22873fbd70d11b1d2f2c1dc3c)

set(BUILD_SHARED_LIBS OFF)
set(SAF_BUILD_TESTS OFF)
set(SAF_BUILD_EXAMPLES ON)
set(SAF_BUILD_EXTRAS OFF)
set(SAF_ENABLE_SOFA_READER_MODULE ON)
set(SAF_ENABLE_TRACKER_MODULE OFF)
set(SAF_ENABLE_HADES_MODULE OFF)
set(SAF_USE_INTEL_IPP OFF)
set(SAF_USE_FFTW OFF)
set(SAF_ENABLE_SIMD OFF)
set(saf_example_list ambi_bin)
FetchContent_MakeAvailable(saf dr_libs)

add_library(ambi_decoder STATIC src/ambisonic_decoder.cpp src/temporary_file.cpp)
target_link_libraries(ambi_decoder PRIVATE saf_example_ambi_bin saf)
target_include_directories(ambi_decoder SYSTEM PRIVATE
    "${saf_SOURCE_DIR}/framework/modules/saf_sofa_reader/libmysofa")
target_compile_features(ambi_decoder PUBLIC cxx_std_20)
target_compile_options(ambi_decoder PRIVATE ${ambi_warnings})
target_include_directories(ambi_decoder PUBLIC src)

add_library(ambi_wav STATIC src/wav_io.cpp)
target_link_libraries(ambi_wav PUBLIC ambi_decoder)
target_include_directories(ambi_wav SYSTEM PRIVATE "${dr_libs_SOURCE_DIR}")
target_compile_options(ambi_wav PRIVATE ${ambi_warnings})

add_executable(ambi_render src/main.cpp)
target_link_libraries(ambi_render PRIVATE ambi_wav)
target_compile_options(ambi_render PRIVATE ${ambi_warnings})

option(AMBI_BUILD_GODOT "Build the Godot 4.8 GDExtension and prepare the demo project" OFF)
if(AMBI_BUILD_GODOT)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    FetchContent_Declare(godot-cpp
        GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
        GIT_TAG 2a4c066e1e8c121046e9d35fe91cc8e726861c48)
    set(GODOTCPP_API_VERSION 4.8)
    set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/src/godot/build_profile.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GODOTCPP_BUILD_PROFILE}")
    set(GODOTCPP_DISABLE_EXCEPTIONS OFF)
    FetchContent_MakeAvailable(godot-cpp)
    set_target_properties(godot-cpp PROPERTIES
        SYSTEM ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")

    add_library(godot_ambi SHARED src/godot/ambisonic_stream.cpp)
    target_link_libraries(godot_ambi PRIVATE ambi_wav godot::cpp)
    target_compile_options(godot_ambi PRIVATE ${ambi_warnings})
    set(demo_dir "${CMAKE_CURRENT_SOURCE_DIR}/demo_godot")
    set_target_properties(godot_ambi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${demo_dir}/addons/ambisonic_player/bin/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${demo_dir}/addons/ambisonic_player/bin/$<CONFIG>")
    set(demo_fixtures "${demo_dir}/fixtures")
    set(demo_wavs "${demo_fixtures}/fixed-sources.wav")
    foreach(order RANGE 2 5)
        list(APPEND demo_wavs "${demo_fixtures}/fixed-sources-order-${order}.wav")
    endforeach()
    add_custom_command(OUTPUT ${demo_wavs}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/higher_orders.py"
                --fixtures-only "${demo_fixtures}"
        DEPENDS tests/higher_orders.py tests/rotation.py tests/wav_fixture.py
        VERBATIM)
    add_custom_target(ambi_demo_fixture DEPENDS ${demo_wavs})
    add_dependencies(godot_ambi ambi_demo_fixture)

    set(addon_install "addons/ambisonic_player")
    install(TARGETS godot_ambi
        RUNTIME DESTINATION "${addon_install}/bin/Release"
        LIBRARY DESTINATION "${addon_install}/bin/Release"
        CONFIGURATIONS Release)
    install(DIRECTORY "${demo_dir}/addons/ambisonic_player/" DESTINATION "${addon_install}"
        FILES_MATCHING PATTERN "*.gd" PATTERN "*.uid" PATTERN "*.cfg" PATTERN "*.gdextension"
        PATTERN "bin" EXCLUDE)
    install(FILES LICENSE LICENSES.md DESTINATION "${addon_install}")
    install(FILES docs/addon-readme.md DESTINATION "${addon_install}" RENAME README.md)
    install(FILES LICENSE .gitignore DESTINATION ".")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME standalone_foa_render
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_smoke.py"
                "$<TARGET_FILE:ambi_render>" "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>")
    add_test(NAME listener_rotation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rotation.py"
                "$<TARGET_FILE:ambi_render>" "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/rotation")
    add_test(NAME higher_order_render
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/higher_orders.py"
                "$<TARGET_FILE:ambi_render>" "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/higher-orders")
    set(AMBI_GODOT_EXE "" CACHE FILEPATH "Godot 4.8 executable for the headless demo test")
    if(AMBI_BUILD_GODOT AND AMBI_GODOT_EXE)
        if(NOT EXISTS "${AMBI_GODOT_EXE}")
            message(FATAL_ERROR "AMBI_GODOT_EXE does not exist: ${AMBI_GODOT_EXE}")
        endif()
        set_tests_properties(listener_rotation PROPERTIES FIXTURES_SETUP listener_reference)
        set_tests_properties(standalone_foa_render PROPERTIES FIXTURES_SETUP pcm_fixtures)
        add_test(NAME godot_demo
            CONFIGURATIONS Release
            COMMAND "${CMAKE_COMMAND}"
                    "-DGODOT=${AMBI_GODOT_EXE}"
                    "-DDEMO=${demo_dir}"
                    "-DSMOKE=${CMAKE_CURRENT_SOURCE_DIR}/tests/godot_smoke.gd"
                    "-DREFERENCE=${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/rotation/reference-neutral.wav"
                    "-DLOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_godot_smoke.cmake")
        set_tests_properties(godot_demo PROPERTIES FIXTURES_REQUIRED "listener_reference;pcm_fixtures" TIMEOUT 70)
    endif()
endif()
