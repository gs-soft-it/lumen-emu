// SPDX-License-Identifier: GPL-2.0-only
#include "host/DocumentFileSystem.h"
#include "host/DocumentOperationCache.h"
#include <jni.h>
#include <mutex>
#include <optional>
#include <sys/mman.h>
#include <unordered_set>

namespace {
std::mutex mutex;
JavaVM* vm{};
jobject library{};
jmethodID open_method{}, stat_method{}, list_method{}, list_entries_method{};
jmethodID begin_operation_method{}, end_operation_method{}, abort_operation_method{};
std::string root;
std::uint64_t mount_generation{};

auto& ReadCache() { return Lumen::Documents::Detail::CurrentReadOperation(); }

bool Check(JNIEnv* env) {
    if (!env->ExceptionCheck()) return true;
    env->ExceptionClear();
    errno = EIO;
    return false;
}

// The local JNI reference pins the exact owner/class behind these method IDs,
// even when another thread replaces the mount's global reference.
struct MountSnapshot {
    jobject owner{};
    std::string root;
    std::uint64_t generation{};
    jmethodID open{}, stat{}, list{}, list_entries{}, begin{}, end{}, abort{};
};

struct ReadOperationContext {
    std::size_t depth{};
    JNIEnv* env{};
    jobject owner{};
    jmethodID owner_end_method{}, owner_abort_method{};
    std::uint64_t generation{};
    bool active{}, disabled{};

    void DropOwner() {
        if (!owner) return;
        if (active) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->CallVoidMethod(owner, owner_end_method);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteGlobalRef(owner);
        owner = nullptr;
        owner_end_method = owner_abort_method = nullptr;
        active = false;
    }

    bool Prepare(JNIEnv* current_env, const MountSnapshot& mount) {
        if (owner && generation != mount.generation) {
            DropOwner();
            disabled = true;
            ReadCache().Fail();
        }
        if (owner || disabled) return true;
        env = current_env;
        owner = env->NewGlobalRef(mount.owner);
        if (!Check(env) || !owner) {
            if (owner) env->DeleteGlobalRef(owner);
            owner = nullptr;
            disabled = true;
            return false;
        }
        generation = mount.generation;
        owner_end_method = mount.end;
        owner_abort_method = mount.abort;
        env->CallVoidMethod(owner, mount.begin);
        if (!Check(env)) {
            env->DeleteGlobalRef(owner);
            owner = nullptr;
            disabled = true;
            return false;
        }
        active = true;
        return true;
    }

    void Abort() {
        disabled = true;
        if (env && owner && active) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->CallVoidMethod(owner, owner_abort_method);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    void Finish() {
        DropOwner();
        env = nullptr;
        disabled = false;
    }
};

// Retain only attachments created here. Java-owned callers are borrowed and
// queried again on each access; their attachment is never detached by us.
// Java ThreadLocal metadata still ends with each syscall's outermost scope.
struct ThreadState {
    JavaVM* owned_vm{};
    JNIEnv* owned_env{};
    ReadOperationContext operation;

    JNIEnv* Acquire(JavaVM* requested_vm) {
        if (!requested_vm) return nullptr;
        if (owned_vm == requested_vm) return owned_env;
        JNIEnv* env{};
        const int status = requested_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (status == JNI_OK) return env;
        if (status != JNI_EDETACHED || owned_vm) return nullptr;
        if (requested_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
        owned_vm = requested_vm;
        owned_env = env;
        return env;
    }

    ~ThreadState() {
        // The separate metadata cache may already be destroyed during TLS
        // teardown. Clean up Java references without touching that cache.
        operation.Finish();
        if (owned_vm) owned_vm->DetachCurrentThread();
    }
};
thread_local ThreadState thread_state;

void AbortReadOperation() {
    const int saved_errno = errno;
    ReadCache().Fail();
    if (thread_state.operation.depth) thread_state.operation.Abort();
    errno = saved_errno;
}

struct AccessResult {
    bool success{};
    ~AccessResult() { if (!success) AbortReadOperation(); }
};

struct Environment {
    JNIEnv* env{};
    MountSnapshot mount;
    bool ready{};

    Environment() {
        JavaVM* current_vm{};
        {
            std::lock_guard lock(mutex);
            current_vm = vm;
        }
        env = thread_state.Acquire(current_vm);
        if (!env) return;
        {
            std::lock_guard lock(mutex);
            if (!library || vm != current_vm) return;
            mount.owner = env->NewLocalRef(library);
            mount.root = root;
            mount.generation = mount_generation;
            mount.open = open_method;
            mount.stat = stat_method;
            mount.list = list_method;
            mount.list_entries = list_entries_method;
            mount.begin = begin_operation_method;
            mount.end = end_operation_method;
            mount.abort = abort_operation_method;
        }
        if (!Check(env) || !mount.owner) return;
        ready = !thread_state.operation.depth || thread_state.operation.Prepare(env, mount);
    }
    ~Environment() { if (mount.owner) env->DeleteLocalRef(mount.owner); }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    explicit operator bool() const { return ready; }

    // Linearize publication against mount replacement. No Java/provider work
    // runs under this lock, including scope begin/end/abort callbacks.
    template<class Publish>
    bool PublishCurrent(Publish&& publish) const {
        std::lock_guard lock(mutex);
        if (!library || mount_generation != mount.generation) { errno = ESTALE; return false; }
        publish();
        return true;
    }
    bool IsCurrent() const { return PublishCurrent([] {}); }
};

bool Owns(const std::filesystem::path& path, const std::string& selected_root) {
    const auto name = path.string();
    return !selected_root.empty() && (name == selected_root || name.starts_with(selected_root + "/"));
}
std::optional<std::string> RelativePath(const std::filesystem::path& path, const std::string& selected_root) {
    if (!Owns(path, selected_root)) { errno = EINVAL; return std::nullopt; }
    const auto raw = path.string().substr(selected_root.size());
    const auto relative = raw.empty() ? std::string{} : raw.substr(1);
    std::string clean;
    for (const auto& component : std::filesystem::path(relative)) {
        if (component == "..") { errno = EACCES; return std::nullopt; }
        if (component.empty() || component == ".") continue;
        if (!clean.empty()) clean += '/';
        clean += component.string();
    }
    return clean;
}
jstring Relative(const Environment& access, const std::filesystem::path& path) {
    const auto relative = RelativePath(path, access.mount.root);
    return relative ? access.env->NewStringUTF(relative->c_str()) : nullptr;
}
}

namespace DocumentBridge {
bool IsMounted(const std::filesystem::path& path) {
    std::lock_guard lock(mutex);
    return Owns(path, root);
}
int OpenRead(const std::filesystem::path& path) {
    AccessResult outcome;
    Environment access;
    if (!access) { errno = EACCES; return -1; }
    auto name = Relative(access, path);
    if (!name) { Check(access.env); return -1; }
    int fd = access.env->CallIntMethod(access.mount.owner, access.mount.open, name);
    access.env->DeleteLocalRef(name);
    if (!Check(access.env) || fd < 0) { if (fd >= 0) ::close(fd); errno = EIO; return -1; }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || ::lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd); errno = ESPIPE; return -1;
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) { ::close(fd); return -1; }
    if (!access.IsCurrent()) { ::close(fd); return -1; }
    outcome.success = true;
    return fd;
}
bool Stat(const std::filesystem::path& path, struct stat& result) {
    AccessResult outcome;
    const auto key = path.string();
    {
        std::lock_guard lock(mutex);
        if (!vm || !library) { errno = EACCES; return false; }
        if (!RelativePath(path, root)) return false;
        // A hit is validated and published under the mount lock, without JNI.
        if (ReadCache().Get(key, mount_generation, result)) {
            outcome.success = true;
            return true;
        }
    }
    Environment access;
    if (!access) { errno = EACCES; return false; }
    const auto relative = RelativePath(path, access.mount.root);
    if (!relative) return false;
    auto name = access.env->NewStringUTF(relative->c_str());
    if (!name) { Check(access.env); return false; }
    auto values = static_cast<jlongArray>(access.env->CallObjectMethod(access.mount.owner, access.mount.stat, name));
    access.env->DeleteLocalRef(name);
    if (!Check(access.env)) { if (values) access.env->DeleteLocalRef(values); return false; }
    if (!values) { errno = ENOENT; return false; }
    if (access.env->GetArrayLength(values) != 3) {
        access.env->DeleteLocalRef(values); errno = EIO; return false;
    }
    jlong info[3]{};
    access.env->GetLongArrayRegion(values, 0, 3, info);
    access.env->DeleteLocalRef(values);
    if (!Check(access.env) || (!S_ISREG(info[0]) && !S_ISDIR(info[0])) || info[1] < 0) {
        errno = EIO; return false;
    }
    struct stat snapshot{};
    snapshot.st_mode = info[0];
    snapshot.st_size = info[1];
    snapshot.st_mtim.tv_sec = info[2] / 1000;
    snapshot.st_mtim.tv_nsec = (info[2] % 1000) * 1000000;
    snapshot.st_atim = snapshot.st_ctim = snapshot.st_mtim;
    snapshot.st_nlink = S_ISDIR(snapshot.st_mode) ? 2 : 1;
    snapshot.st_blksize = 4096;
    snapshot.st_blocks = (snapshot.st_size + 511) / 512;
    snapshot.st_dev = 0x4c554d;
    uint64_t identity = 14695981039346656037ull;
    for (unsigned char byte : path.lexically_normal().string()) {
        identity ^= byte;
        identity *= 1099511628211ull;
    }
    snapshot.st_ino = identity;
    if (!access.PublishCurrent([&] {
            ReadCache().Put(key, access.mount.generation, snapshot);
            result = snapshot;
        })) return false;
    outcome.success = true;
    return true;
}
std::vector<std::string> List(const std::filesystem::path& path, std::error_code& error) {
    AccessResult outcome;
    std::vector<std::string> result;
    error.clear();
    Environment access;
    if (!access) { error = std::make_error_code(std::errc::permission_denied); return result; }
    auto name = Relative(access, path);
    if (!name) { Check(access.env); error = std::error_code(errno, std::generic_category()); return result; }
    auto values = static_cast<jobjectArray>(access.env->CallObjectMethod(access.mount.owner, access.mount.list, name));
    access.env->DeleteLocalRef(name);
    if (!Check(access.env) || !values) {
        if (values) access.env->DeleteLocalRef(values);
        error = std::make_error_code(std::errc::io_error); return result;
    }
    const auto count = access.env->GetArrayLength(values);
    for (jsize i = 0; i < count; ++i) {
        auto entry = static_cast<jstring>(access.env->GetObjectArrayElement(values, i));
        if (!entry) { error = std::make_error_code(std::errc::io_error); break; }
        auto text = access.env->GetStringUTFChars(entry, nullptr);
        std::string value = text ? text : "";
        if (text) access.env->ReleaseStringUTFChars(entry, text);
        access.env->DeleteLocalRef(entry);
        if (value.empty() || value == "." || value == ".." || value.find('/') != std::string::npos) {
            error = std::make_error_code(std::errc::invalid_argument); break;
        }
        result.push_back(std::move(value));
    }
    access.env->DeleteLocalRef(values);
    if (!Check(access.env)) error = std::make_error_code(std::errc::io_error);
    if (!error && !access.IsCurrent()) error = std::error_code(errno, std::generic_category());
    if (error) result.clear();
    outcome.success = !error;
    return result;
}

std::vector<Lumen::Documents::DirectoryEntry> ListEntries(
        const std::filesystem::path& path, std::error_code& error) {
    AccessResult outcome;
    std::vector<Lumen::Documents::DirectoryEntry> result;
    error.clear();
    Environment access;
    if (!access) { error = std::make_error_code(std::errc::permission_denied); return result; }
    auto name = Relative(access, path);
    if (!name) { Check(access.env); error = std::error_code(errno, std::generic_category()); return result; }
    auto values = static_cast<jobjectArray>(access.env->CallObjectMethod(access.mount.owner, access.mount.list_entries, name));
    access.env->DeleteLocalRef(name);
    if (!Check(access.env) || !values) {
        if (values) access.env->DeleteLocalRef(values);
        error = std::make_error_code(std::errc::io_error); return result;
    }
    const auto count = access.env->GetArrayLength(values);
    if (count % 2 != 0 || count / 2 > 200000) {
        error = std::make_error_code(std::errc::invalid_argument);
    }
    const auto read_string = [&](jsize index, std::string& value) {
        auto item = static_cast<jstring>(access.env->GetObjectArrayElement(values, index));
        if (!item || !Check(access.env)) {
            if (item) access.env->DeleteLocalRef(item);
            return false;
        }
        const char* text = access.env->GetStringUTFChars(item, nullptr);
        if (text) {
            value = text;
            access.env->ReleaseStringUTFChars(item, text);
        }
        access.env->DeleteLocalRef(item);
        return Check(access.env) && text != nullptr;
    };
    std::unordered_set<std::string> names;
    for (jsize index = 0; !error && index < count; index += 2) {
        std::string value, type;
        if (!read_string(index, value) || !read_string(index + 1, type)) {
            error = std::make_error_code(std::errc::io_error); break;
        }
        bool valid_name = !value.empty() && value.size() <= 255 && value != "." && value != ".."
            && value.find('/') == std::string::npos && value.find('\\') == std::string::npos;
        for (unsigned char byte : value) {
            if (byte < 0x20 || byte == 0x7f) valid_name = false;
        }
        if (!valid_name || (type != "f" && type != "d") || !names.insert(value).second) {
            error = std::make_error_code(std::errc::invalid_argument); break;
        }
        result.push_back({std::move(value), type == "f"});
    }
    access.env->DeleteLocalRef(values);
    if (!Check(access.env)) error = std::make_error_code(std::errc::io_error);
    if (!error && !access.IsCurrent()) error = std::error_code(errno, std::generic_category());
    if (error) result.clear();
    outcome.success = !error;
    return result;
}
}

extern "C" void LumenDocumentBeginReadOperation() {
    ++thread_state.operation.depth;
    ReadCache().Begin();
}
extern "C" void LumenDocumentEndReadOperation() {
    if (thread_state.operation.depth && --thread_state.operation.depth == 0) {
        thread_state.operation.Finish();
    }
    ReadCache().End();
}
extern "C" bool LumenDocumentIsMounted(const char* path) {
    return path && DocumentBridge::IsMounted(path);
}
extern "C" int LumenDocumentOpenRead(const char* path) {
    if (!path) { AbortReadOperation(); errno = EINVAL; return -1; }
    return DocumentBridge::OpenRead(path);
}
extern "C" bool LumenDocumentStat(const char* path, struct stat* result) {
    if (!path || !result) { AbortReadOperation(); errno = EINVAL; return false; }
    return DocumentBridge::Stat(path, *result);
}
extern "C" int LumenDocumentList(const char* path, void (*entry)(void*, const char*), void* context) {
    if (!path || !entry) { AbortReadOperation(); return EINVAL; }
    std::error_code error;
    auto names = DocumentBridge::List(path, error);
    if (error) return error.value();
    for (const auto& name : names) entry(context, name.c_str());
    return 0;
}

extern "C" int LumenDocumentListEntries(const char* path,
        void (*entry)(void*, const char*, bool), void* context) {
    if (!path || !entry) { AbortReadOperation(); return EINVAL; }
    std::error_code error;
    const auto entries = DocumentBridge::ListEntries(path, error);
    if (error) return error.value();
    // Publish only a completely validated enumeration, after releasing the bridge mutex.
    for (const auto& value : entries) entry(context, value.name.c_str(), value.is_file);
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_it_gssoft_lumenemu_spike_AndroidDocumentFileSystem_nativeInitialize(
        JNIEnv* env, jclass, jobject selected, jstring selected_root) {
    jobject previous{};
    std::uint64_t generation{};
    {
        std::lock_guard lock(mutex);
        generation = ++mount_generation;
        previous = library;
        library = nullptr;
        root.clear();
    }
    AbortReadOperation();
    if (previous) env->DeleteGlobalRef(previous);
    if (!selected) return;
    JavaVM* selected_vm{};
    if (env->GetJavaVM(&selected_vm) != JNI_OK) return;
    auto type = env->GetObjectClass(selected);
    if (!type) return;
    const auto selected_open = env->GetMethodID(type, "openFile", "(Ljava/lang/String;)I");
    const auto selected_stat = env->GetMethodID(type, "stat", "(Ljava/lang/String;)[J");
    const auto selected_list = env->GetMethodID(type, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
    const auto selected_entries = env->GetMethodID(type, "listEntries", "(Ljava/lang/String;)[Ljava/lang/String;");
    const auto selected_begin = env->GetMethodID(type, "beginReadOperation", "()V");
    const auto selected_end = env->GetMethodID(type, "endReadOperation", "()V");
    const auto selected_abort = env->GetMethodID(type, "abortReadOperation", "()V");
    env->DeleteLocalRef(type);
    if (env->ExceptionCheck() || !selected_open || !selected_stat || !selected_list || !selected_entries
            || !selected_begin || !selected_end || !selected_abort) return;
    if (!selected_root) return;
    const char* text = env->GetStringUTFChars(selected_root, nullptr);
    if (!text) return;
    std::filesystem::path requested(text);
    env->ReleaseStringUTFChars(selected_root, text);
    if (!requested.is_absolute() || requested != requested.lexically_normal()) {
        auto exception = env->FindClass("java/lang/IllegalArgumentException");
        if (exception) {
            env->ThrowNew(exception, "Invalid document root");
            env->DeleteLocalRef(exception);
        }
        return;
    }
    auto selected_owner = env->NewGlobalRef(selected);
    if (!selected_owner) return;
    {
        std::lock_guard lock(mutex);
        // A newer initialize/unmount wins while JNI validation runs unlocked.
        if (mount_generation == generation) {
            vm = selected_vm;
            library = selected_owner;
            root = requested.string();
            open_method = selected_open;
            stat_method = selected_stat;
            list_method = selected_list;
            list_entries_method = selected_entries;
            begin_operation_method = selected_begin;
            end_operation_method = selected_end;
            abort_operation_method = selected_abort;
            selected_owner = nullptr;
        }
    }
    if (selected_owner) env->DeleteGlobalRef(selected_owner);
}

namespace {
void ThrowIo(JNIEnv* env, const char* message) {
    auto type = env->FindClass("java/io/IOException");
    if (type) { env->ThrowNew(type, message); env->DeleteLocalRef(type); }
}
std::filesystem::path PathArgument(JNIEnv* env, jstring text) {
    if (!text) return {};
    auto value = env->GetStringUTFChars(text, nullptr);
    if (!value) return {};
    std::filesystem::path path(value);
    env->ReleaseStringUTFChars(text, value);
    return path;
}
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_it_gssoft_lumenemu_spike_AndroidDocumentFileSystem_inspect(JNIEnv* env, jclass, jstring text) {
    const auto path = PathArgument(env, text);
    struct stat info{};
    if (!Lumen::Documents::IsMounted(path) || !Lumen::Documents::Stat(path, info)) {
        ThrowIo(env, "Document is unavailable"); return nullptr;
    }
    const int fd = Lumen::Documents::OpenRead(path);
    if (fd < 0) { ThrowIo(env, "Document descriptor is unavailable"); return nullptr; }
    unsigned char first{}, last{};
    bool okay = info.st_size > 0 && ::read(fd, &first, 1) == 1
        && ::lseek(fd, info.st_size - 1, SEEK_SET) == info.st_size - 1
        && ::read(fd, &last, 1) == 1;
    void* mapped = okay ? ::mmap(nullptr, 1, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    ::close(fd);
    if (mapped == MAP_FAILED) { ThrowIo(env, "Document must support random access and mapping"); return nullptr; }
    jlong values[]{static_cast<jlong>(info.st_mode), info.st_size, first, last,
                  *static_cast<unsigned char*>(mapped)};
    ::munmap(mapped, 1);
    Lumen::HostFileSystem::InputStream stream(path, std::ios::binary | std::ios::ate);
    char stream_first{}, stream_last{};
    if (!stream || stream.tellg() != info.st_size || !stream.seekg(0)
        || !stream.get(stream_first) || !stream.seekg(-1, std::ios::end)
        || !stream.get(stream_last) || static_cast<unsigned char>(stream_first) != first
        || static_cast<unsigned char>(stream_last) != last) {
        ThrowIo(env, "Document stream must preserve random access"); return nullptr;
    }
    auto result = env->NewLongArray(5);
    if (result) env->SetLongArrayRegion(result, 0, 5, values);
    return result;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_it_gssoft_lumenemu_spike_AndroidDocumentFileSystem_list(JNIEnv* env, jclass, jstring text) {
    const auto path = PathArgument(env, text);
    std::error_code error;
    if (!Lumen::Documents::IsMounted(path)) { ThrowIo(env, "Path is outside the document mount"); return nullptr; }
    auto names = Lumen::Documents::List(path, error);
    if (error) { ThrowIo(env, "Document directory is unavailable"); return nullptr; }
    auto type = env->FindClass("java/lang/String");
    if (!type) return nullptr;
    auto result = env->NewObjectArray(names.size(), type, nullptr);
    env->DeleteLocalRef(type);
    if (!result) return nullptr;
    for (size_t index = 0; index < names.size(); ++index) {
        auto name = env->NewStringUTF(names[index].c_str());
        if (!name) return nullptr;
        env->SetObjectArrayElement(result, index, name);
        env->DeleteLocalRef(name);
    }
    return result;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_it_gssoft_lumenemu_spike_AndroidDocumentFileSystem_listEntries(JNIEnv* env, jclass, jstring text) {
    const auto path = PathArgument(env, text);
    std::error_code error;
    if (!Lumen::Documents::IsMounted(path)) { ThrowIo(env, "Path is outside the document mount"); return nullptr; }
    const auto entries = Lumen::Documents::ListEntries(path, error);
    if (error) { ThrowIo(env, "Document directory is unavailable"); return nullptr; }
    auto type = env->FindClass("java/lang/String");
    if (!type) return nullptr;
    auto result = env->NewObjectArray(entries.size() * 2, type, nullptr);
    env->DeleteLocalRef(type);
    if (!result) return nullptr;
    for (size_t index = 0; index < entries.size(); ++index) {
        auto name = env->NewStringUTF(entries[index].name.c_str());
        if (!name) return nullptr;
        env->SetObjectArrayElement(result, index * 2, name);
        env->DeleteLocalRef(name);
        if (env->ExceptionCheck()) return nullptr;
        auto kind = env->NewStringUTF(entries[index].is_file ? "f" : "d");
        if (!kind) return nullptr;
        env->SetObjectArrayElement(result, index * 2 + 1, kind);
        env->DeleteLocalRef(kind);
        if (env->ExceptionCheck()) return nullptr;
    }
    return result;
}
