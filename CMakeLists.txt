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

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB dicolens_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM dicolens_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(dicolens_core STATIC ${dicolens_sources})
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
set_tests_properties(dicolens_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# The dictionary has a test file of its own, with the tables from the
# standard, which are resources too.
foreach(suite dictionary_tests)
  add_executable(${suite} tests/${suite}.cpp src/resources.qrc)
  target_link_libraries(${suite} PRIVATE dicolens_core Qt6::Test)
  add_test(NAME ${suite} COMMAND ${suite})
  set_tests_properties(${suite} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# bin/demos records the GIFs in docs/features.md with this; a plain build leaves it out.
add_executable(dicolens_demo EXCLUDE_FROM_ALL tools/demo.cpp src/resources.qrc)
target_link_libraries(dicolens_demo PRIVATE dicolens_core Qt6::Test)

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS dicolens RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/dicolens.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/dicolens.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dicolens)
