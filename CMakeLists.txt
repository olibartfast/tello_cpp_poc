cmake_minimum_required(VERSION 3.15)
project(tello_rabbitmq_poc CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


list(APPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}/_deps/*.cmake")
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/_deps/*.cmake")

find_package(amqpcpp REQUIRED)
find_package(libuv)
find_package(ZLIB)  # CMake's built-in FindZLIB module
find_package(OpenSSL)  # CMake's built-in FindOpenSSL module

# Include directories
include_directories(include)

# Executables
add_executable(flight_controller src/flight_controller.cpp)
target_link_libraries(flight_controller PRIVATE amqpcpp uv OpenSSL::SSL OpenSSL::Crypto)

add_executable(tello_controller src/tello_controller.cpp src/tello.cpp)
target_link_libraries(tello_controller PRIVATE amqpcpp uv ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto)

# Install
install(TARGETS flight_controller tello_controller DESTINATION bin)