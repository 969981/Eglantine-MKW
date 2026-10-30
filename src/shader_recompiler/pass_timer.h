// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <string_view>

#include "common/common_types.h"

/// Per-pass timing of shader translation, for the offline shader benchmark.
/// Collection is off unless a thread installs a Totals: a Scope then costs one thread-local
/// load and a branch, so the scopes stay compiled in.
namespace Shader::PassTimer {

enum class Pass : u32 {
    Cfg,
    GotoPass,
    TranslatePass,
    Blocks,
    LowerFp64ToFp32,
    LowerFp16ToFp32,
    LowerInt64ToInt32,
    ConditionalBarrier,
    SsaRewrite,
    ConstantPropagation,
    Position,
    BlitConstants,
    GlobalMemoryToStorageBuffer,
    GeometryCompaction,
    Texture,
    Rescaling,
    DeadCodeElimination,
    Verification,
    CollectShaderInfo,
    Layer,
    VendorWorkaround,
    DualVertex,
    EmitContext,
    EmitCode,
    EmitFinish,
    Count,
};

struct Totals {
    std::array<u64, static_cast<size_t>(Pass::Count)> nanoseconds{};
    std::array<u64, static_cast<size_t>(Pass::Count)> calls{};
};

/// Where the calling thread accumulates its timings; null when nobody collects them.
extern thread_local Totals* thread_totals;

[[nodiscard]] std::string_view Name(Pass pass);

/// Charges the time until its destruction to one pass. Scopes must not nest.
class Scope {
public:
    explicit Scope(Pass pass_) : totals{thread_totals}, pass{pass_} {
        if (totals) {
            start = std::chrono::steady_clock::now();
        }
    }

    ~Scope() {
        if (totals) {
            const auto elapsed{std::chrono::steady_clock::now() - start};
            const size_t index{static_cast<size_t>(pass)};
            totals->nanoseconds[index] += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            ++totals->calls[index];
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Totals* totals;
    Pass pass;
    std::chrono::steady_clock::time_point start;
};

} // namespace Shader::PassTimer
