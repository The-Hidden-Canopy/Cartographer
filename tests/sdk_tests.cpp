#include <cartographer/carto.h>

#include <carto/core/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("cartographer-sdk-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

void require_ok(carto_status status, carto_error& error) {
    REQUIRE(status == CARTO_OK);
    REQUIRE(error.code == CARTO_OK);
    REQUIRE(error.message == nullptr);
}

void sdk_exposes_bounded_versioned_query_and_execute_surface() {
    REQUIRE(carto_abi_version() == 1U);
    carto_error negotiation_error{};
    uint32_t negotiated_version = 0U;
    require_ok(carto_abi_negotiate(1U, 1U, &negotiated_version, &negotiation_error),
               negotiation_error);
    REQUIRE(negotiated_version == 1U);
    REQUIRE(carto_abi_negotiate(2U, 3U, &negotiated_version, &negotiation_error) ==
            CARTO_UNSUPPORTED_VERSION);
    REQUIRE(negotiated_version == 0U);
    REQUIRE(negotiation_error.message != nullptr);
    carto_free(negotiation_error.message);
    negotiation_error = {};
    REQUIRE(carto_abi_negotiate(0U, 1U, &negotiated_version, &negotiation_error) ==
            CARTO_INVALID_ARGUMENT);
    REQUIRE(negotiated_version == 0U);
    REQUIRE(negotiation_error.message != nullptr);
    carto_free(negotiation_error.message);
    TempDirectory temp;
    carto_context* context = nullptr;
    carto_error error{};
    const std::string config =
        "{\"project_root\":\"" + temp.path().generic_string() + "\"}";
    require_ok(carto_context_create(config.c_str(), &context, &error), error);
    REQUIRE(context != nullptr);

    const auto missing = carto_document_open(context, "missing.carto", nullptr, &error);
    REQUIRE(missing == CARTO_INVALID_ARGUMENT);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    const auto outside_path = temp.path().parent_path() / "cartographer-sdk-outside.carto";
    std::error_code ignored;
    carto_document* rejected = nullptr;
    std::filesystem::remove(outside_path, ignored);
    REQUIRE(carto_document_open(context, outside_path.string().c_str(), &rejected, &error) ==
            CARTO_INVALID_ARGUMENT);
    REQUIRE(rejected == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    const auto path = temp.path() / "scene.carto";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "CARTOGRAPHER_PROJECT 1\nNAME \"SDK\tName\"\nREVISION 0\nOBJECTS 0\nMESHES 0\nEND\n";
    }
    carto_document* document = nullptr;
    require_ok(carto_document_open(context, path.string().c_str(), &document, &error), error);
    REQUIRE(document != nullptr);

    REQUIRE(carto_document_open(context, "../outside.carto", &rejected, &error) == CARTO_INVALID_ARGUMENT);
    REQUIRE(rejected == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    char* response = nullptr;
    require_ok(carto_document_query_json(document, "{\"op\":\"capabilities\"}", &response, &error), error);
    REQUIRE(std::string(response).find("abi_min_version") != std::string::npos);
    REQUIRE(std::string(response).find("create_box") != std::string::npos);
    carto_free(response);
    response = nullptr;

    require_ok(carto_document_query_json(document, "{\"op\":\"snapshot\"}", &response, &error), error);
    REQUIRE(carto::core::json::is_object(response));
    REQUIRE(std::string(response).find("SDK\\tName") != std::string::npos);
    carto_free(response);
    response = nullptr;

    require_ok(carto_document_execute_json(
        document, "{\"op\":\"create_box\",\"name\":\"Box\"}", &response, &error), error);
    REQUIRE(std::string(response).find("\"document_changed\":true") != std::string::npos);
    carto_free(response);
    response = nullptr;

    REQUIRE(carto_document_execute_json(
                document,
                "{\"op\":\"save\",\"path\":\"../outside.carto\"}",
                &response,
                &error) == CARTO_INVALID_ARGUMENT);
    REQUIRE(response == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    carto_context_destroy(context);
    context = nullptr;
    require_ok(carto_document_query_json(document, "{\"op\":\"snapshot\"}", &response, &error), error);
    REQUIRE(std::string(response).find("\"object_count\":1") != std::string::npos);
    carto_free(response);
    carto_document_close(document);
    carto_context_destroy(context);
}

void sdk_rejects_oversized_and_unknown_requests() {
    TempDirectory temp;
    carto_context* context = nullptr;
    carto_error error{};
    const std::string config =
        "{\"project_root\":\"" + temp.path().generic_string() + "\"}";
    require_ok(carto_context_create(config.c_str(), &context, &error), error);
    const auto path = temp.path() / "scene.carto";
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << "CARTOGRAPHER_PROJECT 1\nNAME \"SDK\"\nREVISION 0\nOBJECTS 0\nMESHES 0\nEND\n";
    stream.close();
    carto_document* document = nullptr;
    require_ok(carto_document_open(context, path.string().c_str(), &document, &error), error);

    char* response = nullptr;
    REQUIRE(carto_document_query_json(document, "{\"op\":\"unknown\"}", &response, &error) ==
            CARTO_INVALID_ARGUMENT);
    REQUIRE(response == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    REQUIRE(carto_document_query_json(
                document, "{\"nested\":{\"op\":\"snapshot\"}}", &response, &error) ==
            CARTO_INVALID_ARGUMENT);
    REQUIRE(response == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    REQUIRE(carto_document_query_json(
                document, "{\"op\":\"snapshot\",\"op\":\"capabilities\"}",
                &response, &error) == CARTO_INVALID_ARGUMENT);
    REQUIRE(response == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    const std::string oversized(65U * 1024U, 'x');
    REQUIRE(carto_document_query_json(document, oversized.c_str(), &response, &error) ==
            CARTO_INVALID_ARGUMENT);
    REQUIRE(response == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    REQUIRE(carto_document_query_json(document, "{\"op\":\"snapshot\"", &response, &error) ==
            CARTO_INVALID_ARGUMENT);
    REQUIRE(response == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    carto_document_close(document);
    carto_context_destroy(context);
}

void sdk_bounds_context_lifetime_and_null_outputs() {
    carto_context* context = nullptr;
    carto_error error{};
    REQUIRE(carto_abi_negotiate(1U, 1U, nullptr, &error) == CARTO_INVALID_ARGUMENT);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};
    const std::string oversized(65U * 1024U, 'x');
    REQUIRE(carto_context_create(oversized.c_str(), &context, &error) == CARTO_INVALID_ARGUMENT);
    REQUIRE(context == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    REQUIRE(carto_context_create("{\"project_root\":\"unterminated\"", &context, &error) ==
            CARTO_INVALID_ARGUMENT);
    REQUIRE(context == nullptr);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};

    require_ok(carto_context_create(nullptr, &context, &error), error);
    REQUIRE(carto_document_query_json(nullptr, "{}", nullptr, &error) == CARTO_INVALID_ARGUMENT);
    REQUIRE(error.message != nullptr);
    carto_free(error.message);
    error = {};
    carto_context_destroy(context);
}

} // namespace

int main() {
    try {
        sdk_exposes_bounded_versioned_query_and_execute_surface();
        sdk_rejects_oversized_and_unknown_requests();
        sdk_bounds_context_lifetime_and_null_outputs();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
