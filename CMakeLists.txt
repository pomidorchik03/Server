cmake_minimum_required(VERSION 3.14)
project(ServerProject VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    miniupnpc
    GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
    GIT_TAG miniupnpc_2_2_8
    SOURCE_SUBDIR miniupnpc
)

set(UPNPC_BUILD_TESTS OFF CACHE INTERNAL "")
set(UPNPC_BUILD_SAMPLE OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(miniupnpc)

add_library(Server SHARED 
    src/TcpServer.cpp
)

target_include_directories(Server PUBLIC header)

target_link_libraries(Server PRIVATE libminiupnpc-static)

target_include_directories(Server PRIVATE 
    ${miniupnpc_SOURCE_DIR}
    ${miniupnpc_SOURCE_DIR}/miniupnpc
)

if(WIN32)
    target_link_libraries(Server PRIVATE ws2_32 iphlpapi)
endif()

add_executable(main main.cpp)

target_link_libraries(main PRIVATE Server)

include(CTest)
enable_testing()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
