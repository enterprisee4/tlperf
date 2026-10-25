cmake_minimum_required(VERSION 3.10)
project(tlperf CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -O2)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- tlperf sampling library (deliverable: libtlperf.a + tlperf.h) ----
add_library(tlperf STATIC src/tlperf.cpp)
target_include_directories(tlperf PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
find_package(Threads REQUIRED)
target_link_libraries(tlperf PUBLIC Threads::Threads)

# ---- demo application: generates tlperf.data with synthetic per-thread stacks ----
add_executable(tlperf-demo demo/tlperf-demo.cpp)
target_link_libraries(tlperf-demo PRIVATE tlperf)

# ---- analyzer backend: HTTP/JSON server over tlperf.data (cpp-httplib) ----
add_executable(tlperf-server src/tlperf-server.cpp)
target_link_libraries(tlperf-server PRIVATE tlperf)
# cpp-httplib (Ubuntu 0.10.3 is compiled into a shared lib; vendored in third_party)
set(HTTPLIB_SO ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libcpp-httplib.so.0)
target_link_libraries(tlperf-server PRIVATE ${HTTPLIB_SO})
set_target_properties(tlperf-server PROPERTIES
    BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    INSTALL_RPATH "$ORIGIN/../lib")

install(TARGETS tlperf ARCHIVE DESTINATION lib)
install(FILES tlperf.h DESTINATION include)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib.h DESTINATION include)
install(FILES ${HTTPLIB_SO} DESTINATION lib)
