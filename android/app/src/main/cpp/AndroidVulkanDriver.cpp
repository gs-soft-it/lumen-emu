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
bool custom_loader = false;
bool configured = false;
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
bool ProbeTurnip(void* loader) {
    auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(loader, "vkGetInstanceProcAddr"));
    if (!get_proc) return Fail("vkGetInstanceProcAddr-missing");
    auto get_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
    uint32_t version = VK_API_VERSION_1_0;
    if (!get_version || get_version(&version) != VK_SUCCESS
            || version < VK_API_VERSION_1_3 || !create_instance)
        return Fail("custom-loader-vulkan-1.3-unavailable");
    VkApplicationInfo application {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Lumen Android Vulkan driver identity";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
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
        valid = driver.driverID == VK_DRIVER_ID_MESA_TURNIP
            && gpu.properties.apiVersion >= VK_API_VERSION_1_3;
    }
    if (destroy) destroy(instance, nullptr);
    return valid || Fail("custom-driver-identity-or-api-mismatch");
}
#endif
} // namespace

bool LumenAndroidConfigureVulkanDriver(const char* runtimeGeneration, const char* driverDirectory,
    const char* driverName, const LumenAndroidVulkanHost& host) {
    if (!runtimeGeneration || (std::strcmp(runtimeGeneration, "PS4") != 0 &&
        std::strcmp(runtimeGeneration, "PS5") != 0)) return Fail("runtime-generation");
    generation = runtimeGeneration;
    if (configured) return Fail("already-configured");
    if (!driverDirectory || !driverName) return Fail("null-options");
    if (!*driverDirectory && !*driverName) {
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
    if (!host.getInternalStoragePath || !host.getJNIEnv || !host.getActivity)
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
    if (!NativeLibraryDirectory(host, hooks)
            || !std::filesystem::is_regular_file(hooks / "libmain_hook.so", error)
            || error || !std::filesystem::is_regular_file(hooks / "libhook_impl.so", error) || error)
        return Fail("extracted-namespace-hooks-missing");
    std::string driver_path = requested.string() + "/";
    void* loader = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL,
        ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hooks.c_str(), driver_path.c_str(), name.c_str(),
        nullptr, nullptr);
    if (!loader) {
        const char* error_text = dlerror();
        std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 dlerror=%s\n",
            generation, error_text ? error_text : "unavailable");
        return Fail("adrenotools-open");
    }
    // Retain the loader and hook namespaces until the isolated process exits;
    // upstream provides no namespace teardown contract after a failed probe.
    if (!ProbeTurnip(loader)) return false;
    selected_loader = loader;
    custom_loader = true;
    configured = true;
    std::fprintf(stderr, "LUMEN_ANDROID_%s_VULKAN_DRIVER_V1 requested=custom identity=mesa-turnip "
        "loader=adrenotools path=%s fallback=denied rendererCapabilities=pending\n",
        generation, (requested / name).c_str());
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
    auto* function = dlsym(object, name);
    if (!function) Fail("loader-symbol-unavailable");
    return function;
}

void LumenAndroidVulkanUnloadObject(void* object) {
    // There is no custom namespace teardown contract. Stock uses the same
    // process lifetime, so failed loads and SDL teardown preserve live PFNs.
    if (object && object != selected_loader) Fail("loader-unload-identity");
}

const char* LumenAndroidVulkanDriverError() { return last_error; }
