cmake_minimum_required(VERSION 3.20)

project(quarry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(GIO gio-2.0 glib-2.0)
endif()

add_executable(quarry
  src/main.cpp
  src/MainFrame.cpp
  src/FilePanel.cpp
  src/Connections.cpp
  src/QuadSplitter.cpp
  src/NavIcons.cpp
  src/util.cpp
)

target_include_directories(quarry PRIVATE src)
target_link_libraries(quarry PRIVATE ${wxWidgets_LIBRARIES})
target_compile_definitions(quarry PRIVATE QUARRY_VERSION=\"1.1.0\")

if(GIO_FOUND)
  target_compile_definitions(quarry PRIVATE QUARRY_USE_GIO=1)
  target_include_directories(quarry PRIVATE ${GIO_INCLUDE_DIRS})
  target_link_libraries(quarry PRIVATE ${GIO_LIBRARIES})
  target_compile_options(quarry PRIVATE ${GIO_CFLAGS_OTHER})
endif()
