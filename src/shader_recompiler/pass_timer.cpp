// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/pass_timer.h"

namespace Shader::PassTimer {

thread_local Totals* thread_totals{};

std::string_view Name(Pass pass) {
    static constexpr std::array<std::string_view, static_cast<size_t>(Pass::Count)> names{
        "CFG",
        "GotoPass",
        "TranslatePass",
        "Blocks",
        "LowerFp64ToFp32",
        "LowerFp16ToFp32",
        "LowerInt64ToInt32",
        "ConditionalBarrier",
        "SsaRewrite",
        "ConstantPropagation",
        "Position",
        "BlitConstants",
        "GlobalMemoryToStorageBuffer",
        "GeometryCompaction",
        "Texture",
        "Rescaling",
        "DeadCodeElimination",
        "Verification",
        "CollectShaderInfo",
        "Layer",
        "VendorWorkaround",
        "DualVertex",
        "EmitContext",
        "EmitCode",
        "EmitFinish",
    };
    const size_t index{static_cast<size_t>(pass)};
    return index < names.size() ? names[index] : "?";
}

} // namespace Shader::PassTimer
