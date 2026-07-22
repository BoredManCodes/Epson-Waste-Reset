cmake_minimum_required(VERSION 3.15)
project(EWR VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIB_SOURCES src/parser.cpp src/generator.cpp src/maintenance.cpp)

if(WIN32)
    list(APPEND LIB_SOURCES src/usb_windows.cpp src/snmp_windows.cpp)
else()
    list(APPEND LIB_SOURCES src/usb_linux.cpp)
endif()

add_library(ewr_core STATIC ${LIB_SOURCES})

target_include_directories(ewr_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
)

if(WIN32)
    target_link_libraries(ewr_core PRIVATE setupapi urlmon shell32 ws2_32)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    pkg_check_modules(LIBCURL REQUIRED libcurl)
    
    target_include_directories(ewr_core PRIVATE ${LIBUSB_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS})
    target_link_libraries(ewr_core PRIVATE ${LIBUSB_LIBRARIES} ${LIBCURL_LIBRARIES})
endif()

add_executable(ewr cli/main.cpp)
target_link_libraries(ewr PRIVATE ewr_core)
set_target_properties(ewr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
    enable_language(RC)
    add_executable(ewr-gui WIN32 gui/main_gui.cpp gui/ewr-gui.rc)
    target_link_libraries(ewr-gui PRIVATE ewr_core comctl32)
    set_target_properties(ewr-gui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Statically link the MinGW runtime so the executables don't depend on
    # libgcc/libstdc++/libwinpthread DLLs being present on the target machine.
    target_link_options(ewr PRIVATE -static-libgcc -static-libstdc++ -static)
    target_link_options(ewr-gui PRIVATE -static-libgcc -static-libstdc++ -static)

    # MinGW's endfile spec always links its own default-manifest.o, which then
    # collides with our application manifest ("multiple non-default manifests"
    # and a duplicated resource). This specs override drops the default manifest
    # so only gui/ewr-gui.manifest is embedded (the one declaring supportedOS,
    # which stops the Program Compatibility Assistant from firing on launch).
    target_link_options(ewr-gui PRIVATE "-specs=${CMAKE_CURRENT_SOURCE_DIR}/gui/nodefaultmanifest.specs")
endif()