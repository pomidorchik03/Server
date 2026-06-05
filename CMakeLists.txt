cmake_minimum_required(VERSION 3.10.0)
project(ServerProject VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(Server SHARED 
    src/TcpServer.cpp
)

target_include_directories(Server PUBLIC header)

if(WIN32)
    target_link_libraries(Server PRIVATE ws2_32)
endif()

add_executable(main main.cpp)

target_link_libraries(main PRIVATE Server)

include(CTest)
enable_testing()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
