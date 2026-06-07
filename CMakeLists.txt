cmake_minimum_required(VERSION 3.16)
project(apiwidget LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Widgets — UI, Network — HTTP к приватным API, Sql — чтение state.vscdb Cursor (QSQLITE)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Sql)

add_executable(apiwidget main.cpp MainWindow.cpp Fetcher.cpp desktop_integration.cpp resources.qrc)
target_link_libraries(apiwidget PRIVATE Qt6::Widgets Qt6::Network Qt6::Sql)

# --- регрессионные тесты чистой логики (без сети/creds): ctest --test-dir build ---
find_package(Qt6 QUIET COMPONENTS Test)
if(Qt6Test_FOUND)
    enable_testing()
    add_executable(test_usage_logic tests/test_usage_logic.cpp)
    target_link_libraries(test_usage_logic PRIVATE Qt6::Test)
    add_test(NAME usage_logic COMMAND test_usage_logic)
endif()
