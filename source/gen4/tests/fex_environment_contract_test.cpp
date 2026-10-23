// SPDX-License-Identifier: MIT
#include "Common/Config.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include "../src/lumen/fex_profile.h"

namespace {
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #condition); \
    std::abort(); \
} } while (false)

constexpr std::array<const char*, 8> Keys{
    "FEX_TSOENABLED", "FEX_HALFBARRIERTSOENABLED", "FEX_VECTORTSOENABLED",
    "FEX_STRICTINPROCESSSPLITLOCKS", "FEX_SMCCHECKS", "FEX_MULTIBLOCK",
    "FEX_MAXINST", "FEX_DYNAMICL1CACHE"};
constexpr std::array<const char*, 8> Values{"1", "1", "0", "0", "mtrack", "1", "5000", "1"};

template <std::size_t Size>
struct EnvironmentScope {
    const std::array<const char*, Size>& keys;
    std::array<std::optional<std::string>, Size> saved;
    explicit EnvironmentScope(const std::array<const char*, Size>& names) : keys(names) {
        for (std::size_t i = 0; i < Size; ++i) {
            if (const auto* value = std::getenv(keys[i])) saved[i] = value;
            CHECK(unsetenv(keys[i]) == 0);
        }
    }
    ~EnvironmentScope() {
        for (std::size_t i = 0; i < Size; ++i) {
            if (saved[i]) CHECK(setenv(keys[i], saved[i]->c_str(), 1) == 0);
            else CHECK(unsetenv(keys[i]) == 0);
        }
    }
};

struct ConfigScope {
    explicit ConfigScope(char* const* envp = nullptr) {
        FEX::Config::InitializeConfigs({});
        FEXCore::Config::Initialize();
        FEXCore::Config::AddLayer(FEX::Config::CreateEnvironmentLayer(envp));
        FEXCore::Config::Load();
    }
    ~ConfigScope() { FEXCore::Config::Shutdown(); }
};
} // namespace

int main() {
    using namespace FEXCore::Config;
    EnvironmentScope environment{Keys};
    {
        ConfigScope config;
        CHECK(!Exists(CONFIG_SMCCHECKS));
        CHECK(Get_SMCCHECKS()() == CONFIG_SMC_MTRACK);
        CHECK(Get_MAXINST()() == 5000);
    }
    for (std::size_t i = 0; i < Keys.size(); ++i) CHECK(setenv(Keys[i], Values[i], 1) == 0);
    {
        ConfigScope config;
        CHECK(Get_TSOENABLED()());
        CHECK(Get_HALFBARRIERTSOENABLED()());
        CHECK(!Get_VECTORTSOENABLED()());
        CHECK(!Get_STRICTINPROCESSSPLITLOCKS()());
        CHECK(Get_MULTIBLOCK()());
        CHECK(Get_MAXINST()() == 5000);
        CHECK(Get_DYNAMICL1CACHE()());
        CHECK(Exists(CONFIG_SMCCHECKS));
        CHECK(**Get(CONFIG_SMCCHECKS) == "mtrack");
        // The original nullptr-envp path reads getenv after the enum parser.
        // Its numeric getter therefore converts this literal to zero.
        CHECK(Get_SMCCHECKS()() == CONFIG_SMC_NONE);
    }
    CHECK(setenv("FEX_MAXINST", "1234", 1) == 0);
    CHECK(setenv("FEX_TSOENABLED", "0", 1) == 0);
    {
        ConfigScope config;
        CHECK(Get_MAXINST()() == 1234);
        CHECK(!Get_TSOENABLED()());
    }
    {
        char smc[] = "FEX_SMCCHECKS=mtrack";
        char max_inst[] = "FEX_MAXINST=2345";
        char* explicit_environment[]{smc, max_inst, nullptr};
        ConfigScope config(explicit_environment);
        CHECK(Get_SMCCHECKS()() == CONFIG_SMC_MTRACK);
        CHECK(Get_MAXINST()() == 2345);
        CHECK(!Get_TSOENABLED()());
    }
    for (const auto* key : Keys) CHECK(unsetenv(key) == 0);
    {
        ConfigScope config;
        CHECK(!Exists(CONFIG_SMCCHECKS));
        CHECK(Get_SMCCHECKS()() == CONFIG_SMC_MTRACK);
        CHECK(Get_MAXINST()() == 5000);
        CHECK(Get_TSOENABLED()());
    }
    {
        constexpr std::array<const char*, 3> directory_keys{
            "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_SHADER_PATH", "FEX_APP_CACHE_LOCATION"};
        EnvironmentScope directory_environment{directory_keys};
        ConfigScope config;
        CHECK(GetDataDirectory(true) == "/usr/share/fex-emu/");
        CHECK(GetConfigDirectory(true) == "/usr/share/fex-emu/");
        CHECK(GetConfigFileLocation(true) == "/usr/share/fex-emu/Config.json");
        const auto data_directory = GetDataDirectory(false);
        const auto config_directory = GetConfigDirectory(false);
        const auto cache_directory = FEX::Config::GetCacheDirectory();
        CHECK(setenv("STEAM_COMPAT_DATA_PATH", "/environment-contract/steam-data", 1) == 0);
        CHECK(setenv("STEAM_COMPAT_SHADER_PATH", "/environment-contract/steam-shader", 1) == 0);
        FEX::Config::InitializeConfigs({});
        CHECK(GetDataDirectory(true) == "/usr/share/fex-emu/");
        CHECK(GetConfigDirectory(true) == "/usr/share/fex-emu/");
        CHECK(GetDataDirectory(false) == data_directory);
        CHECK(GetConfigDirectory(false) == config_directory);
        CHECK(FEX::Config::GetCacheDirectory() == cache_directory);
    }
#if defined(__APPLE__)
    {
        constexpr std::array<const char*, 2> extra_keys{
            "FEX_DISABLEOPT", "LUMEN_CPU_TRANSLATOR"};
        EnvironmentScope extra_environment{extra_keys};
        CHECK(setenv("LUMEN_CPU_TRANSLATOR", "qemu-tcg", 1) == 0);
        bool rejected = false;
        try { Lumen::Gen4::ConfigureDesktopFexProfile(); }
        catch (const std::system_error& error) { rejected = error.code().value() == ENOTSUP; }
        CHECK(rejected);
        CHECK(setenv("LUMEN_CPU_TRANSLATOR", "fex", 1) == 0);
        CHECK(setenv("FEX_DISABLEOPT", "1", 1) == 0);
        CHECK(setenv("FEX_MAXINST", "128", 1) == 0);
        Lumen::Gen4::ConfigureDesktopFexProfile();
        CHECK(std::getenv("FEX_DISABLEOPT") == nullptr);
        for (std::size_t index = 0; index < Keys.size(); ++index) {
            CHECK(std::string_view{std::getenv(Keys[index])} == Values[index]);
        }
        ConfigScope config;
        CHECK(Get_MAXINST()() == 5000);
        CHECK(Get_MULTIBLOCK()());
        CHECK(Get_SMCCHECKS()() == CONFIG_SMC_NONE);
        CHECK(Get_TSOENABLED()() && Get_HALFBARRIERTSOENABLED()());
        CHECK(!Get_VECTORTSOENABLED()() && !Get_STRICTINPROCESSSPLITLOCKS()());
        CHECK(Get_DYNAMICL1CACHE()());
    }
#endif
    std::puts("PASS original environment loader: default-smc=1 getenv-mtrack=0 explicit-envp-mtrack=1; eight-value profile, override, shutdown/reload; original global directories and Steam variables ignored");
}
