cmake_minimum_required(VERSION 3.14)
project(NanoSparrow VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto -Wall -Wextra -Werror")

# Зависимости не вендорятся в репозитории: FetchContent тянет пиннутые
# версии при первом конфигурировании (нужен git и интернет). Замена
# третьей стороны = смена GIT_TAG.
include(FetchContent)

# mbedTLS 2.28.9 (собирается как подпроект, статическая библиотека)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG mbedtls-2.28.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(mbedtls)

include_directories(include)

# Ищем системные библиотеки Gzip
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEFLATE REQUIRED libdeflate)
find_package(Threads REQUIRED)

# liburing 2.7: в репозитории нет CMakeLists, поэтому скачиваем исходники
# и собираем свою статическую цель без -Werror и -flto, чтобы не спорить
# с флагами чужого кода
FetchContent_Declare(liburing
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG liburing-2.7
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(liburing)
if(NOT liburing_POPULATED)
    FetchContent_Populate(liburing)
endif()
# liburing из git не содержит compat.h и io_uring_version.h — их генерирует
# configure-скрипт; запускаем его до сборки (файлы появляются в _deps)
execute_process(COMMAND sh configure
    WORKING_DIRECTORY ${liburing_SOURCE_DIR}
    OUTPUT_QUIET
    RESULT_VARIABLE liburing_configure_rc)
if(NOT liburing_configure_rc EQUAL 0)
    message(FATAL_ERROR "liburing configure failed with exit code ${liburing_configure_rc}")
endif()
add_library(uring STATIC
    ${liburing_SOURCE_DIR}/src/setup.c
    ${liburing_SOURCE_DIR}/src/queue.c
    ${liburing_SOURCE_DIR}/src/register.c
    ${liburing_SOURCE_DIR}/src/syscall.c
    ${liburing_SOURCE_DIR}/src/version.c
)
target_include_directories(uring PUBLIC
    ${liburing_SOURCE_DIR}/src/include
    ${liburing_SOURCE_DIR}/src
)
target_compile_definitions(uring PRIVATE _GNU_SOURCE)

add_executable(nano_sparrow 
    src/main.cpp 
    src/server.cpp
    src/http_parser.cpp
    src/picohttpparser.c
    src/handler.cpp
    src/example_handler.cpp
)

# Линкуем mbedTLS, libdeflate, liburing и pthread
target_link_libraries(nano_sparrow 
    ${DEFLATE_LIBRARIES}
    mbedtls 
    mbedcrypto 
    mbedx509
    uring
    Threads::Threads
)
