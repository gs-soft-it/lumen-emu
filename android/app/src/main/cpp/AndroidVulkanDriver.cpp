// Lumen Android host integration, 2026-10-02/11. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidVulkanDriver.h"
#include <android/log.h>
#include <dlfcn.h>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <jni.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <vulkan/vulkan.h>
#ifdef LUMEN_ANDROID_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif

namespace {
void* selected_loader = nullptr;
PFN_vkGetInstanceProcAddr selected_get_proc = nullptr;
bool custom_loader = false;
bool configured = false;
bool configuration_attempted = false;
const char* generation = "PS4";
const char* last_error = "not-configured";

bool Fail(const char* reason) {
    last_error = reason;
    __android_log_print(ANDROID_LOG_ERROR, "LumenAndroidVulkan",
        "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 FAIL reason=%s fallback=denied", generation, reason);
    std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 FAIL reason=%s fallback=denied\n", generation, reason);
    return false;
}

#ifdef LUMEN_ANDROID_ADRENOTOOLS
bool CanonicalDirectory(const char* text, std::filesystem::path& result) {
    if (!text || text[0] != '/') return false;
    std::error_code error;
    result = std::filesystem::canonical(text, error);
    return !error && result == std::filesystem::path(text)
        && std::filesystem::is_directory(result, error) && !error;
}

bool PrivateFile(const std::filesystem::path& path) {
    struct stat info {};
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)
        && info.st_uid == geteuid() && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0
        && info.st_size > 0;
}

bool NativeLibraryDirectory(const LumenAndroidVulkanHost& host, std::filesystem::path& directory) {
    auto* env = static_cast<JNIEnv*>(host.getJNIEnv());
    if (!env) return false;
    auto activity = static_cast<jobject>(host.getActivity());
    if (!activity) return false;
    jclass activity_class = env->GetObjectClass(activity);
    jmethodID get_info = activity_class ? env->GetMethodID(activity_class,
        "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;") : nullptr;
    jobject info = get_info ? env->CallObjectMethod(activity, get_info) : nullptr;
    jclass info_class = info ? env->GetObjectClass(info) : nullptr;
    jfieldID native_dir = info_class ? env->GetFieldID(info_class,
        "nativeLibraryDir", "Ljava/lang/String;") : nullptr;
    auto text = native_dir ? static_cast<jstring>(env->GetObjectField(info, native_dir)) : nullptr;
    bool valid = false;
    if (!env->ExceptionCheck() && text) {
        const char* utf = env->GetStringUTFChars(text, nullptr);
        if (utf) {
            // Android's trusted ApplicationInfo path can contain a platform
            // symlink. Use its canonical form for the actual hook directory.
            std::error_code error;
            directory = std::filesystem::canonical(utf, error);
            valid = !error && std::filesystem::is_directory(directory, error) && !error;
            env->ReleaseStringUTFChars(text, utf);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (text) env->DeleteLocalRef(text);
    if (info_class) env->DeleteLocalRef(info_class);
    if (info) env->DeleteLocalRef(info);
    if (activity_class) env->DeleteLocalRef(activity_class);
    env->DeleteLocalRef(activity);
    return valid;
}

// Upstream can silently fall back after loading its hook. Qualify the handle
// that SDL will actually use; a successful dlopen alone cannot admit custom.
bool ProbeCustomDriver(PFN_vkGetInstanceProcAddr get_proc, VkDriverId expected_driver,
                       bool require_android_surface) {
    if (!get_proc) return Fail("vkGetInstanceProcAddr-missing");
    auto get_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
    uint32_t version = VK_API_VERSION_1_0;
    if (!get_version || get_version(&version) != VK_SUCCESS
            || version < VK_API_VERSION_1_3 || !create_instance)
        return Fail("custom-loader-vulkan-1.3-unavailable");
    const char* surface_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                       "VK_KHR_android_surface"};
    if (require_android_surface) {
        auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        uint32_t count = 0;
        if (!enumerate_extensions || enumerate_extensions(nullptr, &count, nullptr) != VK_SUCCESS
                || count == 0 || count > 1024) return Fail("direct-icd-instance-extensions");
        std::vector<VkExtensionProperties> extensions(count);
        if (enumerate_extensions(nullptr, &count, extensions.data()) != VK_SUCCESS
                || count > extensions.size()) return Fail("direct-icd-instance-extensions");
        for (const char* required : surface_extensions) {
            bool found = false;
            for (uint32_t index = 0; index < count; ++index)
                found |= std::strcmp(extensions[index].extensionName, required) == 0;
            if (!found) return Fail("direct-icd-android-surface-unavailable");
        }
    }
    VkApplicationInfo application {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Lumen Android Vulkan driver identity";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    if (require_android_surface) {
        info.enabledExtensionCount = 2;
        info.ppEnabledExtensionNames = surface_extensions;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = create_instance(&info, nullptr, &instance);
    if (result != VK_SUCCESS || !instance) {
        std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 createInstance=%d\n", generation, result);
        return Fail("custom-probe-instance");
    }
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get_proc(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        get_proc(instance, "vkGetPhysicalDeviceProperties2"));
    uint32_t count = 0;
    bool valid = destroy && enumerate && properties
        && (!require_android_surface || get_proc(instance, "vkCreateAndroidSurfaceKHR"))
        && enumerate(instance, &count, nullptr) == VK_SUCCESS && count > 0 && count <= 16;
    std::vector<VkPhysicalDevice> devices(count <= 16 ? count : 0);
    if (valid) valid = enumerate(instance, &count, devices.data()) == VK_SUCCESS && count > 0;
    for (uint32_t index = 0; valid && index < count; ++index) {
        VkPhysicalDeviceDriverProperties driver {};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 gpu {};
        gpu.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        gpu.pNext = &driver;
        properties(devices[index], &gpu);
        std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 gpu=%s api=%u.%u.%u "
            "driverID=%u driverName=%s driverInfo=%s\n", generation, gpu.properties.deviceName,
            VK_VERSION_MAJOR(gpu.properties.apiVersion), VK_VERSION_MINOR(gpu.properties.apiVersion),
            VK_VERSION_PATCH(gpu.properties.apiVersion), driver.driverID, driver.driverName,
            driver.driverInfo);
        valid = driver.driverID == expected_driver
            && gpu.properties.apiVersion >= VK_API_VERSION_1_3;
        __android_log_print(valid ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "LumenAndroidVulkan",
            "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 gpu=%s api=%u.%u.%u driverID=%u identityAdmitted=%d",
            generation, gpu.properties.deviceName, VK_VERSION_MAJOR(gpu.properties.apiVersion),
            VK_VERSION_MINOR(gpu.properties.apiVersion), VK_VERSION_PATCH(gpu.properties.apiVersion),
            driver.driverID, valid ? 1 : 0);
    }
    if (destroy) destroy(instance, nullptr);
    return valid || Fail("custom-driver-identity-or-api-mismatch");
}

PFN_vkGetInstanceProcAddr DirectDriverEntry(void* library, const std::filesystem::path& path,
                                          const struct stat& requested) {
    auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(library, "vk_icdGetInstanceProcAddr"));
    Dl_info origin {};
    struct stat resolved {}, current {};
    if (!get_proc || !dladdr(reinterpret_cast<void*>(get_proc), &origin) || !origin.dli_fname
            || stat(origin.dli_fname, &resolved) != 0 || lstat(path.c_str(), &current) != 0
            || requested.st_dev != resolved.st_dev || requested.st_ino != resolved.st_ino
            || requested.st_dev != current.st_dev || requested.st_ino != current.st_ino
            || requested.st_size != current.st_size) {
        Fail("direct-icd-entrypoint-ownership");
        return nullptr;
    }
    using Negotiate = VkResult (*)(uint32_t*);
    auto negotiate = reinterpret_cast<Negotiate>(
        dlsym(library, "vk_icdNegotiateLoaderICDInterfaceVersion"));
    if (!negotiate) {
        Fail("direct-icd-interface-negotiation-missing");
        return nullptr;
    }
    uint32_t version = 7;
    if (negotiate(&version) != VK_SUCCESS || version < 2 || version > 7) {
        Fail("direct-icd-interface-negotiation");
        return nullptr;
    }
    std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 loader=direct-icd "
        "experimental=1 icdInterface=%u entrypointObject=%s\n", generation, version, origin.dli_fname);
    return get_proc;
}
#endif
} // namespace

bool LumenAndroidConfigureVulkanDriver(const char* runtimeGeneration, const char* driverDirectory,
    const char* driverName, const LumenAndroidVulkanHost& host, const char* loaderMode) {
    if (configuration_attempted) return Fail("already-configured");
    configuration_attempted = true;
    if (!runtimeGeneration || (std::strcmp(runtimeGeneration, "PS4") != 0 &&
        std::strcmp(runtimeGeneration, "PS5") != 0)) return Fail("runtime-generation");
    generation = runtimeGeneration;
    if (!driverDirectory || !driverName || !loaderMode) return Fail("null-options");
    const bool stock = !*driverDirectory && !*driverName;
    const bool legacy = !*loaderMode;
    const bool direct = std::strcmp(loaderMode, "direct-icd") == 0;
    if (!(legacy || direct || std::strcmp(loaderMode, "system") == 0
            || std::strcmp(loaderMode, "adrenotools") == 0)) return Fail("loader-mode");
    if ((!legacy && stock != (std::strcmp(loaderMode, "system") == 0))
            || (!stock && (!*driverDirectory || !*driverName))) return Fail("loader-options");
    if (stock) {
        configured = true;
        std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 requested=system\n", generation);
        return true;
    }
#ifndef LUMEN_ANDROID_ADRENOTOOLS
    (void)host;
    return Fail("custom-loader-not-built");
#else
    const std::string name(driverName);
    if (name.empty() || name.size() > 128 || name.find('/') != std::string::npos
            || name.find('\\') != std::string::npos || name == "." || name == ".."
            || name.size() < 3 || name.compare(name.size() - 3, 3, ".so") != 0) return Fail("driver-basename");
    std::filesystem::path requested, internal, hooks;
    if (!CanonicalDirectory(driverDirectory, requested)) return Fail("driver-directory");
    if (!host.getInternalStoragePath || (!direct && (!host.getJNIEnv || !host.getActivity)))
        return Fail("host-functions");
    const char* storage = host.getInternalStoragePath();
    std::error_code error;
    if (!storage) return Fail("internal-storage");
    internal = std::filesystem::canonical(storage, error);
    auto relative = requested.lexically_relative(internal);
    if (error || relative.empty() || relative == "." || relative.is_absolute()
            || *relative.begin() == ".." || !PrivateFile(requested / name))
        return Fail("driver-not-app-private");
    struct stat directory_info {};
    if (stat(requested.c_str(), &directory_info) != 0 || directory_info.st_uid != geteuid()
            || (directory_info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return Fail("driver-directory-ownership");
    void* loader = nullptr;
    PFN_vkGetInstanceProcAddr get_proc = nullptr;
    if (direct) {
        const auto driver = requested / name;
        struct stat identity {};
        if (lstat(driver.c_str(), &identity) != 0) return Fail("direct-icd-file-identity");
        loader = dlopen(driver.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (loader) get_proc = DirectDriverEntry(loader, driver, identity);
    } else {
        if (!NativeLibraryDirectory(host, hooks)
                || !std::filesystem::is_regular_file(hooks / "libmain_hook.so", error)
                || error || !std::filesystem::is_regular_file(hooks / "libhook_impl.so", error) || error)
            return Fail("extracted-namespace-hooks-missing");
        std::string driver_path = requested.string() + "/";
        loader = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL,
            ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hooks.c_str(), driver_path.c_str(), name.c_str(),
            nullptr, nullptr);
        if (loader) get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(loader, "vkGetInstanceProcAddr"));
    }
    if (!loader) {
        const char* error_text = dlerror();
        std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 dlerror=%s\n",
            generation, error_text ? error_text : "unavailable");
        return Fail(direct ? "direct-icd-open" : "adrenotools-open");
    }
    // Retain the loader and hook namespaces until the isolated process exits;
    // upstream provides no namespace teardown contract after a failed probe.
    // The pinned Mali driver shares the app-private loader, but must never be
    // mistaken for a successful load of the stock ARM driver or Turnip.
    const bool panvk = direct || name == "libvulkan_panfrost.so";
    if (direct && !get_proc) return false;
    if (!ProbeCustomDriver(get_proc, panvk ? VK_DRIVER_ID_MESA_PANVK
                                         : VK_DRIVER_ID_MESA_TURNIP, direct)) return false;
    selected_loader = loader;
    selected_get_proc = get_proc;
    custom_loader = true;
    configured = true;
    std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 requested=custom identity=%s "
        "loader=%s experimental=%d path=%s fallback=denied rendererCapabilities=pending\n",
        generation, panvk ? "mesa-panvk" : "mesa-turnip", direct ? "direct-icd" : "adrenotools",
        direct ? 1 : 0, (requested / name).c_str());
    __android_log_print(ANDROID_LOG_INFO, "LumenAndroidVulkan",
        "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 requested=custom identity=%s loader=%s "
        "experimental=%d fallback=denied rendererCapabilities=pending", generation,
        panvk ? "mesa-panvk" : "mesa-turnip", direct ? "direct-icd" : "adrenotools", direct ? 1 : 0);
    return true;
#endif
}

// A single handle backs SDL surfaces and the renderer's GIPA. Retain both
// stock and custom loaders until process exit: surface recreation must never
// invalidate entrypoints held by an existing instance or device dispatch.
void* LumenAndroidVulkanLoadObject(const char* path) {
    if (!configured) { Fail("loader-not-configured"); return nullptr; }
    if (path && std::strcmp(path, "libvulkan.so") != 0) {
        Fail(custom_loader ? "custom-loader-override" : "system-loader-override");
        return nullptr;
    }
    if (!selected_loader) {
        selected_loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!selected_loader) { Fail("system-loader-open"); return nullptr; }
    }
    return selected_loader;
}

void* LumenAndroidVulkanLoadFunction(void* object, const char* name) {
    if (!configured || !object || object != selected_loader || !name) {
        Fail("loader-symbol-arguments");
        return nullptr;
    }
    auto* function = selected_get_proc && std::strcmp(name, "vkGetInstanceProcAddr") == 0
        ? reinterpret_cast<void*>(selected_get_proc) : dlsym(object, name);
    if (!function) Fail("loader-symbol-unavailable");
    return function;
}

void LumenAndroidVulkanUnloadObject(void* object) {
    // There is no custom namespace teardown contract. Stock uses the same
    // process lifetime, so failed loads and SDL teardown preserve live PFNs.
    if (object && object != selected_loader) Fail("loader-unload-identity");
}

const char* LumenAndroidVulkanDriverError() { return last_error; }
