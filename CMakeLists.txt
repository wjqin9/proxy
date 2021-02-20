cmake_minimum_required (VERSION 2.8.11)
project (httpproxy)
add_executable (httpproxy httpproxy.cpp)
target_link_libraries(httpproxy stdc++ pthread boost_thread boost_system)
