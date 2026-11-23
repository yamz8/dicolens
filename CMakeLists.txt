cmake_minimum_required(VERSION 3.21)
project(dicolens LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Test)
find_package(ZLIB REQUIRED)

# The decoders read whatever a file says, so they are proven two more ways
# than the tests. -DDICOLENS_SANITIZE=ON builds everything with
# AddressSanitizer and UndefinedBehaviorSanitizer, to run the tests under
# (bin/sanitize); -DDICOLENS_FUZZ=ON builds the fuzz targets in fuzz/ as well
# (bin/fuzz), which takes clang. Either goes in a build folder of its own.
option(DICOLENS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DICOLENS_FUZZ "Build the fuzz targets in fuzz/ (clang only)" OFF)
if(DICOLENS_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The fuzz targets need libFuzzer: configure with -DCMAKE_CXX_COMPILER=clang++")
  endif()
  set(DICOLENS_SANITIZE ON)
  # Coverage for libFuzzer to steer by, in the library as well as the targets.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()
if(DICOLENS_SANITIZE)
  # Undefined behaviour stops the program like a bad read does, so a test
  # or a fuzz run can't pass over it.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g1)
  add_link_options(-fsanitize=address,undefined)
endif()

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB dicolens_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM dicolens_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
# The standard's tables are part of the library, so that everything built on
# it, the tests, the fuzzers, and the tools as much as the app, reads an
# implicit file by the same dictionary.
add_library(dicolens_core STATIC ${dicolens_sources} src/data.qrc)
target_include_directories(dicolens_core PUBLIC src)
target_link_libraries(dicolens_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus ZLIB::ZLIB)
target_compile_options(dicolens_core PUBLIC -Wall -Wextra)
# The views and the backend are the Dicolens QML module, declared with
# QML_ELEMENT, so the build writes a description of their types that qmllint
# reads (through .qmllint.ini) to check the QML that uses them.
qt_policy(SET QTP0001 NEW)
qt_add_qml_module(dicolens_core URI Dicolens VERSION 1.0 NO_PLUGIN
  OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Dicolens)

add_executable(dicolens src/main.cpp src/resources.qrc)
target_link_libraries(dicolens PRIVATE dicolens_core)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(dicolens_tests tests/dicolens_tests.cpp src/resources.qrc)
target_link_libraries(dicolens_tests PRIVATE dicolens_core Qt6::Test)
add_test(NAME dicolens_tests COMMAND dicolens_tests)

# What damaged and hostile files do, one test for each bug fuzzing found.
add_executable(hostile_tests tests/hostile_tests.cpp)
target_link_libraries(hostile_tests PRIVATE dicolens_core Qt6::Test)
add_test(NAME hostile_tests COMMAND hostile_tests)

# Pictures that decoded wrongly, one test for each that bin/conformance found.
add_executable(decode_tests tests/decode_tests.cpp)
target_link_libraries(decode_tests PRIVATE dicolens_core Qt6::Test)
add_test(NAME decode_tests COMMAND decode_tests)

# A CT taken with its gantry tilted: a shape of known size, scanned straight
# and leaning, has to come out of every cut, measurement, and drawing true.
add_executable(geometry_tests tests/geometry_tests.cpp)
target_link_libraries(geometry_tests PRIVATE dicolens_core Qt6::Test)
add_test(NAME geometry_tests COMMAND geometry_tests)

set(test_environment QT_QPA_PLATFORM=offscreen)
if(DICOLENS_SANITIZE)
  list(APPEND test_environment
    LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/sanitizers/lsan.supp:print_suppressions=0)
endif()
set_tests_properties(dicolens_tests hostile_tests decode_tests geometry_tests PROPERTIES ENVIRONMENT "${test_environment}")

# The dictionary, the anonymizer, what the panes share, and the anonymous
# copy as the window offers it have test files of their own, with the tables
# from the standard and the window's QML, which are resources too.
foreach(suite dictionary_tests anonymize_tests library_tests anonymous_tests)
  add_executable(${suite} tests/${suite}.cpp src/resources.qrc)
  target_link_libraries(${suite} PRIVATE dicolens_core Qt6::Test)
  add_test(NAME ${suite} COMMAND ${suite})
  set_tests_properties(${suite} PROPERTIES ENVIRONMENT "${test_environment}")
endforeach()

# bin/demos records the GIFs in docs/features.md with this; a plain build leaves it out.
add_executable(dicolens_demo EXCLUDE_FROM_ALL tools/demo.cpp src/resources.qrc)
target_link_libraries(dicolens_demo PRIVATE dicolens_core Qt6::Test)

# bin/conformance compares what this prints with a reference decoder's.
add_executable(dicolens_pixeldump EXCLUDE_FROM_ALL tools/pixeldump.cpp)
target_link_libraries(dicolens_pixeldump PRIVATE dicolens_core)

if(DICOLENS_FUZZ)
  add_subdirectory(fuzz)
endif()

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS dicolens RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/dicolens.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/dicolens.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dicolens)
