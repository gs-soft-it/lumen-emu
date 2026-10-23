// SPDX-License-Identifier: GPL-2.0-only
#include "AndroidVulkanDriver.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const LumenAndroidVulkanHost host {nullptr, nullptr, nullptr};
    bool pass = false;
    if (std::strcmp(argv[1], "unconfigured") == 0) {
        pass = LumenAndroidVulkanLoadObject("libvulkan.so") == nullptr &&
            std::strcmp(LumenAndroidVulkanDriverError(), "loader-not-configured") == 0;
    } else if (std::strcmp(argv[1], "custom-denial") == 0) {
        pass = !LumenAndroidConfigureVulkanDriver("PS4", "relative", "libvulkan.so", host) &&
            LumenAndroidVulkanLoadObject("libvulkan.so") == nullptr;
    } else if (std::strcmp(argv[1], "invalid-options") == 0) {
        pass = !LumenAndroidConfigureVulkanDriver("PS4", nullptr, "", host) &&
            !LumenAndroidConfigureVulkanDriver("PS6", "", "", host) &&
            LumenAndroidVulkanLoadObject("libvulkan.so") == nullptr;
    } else if (std::strcmp(argv[1], "stock") == 0) {
        pass = LumenAndroidConfigureVulkanDriver("PS4", "", "", host) &&
            !LumenAndroidConfigureVulkanDriver("PS4", "", "", host) &&
            LumenAndroidVulkanLoadObject("unqualified.so") == nullptr;
        void* object = pass ? LumenAndroidVulkanLoadObject("libvulkan.so") : nullptr;
        void* gipa = object ? LumenAndroidVulkanLoadFunction(object, "vkGetInstanceProcAddr") : nullptr;
        pass = pass && object && gipa &&
            LumenAndroidVulkanLoadFunction(nullptr, "vkGetInstanceProcAddr") == nullptr &&
            LumenAndroidVulkanLoadFunction(object, "lumen_test_missing_vulkan_symbol") == nullptr;
        LumenAndroidVulkanUnloadObject(object);
        pass = pass && LumenAndroidVulkanLoadObject("libvulkan.so") == object &&
            LumenAndroidVulkanLoadFunction(object, "vkGetInstanceProcAddr") == gipa;
    } else {
        return 2;
    }
    std::printf("%s vulkan_driver_host case=%s renderer_execution=0\n", pass ? "PASS" : "FAIL", argv[1]);
    return pass ? 0 : 1;
}
