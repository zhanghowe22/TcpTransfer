cmake_minimum_required(VERSION 3.10)
project(file_transfer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 包含头文件目录 
# 包含头文件目录 
include_directories(include)

# 服务端可执行程序 
add_executable(server
    src/main_server.cpp
    src/TCPServer.cpp
    src/protocol/TransferProtocol.cpp 
)

# Windows平台需要链接ws2_32库（socket依赖）  
if(WIN32)  
    target_link_libraries(server ws2_32)  
endif()  

add_executable(client
    src/main_client.cpp
    src/TCPClient.cpp
    src/protocol/TransferProtocol.cpp
)
if(WIN32)
    target_link_libraries(client ws2_32)
endif()

find_package(Threads REQUIRED)
if(UNIX)
    target_link_libraries(server PRIVATE Threads::Threads stdc++fs)
    target_link_libraries(client PRIVATE Threads::Threads stdc++fs)
endif()

