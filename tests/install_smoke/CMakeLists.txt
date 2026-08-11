# The smallest host project, built against an INSTALLED lhat through
# find_package -- never against this repository's targets. Driven by
# scripts/install_smoke.cmake as the install_smoke ctest entry.

cmake_minimum_required(VERSION 3.25)
project(lhat_install_smoke LANGUAGES C)

find_package(lhat CONFIG REQUIRED)

add_executable(host host.c)
target_link_libraries(host PRIVATE lhat::lhat lhat::lhatport)
