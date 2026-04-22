cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(UltraHackPack VERSION 1.0.0)

# Подключаем Geode SDK
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# ========================================
# Собираем ВСЕ .cpp файлы рекурсивно
# включая src/hooks/, src/utils/ и т.д.
# ========================================
file(GLOB_RECURSE SOURCES
    src/*.cpp
    src/**/*.cpp
)

# Создаём библиотеку со всеми исходниками
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# Добавляем src/ как директорию для #include
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Настраиваем как Geode мод
setup_geode_mod(${PROJECT_NAME})
