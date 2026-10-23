#include <carto/runtime_api/client.hpp>

#include <carto/core/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace carto::runtime_api {
namespace {

using core::Diagnostic;
using core::ErrorCode;

constexpr std::size_t kMaxSourceBytes = 16U * 1024U;
constexpr std::size_t kMaxResponseBytes = 40U * 1024U * 1024U;
constexpr std::string_view kApiPrefix = "/api/cartographer/runtime/v1";

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic protocol(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

bool opaque_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '_' ||
            character == '-';
    });
}

std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const char raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        switch (byte) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20U) {
                result += "\\u00";
                result.push_back(hex[(byte >> 4U) & 0x0fU]);
                result.push_back(hex[byte & 0x0fU]);
            } else {
                result.push_back(static_cast<char>(byte));
            }
        }
    }
    result.push_back('"');
    return result;
}

core::Result<std::vector<core::json::Member>> object(std::string_view value,
    std::string_view label) {
    auto parsed = core::json::parse_object(value);
    if (!parsed) {
        return core::Result<std::vector<core::json::Member>>::failure(
            protocol(std::string(label) + " response is malformed"));
    }
    return core::Result<std::vector<core::json::Member>>::success(
        std::move(parsed).value());
}

core::Result<std::string> string_field(
    const std::vector<core::json::Member>& members, std::string_view name) {
    const auto* member = core::json::find_member(members, name);
    if (member == nullptr) {
        return core::Result<std::string>::failure(protocol(
            "runtime response is missing a required field"));
    }
    auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded || decoded.value().empty()) {
        return core::Result<std::string>::failure(protocol(
            "runtime response contains an invalid string field"));
    }
    return decoded;
}

std::string response_text(const Response& response) {
    return std::string(reinterpret_cast<const char*>(response.body.data()),
        response.body.size());
}

const std::string* response_header(const Response& response, std::string_view name) {
    const auto found = std::find_if(response.headers.begin(), response.headers.end(),
        [name](const auto& header) {
            if (header.first.size() != name.size()) return false;
            return std::equal(header.first.begin(), header.first.end(), name.begin(),
                [](unsigned char left, unsigned char right) {
                    return std::tolower(left) == std::tolower(right);
                });
        });
    return found == response.headers.end() ? nullptr : &found->second;
}

core::Result<void> validate_json_response(
    const Response& response, std::string_view label) {
    if (response.status < 200U || response.status >= 300U ||
        response.body.empty() || response.body.size() > kMaxResponseBytes) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error,
                std::string(label) + " request failed (HTTP " +
                std::to_string(response.status) + ")"));
    }
    const std::string text = response_text(response);
    auto parsed = core::json::parse_object(text);
    if (!parsed) {
        return core::Result<void>::failure(protocol(
            std::string(label) + " response is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<JobReceipt> parse_job(const Response& response,
    std::string source_identity, core::Revision source_revision,
    std::string organization_scope) {
    if (auto valid = validate_json_response(response, "runtime job"); !valid) {
        return core::Result<JobReceipt>::failure(valid.error());
    }
    const std::string payload = response_text(response);
    auto root = object(payload, "runtime job");
    if (!root) return core::Result<JobReceipt>::failure(root.error());
    const auto* job_member = core::json::find_member(root.value(), "job");
    if (job_member == nullptr) {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime response does not contain a job"));
    }
    auto job = object(job_member->raw_value, "runtime job");
    if (!job) return core::Result<JobReceipt>::failure(job.error());
    auto job_id = string_field(job.value(), "job_id");
    auto status = string_field(job.value(), "status");
    if (!job_id || !status || !opaque_id(job_id.value())) {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job identity or status is invalid"));
    }
    if (status.value() != "queued" && status.value() != "running" &&
        status.value() != "succeeded" && status.value() != "failed" &&
        status.value() != "cancel_requested" && status.value() != "cancelled") {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job status is not recognized"));
    }
    const auto* request_member = core::json::find_member(job.value(), "request");
    if (request_member == nullptr) {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job is missing its request binding"));
    }
    auto request = object(request_member->raw_value, "runtime request");
    if (!request) return core::Result<JobReceipt>::failure(request.error());
    auto operation = string_field(request.value(), "operation");
    auto revision = string_field(request.value(), "source_revision");
    if (!operation || operation.value() != "carto.botanical.generate.v1" || !revision ||
        revision.value() != std::to_string(source_revision.value())) {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job does not preserve the selected source revision and operation"));
    }
    const auto* input_member = core::json::find_member(request.value(), "input");
    if (input_member == nullptr) {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job is missing its input artifact binding"));
    }
    auto input = object(input_member->raw_value, "runtime input");
    if (!input) return core::Result<JobReceipt>::failure(input.error());
    auto input_id = string_field(input.value(), "artifact_id");
    auto input_digest_text = string_field(input.value(), "sha256");
    auto input_kind = string_field(input.value(), "kind");
    if (!input_id || !input_digest_text || !input_kind || !opaque_id(input_id.value()) ||
        input_kind.value() != "botanical-source-v1") {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job input artifact binding is invalid"));
    }
    auto input_digest = assets::Sha256Digest::from_hex(input_digest_text.value());
    if (!input_digest || input_digest.value().is_zero()) {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job input digest is invalid"));
    }
    JobReceipt result{job_id.value(), status.value(), std::move(source_identity),
        source_revision, std::move(input_id).value(), input_digest.value(), {}, {}, {},
        std::move(organization_scope)};
    if (status.value() == "succeeded") {
        const auto* output_member = core::json::find_member(job.value(), "output");
        if (output_member == nullptr) {
            return core::Result<JobReceipt>::failure(protocol(
                "succeeded runtime job has no output reference"));
        }
        auto output = object(output_member->raw_value, "runtime output");
        if (!output) return core::Result<JobReceipt>::failure(output.error());
        auto artifact_id = string_field(output.value(), "artifact_id");
        auto digest_text = string_field(output.value(), "sha256");
        auto kind = string_field(output.value(), "kind");
        if (!artifact_id || !digest_text || !kind || !opaque_id(artifact_id.value())) {
            return core::Result<JobReceipt>::failure(protocol(
                "runtime output reference is invalid"));
        }
        auto digest = assets::Sha256Digest::from_hex(digest_text.value());
        if (!digest || digest.value().is_zero() ||
            (kind.value() != kBotanicalGltfOutputKind &&
             kind.value() != kBotanicalGlbOutputKind)) {
            return core::Result<JobReceipt>::failure(protocol(
                "runtime output digest or kind is not approved"));
        }
        result.output_artifact_id = std::move(artifact_id).value();
        result.output_digest = digest.value();
        result.output_kind = std::move(kind).value();
    }
    return core::Result<JobReceipt>::success(std::move(result));
}

Request authenticated_request(const Config& config, std::string method,
    std::string path, std::string content_type, std::vector<std::uint8_t> body) {
    Request request{std::move(method), std::move(path), std::move(content_type), {},
        std::move(body)};
    request.headers.emplace_back("x-functions-key", config.function_key);
    request.headers.emplace_back("Authorization", "Bearer " + config.bearer_token);
    request.headers.emplace_back("x-cartographer-org", config.organization);
    return request;
}

} // namespace

core::Result<void> Config::validate() const {
    constexpr std::string_view https = "https://";
    if (!base_url.starts_with(https) || base_url.size() <= https.size() ||
        base_url.find_first_of("?#") != std::string::npos ||
        base_url.find('/', https.size()) != std::string::npos ||
        base_url.find('\\') != std::string::npos ||
        base_url.find('@', https.size()) != std::string::npos ||
        base_url.find_first_of("% \t\r\n", https.size()) != std::string::npos ||
        function_key.empty() || function_key.size() > 4096U ||
        bearer_token.empty() || bearer_token.size() > 4096U ||
        !opaque_id(organization)) {
        return core::Result<void>::failure(invalid(
            "private generation service is not configured with a valid HTTPS endpoint and credentials"));
    }
    return core::Result<void>::success();
}

Config Config::from_environment() {
    const auto read = [](const char* name) {
        const char* value = std::getenv(name);
        return value == nullptr ? std::string{} : std::string(value);
    };
    return Config{
        read("CARTOGRAPHER_RUNTIME_BASE_URL"),
        read("CARTOGRAPHER_RUNTIME_FUNCTION_KEY"),
        read("CARTOGRAPHER_RUNTIME_BEARER_TOKEN"),
        read("CARTOGRAPHER_RUNTIME_ORG")};
}

Client::Client(Config config, std::shared_ptr<Transport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

core::Result<JobReceipt> Client::submit_botanical_source(
    std::string_view source_identity,
    core::Revision source_revision,
    std::string_view serialized_source,
    std::string_view justification,
    std::string_view request_id) {
    if (auto valid = config_.validate(); !valid) {
        return core::Result<JobReceipt>::failure(valid.error());
    }
    if (!transport_) {
        return core::Result<JobReceipt>::failure(Diagnostic(
            ErrorCode::unsupported, "private generation service transport is unavailable"));
    }
    if (!opaque_id(source_identity) || source_revision.value() == 0U ||
        serialized_source.empty() || serialized_source.size() > kMaxSourceBytes ||
        justification.empty() || justification.size() > 1024U ||
        justification.find_first_not_of(" \t\r\n") == std::string_view::npos ||
        !opaque_id(request_id)) {
        return core::Result<JobReceipt>::failure(invalid(
            "an explicit justification and opaque request identity are required"));
    }
    const auto decoded_source = botanical::Source::deserialize(serialized_source);
    if (!decoded_source || decoded_source.value().identity != source_identity ||
        decoded_source.value().revision != source_revision) {
        return core::Result<JobReceipt>::failure(invalid(
            "serialized botanical source must match its exact submitted identity and revision"));
    }
    const std::string source_text(serialized_source);
    const auto* source_begin = reinterpret_cast<const std::uint8_t*>(source_text.data());
    const std::span<const std::uint8_t> source_bytes(source_begin, source_text.size());
    const auto digest = assets::sha256(source_bytes);

    Request upload = authenticated_request(config_, "POST",
        std::string(kApiPrefix) + "/artifacts", "application/octet-stream",
        std::vector<std::uint8_t>(source_bytes.begin(), source_bytes.end()));
    upload.headers.emplace_back("x-cartographer-content-sha256", digest.hex());
    upload.headers.emplace_back("x-cartographer-artifact-kind", "botanical-source-v1");
    upload.headers.emplace_back("x-cartographer-request-id", std::string(request_id) + ".input");
    auto uploaded = transport_->send(config_, upload);
    if (!uploaded) return core::Result<JobReceipt>::failure(uploaded.error());
    if (auto valid = validate_json_response(uploaded.value(), "source upload"); !valid) {
        return core::Result<JobReceipt>::failure(valid.error());
    }
    const std::string upload_payload = response_text(uploaded.value());
    auto upload_root = object(upload_payload, "source upload");
    if (!upload_root) return core::Result<JobReceipt>::failure(upload_root.error());
    const auto* artifact_member = core::json::find_member(upload_root.value(), "artifact");
    if (artifact_member == nullptr) {
        return core::Result<JobReceipt>::failure(protocol(
            "source upload response is missing its artifact receipt"));
    }
    auto artifact = object(artifact_member->raw_value, "source upload");
    if (!artifact) return core::Result<JobReceipt>::failure(artifact.error());
    auto artifact_id = string_field(artifact.value(), "artifact_id");
    auto artifact_digest = string_field(artifact.value(), "sha256");
    auto artifact_kind = string_field(artifact.value(), "kind");
    if (!artifact_id || !artifact_digest || !artifact_kind ||
        !opaque_id(artifact_id.value()) || artifact_digest.value() != digest.hex() ||
        artifact_kind.value() != "botanical-source-v1") {
        return core::Result<JobReceipt>::failure(protocol(
            "source upload receipt does not match the selected source"));
    }

    const std::string job_body = "{\"request_id\":" + json_string(request_id) +
        ",\"operation\":\"carto.botanical.generate.v1\",\"input\":{\"artifact_id\":" +
        json_string(artifact_id.value()) + ",\"sha256\":" + json_string(digest.hex()) +
        ",\"kind\":\"botanical-source-v1\"},\"source_revision\":" +
        json_string(std::to_string(source_revision.value())) +
        ",\"justification\":" + json_string(justification) + "}";
    const auto* job_begin = reinterpret_cast<const std::uint8_t*>(job_body.data());
    Request create_job = authenticated_request(config_, "POST",
        std::string(kApiPrefix) + "/jobs", "application/json; charset=utf-8",
        std::vector<std::uint8_t>(job_begin, job_begin + job_body.size()));
    auto created = transport_->send(config_, create_job);
    if (!created) return core::Result<JobReceipt>::failure(created.error());
    auto receipt = parse_job(created.value(), std::string(source_identity), source_revision,
        config_.organization);
    if (!receipt) return receipt;
    if (receipt.value().source_artifact_id != artifact_id.value() ||
        receipt.value().source_digest != digest) {
        return core::Result<JobReceipt>::failure(protocol(
            "created job is not bound to the exact uploaded source artifact"));
    }
    // A retry with the same request ID can recover an already-terminal job
    // after the original create response was lost. The server's idempotent
    // receipt is still authoritative as long as the exact source binding
    // above matches; the caller must inspect status before download/promotion.
    return receipt;
}

core::Result<JobReceipt> Client::get_job(const JobReceipt& submitted_job) {
    if (auto valid = config_.validate(); !valid) {
        return core::Result<JobReceipt>::failure(valid.error());
    }
    if (!transport_ || !opaque_id(submitted_job.job_id) ||
        !opaque_id(submitted_job.source_identity) ||
        !opaque_id(submitted_job.source_artifact_id) ||
        submitted_job.source_revision.value() == 0U || submitted_job.source_digest.is_zero() ||
        submitted_job.organization_scope != config_.organization) {
        return core::Result<JobReceipt>::failure(invalid(
            "private generation job identity or transport is invalid"));
    }
    Request request = authenticated_request(config_, "GET",
        std::string(kApiPrefix) + "/jobs/" + submitted_job.job_id,
        "application/json", {});
    auto response = transport_->send(config_, request);
    if (!response) return core::Result<JobReceipt>::failure(response.error());
    auto receipt = parse_job(response.value(), submitted_job.source_identity,
        submitted_job.source_revision, config_.organization);
    if (!receipt) return receipt;
    if (receipt.value().job_id != submitted_job.job_id ||
        receipt.value().source_artifact_id != submitted_job.source_artifact_id ||
        receipt.value().source_digest != submitted_job.source_digest) {
        return core::Result<JobReceipt>::failure(protocol(
            "runtime job response does not match the submitted source receipt"));
    }
    return receipt;
}

core::Result<std::vector<std::uint8_t>> Client::download_output(const JobReceipt& job) {
    if (auto valid = config_.validate(); !valid) {
        return core::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    if (!transport_ || job.organization_scope != config_.organization ||
        job.status != "succeeded" || !opaque_id(job.output_artifact_id) ||
        job.output_digest.is_zero() ||
        (job.output_kind != kBotanicalGltfOutputKind &&
         job.output_kind != kBotanicalGlbOutputKind)) {
        return core::Result<std::vector<std::uint8_t>>::failure(invalid(
            "only a succeeded job with a bound botanical interchange artifact can be downloaded"));
    }
    Request request = authenticated_request(config_, "GET",
        std::string(kApiPrefix) + "/artifacts/" + job.output_artifact_id,
        "application/octet-stream", {});
    auto response = transport_->send(config_, request);
    if (!response) return core::Result<std::vector<std::uint8_t>>::failure(response.error());
    if (response.value().status < 200U || response.value().status >= 300U ||
        response.value().body.empty() || response.value().body.size() > kMaxResponseBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(Diagnostic(
            ErrorCode::io_error, "private generation artifact download failed"));
    }
    const auto* hash_header = response_header(response.value(),
        "x-cartographer-content-sha256");
    const auto* kind_header = response_header(response.value(),
        "x-cartographer-artifact-kind");
    const auto actual = assets::sha256(response.value().body);
    if (hash_header == nullptr || kind_header == nullptr ||
        *hash_header != job.output_digest.hex() || *kind_header != job.output_kind ||
        actual != job.output_digest) {
        return core::Result<std::vector<std::uint8_t>>::failure(protocol(
            "downloaded output does not match the job-bound artifact receipt"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(
        std::move(response.value().body));
}

} // namespace carto::runtime_api
