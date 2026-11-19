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

add_executable(dicolens src/main.cpp src/resources.qrc)
target_link_libraries(dicolens PRIVATE dicolens_core)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(dicolens_tests tests/dicolens_tests.cpp src/resources.qrc)
target_link_libraries(dicolens_tests PRIVATE dicolens_core Qt6::Test)
add_test(NAME dicolens_tests COMMAND dicolens_tests)
set_tests_properties(dicolens_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS dicolens RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/dicolens.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/dicolens.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dicolens)
