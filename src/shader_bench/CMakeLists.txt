# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Offline shader translation benchmark (see NX2_SHADER_STUTTER.md). Not part of the default build:
# cmake --build <build dir> --target eden-shader-bench
add_executable(eden-shader-bench EXCLUDE_FROM_ALL
    shader_bench.cpp
    vma.cpp
)

target_link_libraries(eden-shader-bench PRIVATE common core frontend_common input_common video_core
                      shader_recompiler)
target_link_libraries(eden-shader-bench PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)

create_target_directory_groups(eden-shader-bench)
