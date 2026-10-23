// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// Empty driverDirectory selects the process-local system loader. A custom request is
// process-local, app-private, and never falls back to the system driver.
bool LumenAndroidPs5ConfigureVulkanDriver(const char* driverDirectory,
                                        const char* driverName);
