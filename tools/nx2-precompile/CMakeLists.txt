# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(nx2-precompile main.cpp)
target_link_libraries(nx2-precompile PRIVATE common shader_recompiler)
target_include_directories(nx2-precompile PRIVATE ${CMAKE_SOURCE_DIR}/src)
create_target_directory_groups(nx2-precompile)
