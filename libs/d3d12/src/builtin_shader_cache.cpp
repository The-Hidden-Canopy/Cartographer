#include <carto/assets/blob_store.hpp>
#include <carto/d3d12/builtin_shader_cache.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <windows.h>

namespace carto::d3d12 {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
assets::Sha256Digest digest(std::string_view value) {
    return assets::sha256({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
}
bool includes(std::string_view source) {
    // Fail closed even for BOM/whitespace/comment-prefixed directives and
    // preprocessor line splices. False positives merely disable the cache.
    std::string spliced;
    spliced.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\\' && i + 1 < source.size()) {
            if (source[i + 1] == '\n') {
                ++i;
                continue;
            }
            if (source[i + 1] == '\r' && i + 2 < source.size() && source[i + 2] == '\n') {
                i += 2;
                continue;
            }
        }
        spliced.push_back(source[i]);
    }
    return spliced.find("include") != std::string::npos;
}
std::string recipe(const ShaderSource &s) {
    const auto profile = s.stage == gpu::ShaderStage::vertex     ? "vs_6_0"
                         : s.stage == gpu::ShaderStage::fragment ? "ps_6_0"
                                                                 : "cs_6_0";
    return "dxcompiler-runtime|profile=" + std::string(profile) + "|entry=" + s.entry_point +
           "|optimization=" + (s.optimize ? "O3" : "O0") +
           "|debug=" + (s.debug_info ? "Zi" : "Qstrip_debug");
}
constexpr std::size_t maximum_binary = 4U * 1024U * 1024U, header_size = 80, maximum_entries = 64;
constexpr std::array<std::uint8_t, 8> magic = {'C', 'T', 'D', 'X', 'I', 'L', '1', 0};
std::optional<device::ShaderBinary> read(const std::filesystem::path &path,
                                         const ShaderSource &source,
                                         const assets::Sha256Digest &key) {
    std::error_code error;
    if (std::filesystem::is_symlink(path, error))
        return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < header_size || size > header_size + maximum_binary)
        return {};
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        return {};
    if (!std::equal(magic.begin(), magic.end(), bytes.begin()) ||
        !std::equal(key.bytes.begin(), key.bytes.end(), bytes.begin() + 8))
        return {};
    std::uint64_t count = 0;
    for (unsigned i = 0; i < 8; ++i)
        count |= std::uint64_t(bytes[72 + i]) << (i * 8);
    if (count != size - header_size)
        return {};
    const auto payload = std::span<const std::uint8_t>(bytes).subspan(header_size);
    const auto hash = assets::sha256(payload);
    if (!std::equal(hash.bytes.begin(), hash.bytes.end(), bytes.begin() + 40) ||
        !validate_dxil(payload))
        return {};
    return device::ShaderBinary{source.debug_name,
                                source.stage,
                                source.entry_point,
                                device::BinaryFormat::dxil,
                                digest(source.source).hex(),
                                digest(recipe(source)).hex(),
                                std::vector<std::uint8_t>(payload.begin(), payload.end()),
                                hash.hex()};
}
bool store(const std::filesystem::path &directory, const std::filesystem::path &path,
           const assets::Sha256Digest &key, const device::ShaderBinary &binary) {
    if (binary.bytes.empty() || binary.bytes.size() > maximum_binary)
        return false;
    std::error_code error;
    if (std::filesystem::is_symlink(directory, error) || std::filesystem::is_symlink(path, error))
        return false;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    // A nonblocking cross-process lease protects admission as well as atomic
    // replacement. Contention falls back to the already compiled shader.
    struct WriteLease {
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlap{};
        bool locked = false;
        ~WriteLease() {
            if (handle != INVALID_HANDLE_VALUE) {
                if (locked)
                    UnlockFileEx(handle, 0, 1, 0, &overlap);
                CloseHandle(handle);
            }
        }
    } lease;
    const auto lock_path = directory / ".cache-lock";
    if (std::filesystem::is_symlink(lock_path, error))
        return false;
    lease.handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lease.handle == INVALID_HANDLE_VALUE)
        return false;
    lease.locked = LockFileEx(lease.handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                              1, 0, &lease.overlap) != 0;
    if (!lease.locked)
        return false;
    std::size_t entries = 0;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        if (it->path().filename() == ".cache-lock")
            continue;
        ++entries;
        if (entries > maximum_entries ||
            (entries >= maximum_entries && !std::filesystem::exists(path)))
            return false;
    }
    if (error)
        return false;
    static std::atomic<std::uint64_t> counter = 0;
    auto temporary = path;
    temporary +=
        L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(counter++);
    std::vector<std::uint8_t> bytes(magic.begin(), magic.end());
    bytes.insert(bytes.end(), key.bytes.begin(), key.bytes.end());
    const auto hash = assets::sha256(binary.bytes);
    bytes.insert(bytes.end(), hash.bytes.begin(), hash.bytes.end());
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(std::uint8_t(std::uint64_t(binary.bytes.size()) >> (i * 8)));
    bytes.insert(bytes.end(), binary.bytes.begin(), binary.bytes.end());
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool complete =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!complete) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    const bool published = MoveFileExW(temporary.c_str(), path.c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!published)
        std::filesystem::remove(temporary, error);
    return published;
}
} // namespace
core::Result<std::string> BuiltinShaderCache::key(const ShaderSource &s, std::string_view runtime) {
    if (s.debug_name.empty() || s.source.empty() || s.source.size() > 16U * 1024U * 1024U ||
        s.entry_point.empty() || s.entry_point.size() > 256 ||
        !assets::Sha256Digest::from_hex(runtime) ||
        (s.stage != gpu::ShaderStage::vertex && s.stage != gpu::ShaderStage::fragment &&
         s.stage != gpu::ShaderStage::compute))
        return core::Result<std::string>::failure(
            {core::ErrorCode::invalid_argument, "builtin shader cache identity is invalid"});
    // Fixed compiler arguments belong to the key as well as per-call options.
    std::string input = "cartographer-builtin-cache-v1|HV2021|Ges|shader-model6_0|";
    for (const auto part : {std::string_view(s.source), std::string_view(s.source_name),
                            std::string_view(s.entry_point), runtime}) {
        input += std::to_string(part.size()) + ":";
        input.append(part);
    }
    input += std::to_string(static_cast<unsigned>(s.stage));
    input += s.optimize ? "O3" : "O0";
    input += s.debug_info ? "debug" : "stripped";
    return core::Result<std::string>::success(digest(input).hex());
}
std::filesystem::path BuiltinShaderCache::default_directory() {
    std::array<wchar_t, 32768> name{};
    const auto size = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    return !size || size >= name.size()
               ? std::filesystem::path{}
               : std::filesystem::path(std::wstring(name.data(), size)).parent_path() /
                     ".carto-shader-cache";
}
core::Result<BuiltinShaderCacheResult>
BuiltinShaderCache::compile(const ShaderSource &source) const {
    using Result = core::Result<BuiltinShaderCacheResult>;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    BuiltinShaderCacheResult out;
    const auto lookup = Clock::now();
    std::optional<assets::Sha256Digest> cache_key;
    std::error_code error;
    if (!directory_.empty() && !source.debug_info && !includes(source.source) &&
        !std::filesystem::is_symlink(directory_, error)) {
        auto runtime = dxc_runtime_fingerprint();
        if (runtime) {
            auto value = key(source, runtime.value());
            if (value)
                cache_key = assets::Sha256Digest::from_hex(value.value()).value();
        }
    }
    const auto path =
        cache_key ? directory_ / (cache_key->hex() + ".dxil-cache") : std::filesystem::path{};
    if (cache_key)
        if (auto binary = read(path, source, *cache_key)) {
            out.binary = std::move(*binary);
            out.hit = true;
            out.lookup_ms = elapsed(lookup);
            return Result::success(std::move(out));
        }
    out.lookup_ms = elapsed(lookup);
    const auto compiling = Clock::now();
    auto compiled = compile_hlsl(source);
    if (!compiled)
        return Result::failure(compiled.error());
    out.compile_ms = elapsed(compiling);
    out.binary = std::move(compiled.value());
    const auto writing = Clock::now();
    if (cache_key)
        out.stored = store(directory_, path, *cache_key, out.binary);
    out.store_ms = elapsed(writing);
    out.note = !cache_key   ? "persistent cache bypassed"
               : out.stored ? "compiled and cached"
                            : "cache unavailable/full; compiled binary remains usable";
    return Result::success(std::move(out));
}
} // namespace carto::d3d12
