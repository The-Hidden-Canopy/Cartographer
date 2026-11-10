#include "authoring_shader.hpp"
#include <carto/assets/blob_store.hpp>
#include <carto/d3d12/builtin_shader_cache.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace carto;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T take(core::Result<T> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
    return std::move(r.value());
}
const d3d12::ShaderSource tiny{"cache.cpu",
                               gpu::ShaderStage::fragment,
                               "PSMain",
                               "builtin.hlsl",
                               "float4 PSMain():SV_TARGET{return float4(1,.5,.25,1);}",
                               true,
                               false};
void contracts(const std::filesystem::path &directory) {
    const auto runtime = take(d3d12::dxc_runtime_fingerprint());
    const auto original_key = take(d3d12::BuiltinShaderCache::key(tiny, runtime));
    auto changed = tiny;
    changed.source += "\n//changed source";
    check(take(d3d12::BuiltinShaderCache::key(changed, runtime)) != original_key,
          "source invalidates cache");
    changed = tiny;
    changed.optimize = false;
    check(take(d3d12::BuiltinShaderCache::key(changed, runtime)) != original_key,
          "optimization invalidates cache");
    changed = tiny;
    changed.entry_point = "Other";
    check(take(d3d12::BuiltinShaderCache::key(changed, runtime)) != original_key,
          "entry invalidates cache");
    changed = tiny;
    changed.stage = gpu::ShaderStage::vertex;
    check(take(d3d12::BuiltinShaderCache::key(changed, runtime)) != original_key,
          "stage invalidates cache");
    changed = tiny;
    changed.source_name = "Other.hlsl";
    check(take(d3d12::BuiltinShaderCache::key(changed, runtime)) != original_key,
          "source-name/options identity is explicit");
    check(take(d3d12::BuiltinShaderCache::key(tiny, std::string(64, 'a'))) != original_key,
          "compiler runtime fingerprint invalidates cache");
    d3d12::BuiltinShaderCache cache(directory);
    const auto cold = take(cache.compile(tiny));
    check(!cold.hit && cold.stored && cold.compile_ms > 0,
          "cold compile publishes bounded validated artifact");
    const auto warm = take(d3d12::BuiltinShaderCache(directory).compile(tiny));
    check(warm.hit && warm.compile_ms == 0 && warm.binary.bytes == cold.binary.bytes,
          "new cache instance reuses exact DXIL without compilation");
    check(bool(d3d12::validate_dxil(warm.binary.bytes)),
          "cached DXIL passes installed CPU validator");
    auto renamed = tiny;
    renamed.debug_name = "another display name";
    const auto rebound = take(cache.compile(renamed));
    check(rebound.hit && rebound.binary.debug_name == renamed.debug_name,
          "cache stores no display name and returns current caller metadata");
    auto invalid = tiny;
    invalid.source = "not hlsl";
    check(!cache.compile(invalid), "bad replacement shader cannot publish an artifact");
    check(take(cache.compile(tiny)).hit, "failed replacement preserves prior good cache entry");
    const auto path = directory / (original_key + ".dxil-cache");
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(81);
        file.put('\x37');
    }
    const auto repaired = take(cache.compile(tiny));
    check(!repaired.hit && repaired.stored && take(cache.compile(tiny)).hit,
          "checksum-corrupt entry recompiles and atomically replaces only itself");
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        bytes[80] = 'X';
        const auto hash = assets::sha256(std::span<const std::uint8_t>(bytes).subspan(80));
        std::copy(hash.bytes.begin(), hash.bytes.end(), bytes.begin() + 40);
        file.close();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    check(!take(cache.compile(tiny)).hit && take(cache.compile(tiny)).hit,
          "validator rejects invalid DXIL even with a matching payload checksum");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "truncated";
    }
    check(!take(cache.compile(tiny)).hit, "truncated cache never becomes shader bytecode");
    const auto blocked = directory / "not-a-directory";
    {
        std::ofstream file(blocked);
        file << "held";
    }
    const auto fallback = take(d3d12::BuiltinShaderCache(blocked).compile(tiny));
    check(!fallback.hit && !fallback.stored && !fallback.binary.bytes.empty(),
          "cache write failure preserves usable compiled shader");
    const auto contended = directory / "contended";
    std::filesystem::create_directory(contended);
    const auto lock = CreateFileW((contended / ".cache-lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "CPU cache lock fixture");
    OVERLAPPED overlap{};
    check(LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                     &overlap) != 0,
          "CPU fixture owns cache admission lease");
    const auto contention = take(d3d12::BuiltinShaderCache(contended).compile(tiny));
    UnlockFileEx(lock, 0, 1, 0, &overlap);
    CloseHandle(lock);
    check(!contention.stored && !contention.binary.bytes.empty(),
          "contended cache never blocks or publishes beyond admission budget");
    check(take(d3d12::BuiltinShaderCache(contended).compile(tiny)).stored,
          "released admission lease allows later cache publication");
    auto debug = tiny;
    debug.debug_info = true;
    check(!take(cache.compile(debug)).stored, "debug source/symbols are never persisted");
    auto include = tiny;
    include.source = "#if 0\n# include \"external.hlsl\"\n#endif\n" + include.source;
    check(!take(cache.compile(include)).stored,
          "untracked include dependencies bypass persistent caching");
    include = tiny;
    include.source = "#if 0\n#inc\\\nlude \"external.hlsl\"\n#endif\n" + include.source;
    check(!take(cache.compile(include)).stored,
          "line-spliced include directives also bypass persistent caching");
    const auto full = directory / "full";
    std::filesystem::create_directory(full);
    for (unsigned i = 0; i < 64; ++i) {
        std::ofstream file(full / std::to_string(i));
        file << "held";
    }
    check(!take(d3d12::BuiltinShaderCache(full).compile(tiny)).stored,
          "cache admission budget never deletes existing files");
    std::cout << "CPU builtin DXIL cache: key invalidation, exact reuse, validation, "
                 "corruption/truncation repair, failure atomicity, write fallback, include/debug "
                 "bypass and entry budget passed; no GPU\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 3 &&
            (std::string_view(argv[1]) == "--seed" || std::string_view(argv[1]) == "--reopen")) {
            const auto code =
                std::string("#define CARTO_FUSED_IDENTITY 1\n#define CARTO_AUTHORING_SURFACE 1\n") +
                carto_authoring_surface_source + carto_authoring_pbr_source;
            auto shader = tiny;
            shader.source = code;
            shader.source_name = "authoring_surface.hlsl";
            const auto result = take(d3d12::BuiltinShaderCache(argv[2]).compile(shader));
            check(result.hit == (std::string_view(argv[1]) == "--reopen"),
                  "separate-process cache expectation");
            std::cout << "CPU built-in surface PSMain: hit=" << result.hit
                      << " lookup_ms=" << result.lookup_ms << " compile_ms=" << result.compile_ms
                      << " store_ms=" << result.store_ms
                      << " binary_sha256=" << result.binary.binary_digest
                      << "; no GPU/device/pipeline\n";
            return 0;
        }
        const auto directory =
            std::filesystem::temp_directory_path() /
            ("cartographer-cache-cpu-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        contracts(directory);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
