// SPDX-License-Identifier: GPL-2.0-only
#pragma once

namespace Lumen::Ps5::DirectFex {
// The direct build has one translator; an unavailable selection is rejected.
bool LockSelection() noexcept;
}
