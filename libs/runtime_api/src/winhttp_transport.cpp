#include <carto/runtime_api/client.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace carto::runtime_api {
namespace {

constexpr std::size_t kMaximumResponseBytes = 40U * 1024U * 1024U;

core::Diagnostic network_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) {
        return {};
    }
    return result;
}

class Handle final {
public:
    explicit Handle(HINTERNET value = nullptr) : value_(value) {}
    ~Handle() { if (value_ != nullptr) WinHttpCloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
private:
    HINTERNET value_ = nullptr;
};

std::string query_header(HINTERNET request, const wchar_t* header_name) {
    DWORD bytes = 0U;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, header_name,
        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, header_name,
            value.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return output;
}

class WinHttpTransport final : public Transport {
public:
    core::Result<Response> send(const Config& config, const Request& request) override {
        if (auto valid = config.validate(); !valid) {
            return core::Result<Response>::failure(valid.error());
        }
        if (request.path.empty() || request.path.front() != '/' ||
            request.path.find_first_of("?#\\\r\n") != std::string::npos ||
            request.body.size() > kMaximumResponseBytes) {
            return core::Result<Response>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument, "runtime HTTP request is invalid"));
        }
        const auto url = widen(config.base_url);
        const auto method = widen(request.method);
        const auto path = widen(request.path);
        if (url.empty() || method.empty() || path.empty()) {
            return core::Result<Response>::failure(network_error(
                "runtime HTTPS request contains invalid UTF-8"));
        }
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0U, &components) ||
            components.nScheme != INTERNET_SCHEME_HTTPS || components.dwHostNameLength == 0U ||
            components.dwUrlPathLength > 1U || components.dwExtraInfoLength != 0U) {
            return core::Result<Response>::failure(network_error(
                "runtime endpoint must be an HTTPS origin without a path"));
        }
        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        Handle session(WinHttpOpen(L"Cartographer/PrivateRuntime",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0U));
        if (session.get() == nullptr) {
            return core::Result<Response>::failure(network_error(
                "Windows HTTPS session could not be opened"));
        }
        Handle connection(WinHttpConnect(session.get(), host.c_str(),
            components.nPort, 0U));
        if (connection.get() == nullptr) {
            return core::Result<Response>::failure(network_error(
                "private generation service could not be reached"));
        }
        Handle http_request(WinHttpOpenRequest(connection.get(), method.c_str(),
            path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE));
        if (http_request.get() == nullptr) {
            return core::Result<Response>::failure(network_error(
                "private generation HTTPS request could not be created"));
        }
        constexpr int timeout_ms = 10'000;
        WinHttpSetTimeouts(http_request.get(), timeout_ms, timeout_ms,
            timeout_ms, 30'000);
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(http_request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                &disabled, sizeof(disabled))) {
            return core::Result<Response>::failure(network_error(
                "HTTPS redirect protection could not be enabled"));
        }
        std::wstring headers;
        for (const auto& [name, value] : request.headers) {
            const auto wide_name = widen(name);
            const auto wide_value = widen(value);
            if (wide_name.empty() || wide_value.empty() ||
                name.find_first_of(":\r\n") != std::string::npos ||
                value.find_first_of("\r\n") != std::string::npos) {
                return core::Result<Response>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_argument, "runtime HTTP header is invalid"));
            }
            headers += wide_name + L": " + wide_value + L"\r\n";
        }
        if (!request.content_type.empty()) {
            const auto content_type = widen(request.content_type);
            if (content_type.empty()) {
                return core::Result<Response>::failure(network_error(
                    "runtime content type is invalid"));
            }
            headers += L"Content-Type: " + content_type + L"\r\n";
        }
        const DWORD body_size = static_cast<DWORD>(request.body.size());
        void* body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA :
            const_cast<std::uint8_t*>(request.body.data());
        if (!WinHttpSendRequest(http_request.get(),
                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0U : static_cast<DWORD>(headers.size()),
                body, body_size, body_size, 0U) ||
            !WinHttpReceiveResponse(http_request.get(), nullptr)) {
            return core::Result<Response>::failure(network_error(
                "private generation request failed without an approved response"));
        }
        Response response;
        DWORD status_size = sizeof(response.status);
        if (!WinHttpQueryHeaders(http_request.get(), WINHTTP_QUERY_STATUS_CODE |
                WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                &response.status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
            return core::Result<Response>::failure(network_error(
                "private generation response status is unavailable"));
        }
        std::array<std::uint8_t, 16U * 1024U> chunk{};
        for (;;) {
            DWORD available = 0U;
            if (!WinHttpQueryDataAvailable(http_request.get(), &available)) {
                return core::Result<Response>::failure(network_error(
                    "private generation response body could not be read"));
            }
            if (available == 0U) break;
            if (available > kMaximumResponseBytes - response.body.size()) {
                return core::Result<Response>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "private generation response exceeds the configured size limit"));
            }
            while (available > 0U) {
                const DWORD request_size = std::min<DWORD>(available,
                    static_cast<DWORD>(chunk.size()));
                DWORD received = 0U;
                if (!WinHttpReadData(http_request.get(), chunk.data(), request_size,
                        &received) || received == 0U) {
                    return core::Result<Response>::failure(network_error(
                        "private generation response body was truncated"));
                }
                response.body.insert(response.body.end(), chunk.begin(),
                    chunk.begin() + static_cast<std::ptrdiff_t>(received));
                available -= received;
            }
        }
        for (const wchar_t* name : {L"x-cartographer-content-sha256",
                                    L"x-cartographer-artifact-kind"}) {
            const std::string value = query_header(http_request.get(), name);
            if (!value.empty()) {
                const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                    name, -1, nullptr, 0, nullptr, nullptr);
                if (count > 1) {
                    std::wstring header_wide(name);
                    std::string header_name(static_cast<std::size_t>(count - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, header_wide.c_str(),
                        -1, header_name.data(), count, nullptr, nullptr);
                    response.headers.emplace_back(std::move(header_name), value);
                }
            }
        }
        return core::Result<Response>::success(std::move(response));
    }
};

} // namespace

std::shared_ptr<Transport> make_native_transport() {
    return std::make_shared<WinHttpTransport>();
}

} // namespace carto::runtime_api

#else

namespace carto::runtime_api {
namespace {
class UnsupportedTransport final : public Transport {
public:
    core::Result<Response> send(const Config&, const Request&) override {
        return core::Result<Response>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "private generation HTTPS transport is currently supported on Windows only"));
    }
};
} // namespace
std::shared_ptr<Transport> make_native_transport() {
    return std::make_shared<UnsupportedTransport>();
}
} // namespace carto::runtime_api

#endif
