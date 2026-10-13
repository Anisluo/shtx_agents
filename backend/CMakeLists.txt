cmake_minimum_required(VERSION 3.16)
project(shtx_backend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(shtx_backend
  src/main.cpp
  src/db.cpp
  src/auth.cpp
  src/glm_client.cpp
  src/product_repo.cpp
  src/agent.cpp)

target_include_directories(shtx_backend PRIVATE third_party src)
target_link_libraries(shtx_backend PRIVATE OpenSSL::SSL OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)

if(WIN32)
  target_compile_definitions(shtx_backend PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(shtx_backend PRIVATE ws2_32 crypt32 cryptui)
  if(MINGW)
    # 静态链接 libstdc++/libgcc，减少运行时 DLL 依赖
    target_link_options(shtx_backend PRIVATE -static-libgcc -static-libstdc++)
    target_compile_options(shtx_backend PRIVATE -Wa,-mbig-obj)
  endif()
endif()
