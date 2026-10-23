#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/botanical/source.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace carto::runtime_api {

inline constexpr std::string_view kBotanicalGltfOutputKind = "gltf-v1";
inline constexpr std::string_view kBotanicalGlbOutputKind = "glb-v2";

struct Config final {
    std::string base_url;
    std::string function_key;
    std::string bearer_token;
    std::string organization;

    [[nodiscard]] static Config from_environment();
    [[nodiscard]] core::Result<void> validate() const;
};

struct Request final {
    std::string method;
    std::string path;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
};

struct Response final {
    std::uint32_t status = 0U;
    std::vector<std::uint8_t> body;
    std::vector<std::pair<std::string, std::string>> headers;
};

class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual core::Result<Response> send(
        const Config& config, const Request& request) = 0;
};

// Uses the operating system HTTPS stack on Windows. It never follows redirects,
// which prevents authorization headers from crossing an origin boundary.
[[nodiscard]] std::shared_ptr<Transport> make_native_transport();

struct JobReceipt final {
    std::string job_id;
    std::string status;
    std::string source_identity;
    core::Revision source_revision;
    std::string source_artifact_id;
    assets::Sha256Digest source_digest;
    std::string output_artifact_id;
    assets::Sha256Digest output_digest;
    std::string output_kind;
    // Ephemeral authorization context. This is populated by the API client and
    // must never be persisted in .carto project data.
    std::string organization_scope;
};

class Client final {
public:
    Client(Config config, std::shared_ptr<Transport> transport);

    // This is deliberately an explicit operation. Constructing a client or
    // opening a project does not send source data. The caller vouches for the
    // serialized source bytes and their exact revision; the client binds the
    // upload to their digest without parsing species content.
    [[nodiscard]] core::Result<JobReceipt> submit_botanical_source(
        std::string_view source_identity,
        core::Revision source_revision,
        std::string_view serialized_source,
        std::string_view justification,
        std::string_view request_id);
    [[nodiscard]] core::Result<JobReceipt> get_job(const JobReceipt& submitted_job);
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> download_output(
        const JobReceipt& job);

private:
    Config config_;
    std::shared_ptr<Transport> transport_;
};

} // namespace carto::runtime_api
