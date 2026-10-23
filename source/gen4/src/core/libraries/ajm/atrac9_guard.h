// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mutex>
namespace Libraries::Ajm {
// LibAtrac9 initializes shared transform/codebook tables. Its decoder objects
// are independent, but table initialization must not overlap any decode.
inline std::recursive_mutex& Atrac9LibraryMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
}
