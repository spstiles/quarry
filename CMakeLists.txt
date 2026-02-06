cmake_minimum_required(VERSION 3.20)

project(quarry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

add_executable(quarry
  src/main.cpp
  src/MainFrame.cpp
  src/FilePanel.cpp
  src/NavIcons.cpp
  src/util.cpp
)

target_include_directories(quarry PRIVATE src)
target_link_libraries(quarry PRIVATE ${wxWidgets_LIBRARIES})
