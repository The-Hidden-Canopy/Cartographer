#include <carto/runtime_api/client.hpp>
#include <carto/botanical/source.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using carto::assets::sha256;
using carto::runtime_api::Client;
using carto::runtime_api::Config;
using carto::runtime_api::Request;
using carto::runtime_api::Response;
using carto::runtime_api::Transport;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string as_text(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string header_value(const Request& request, std::string_view name) {
    for (const auto& [key, value] : request.headers) {
        if (key == name) return value;
    }
    return {};
}

class FakeTransport final : public Transport {
public:
    std::vector<Request> requests;
    std::string input_digest;
    std::string output_digest;
    std::vector<std::uint8_t> output_bytes = bytes("fixed glTF fixture");
    bool alter_revision = false;
    bool alter_download_hash = false;
    std::string created_status = "queued";

    carto::core::Result<Response> send(const Config&, const Request& request) override {
        requests.push_back(request);
        Response response;
        response.status = 200U;
        if (request.path.ends_with("/artifacts") && request.method == "POST") {
            input_digest = header_value(request, "x-cartographer-content-sha256");
            response.body = bytes("{\"artifact\":{\"artifact_id\":\"artifact-in-1\",\"sha256\":\"" +
                input_digest + "\",\"kind\":\"botanical-source-v1\"},\"size_bytes\":4}");
        } else if (request.path.ends_with("/jobs") && request.method == "POST") {
            const std::string revision = alter_revision ? "2" : "1";
            response.body = bytes("{\"job\":{\"job_id\":\"job-1\",\"status\":\"" + created_status +
                "\",\"request\":{\"operation\":\"carto.botanical.generate.v1\",\"source_revision\":\"" +
                revision + "\",\"input\":{\"artifact_id\":\"artifact-in-1\",\"sha256\":\"" +
                input_digest + "\",\"kind\":\"botanical-source-v1\"}}}}");
        } else if (request.path.ends_with("/jobs/job-1")) {
            output_digest = sha256(output_bytes).hex();
            response.body = bytes("{\"job\":{\"job_id\":\"job-1\",\"status\":\"succeeded\",\"request\":{\"operation\":\"carto.botanical.generate.v1\",\"source_revision\":\"1\",\"input\":{\"artifact_id\":\"artifact-in-1\",\"sha256\":\"" +
                input_digest + "\",\"kind\":\"botanical-source-v1\"}},\"output\":{\"artifact_id\":\"artifact-out-1\",\"sha256\":\"" +
                output_digest + "\",\"kind\":\"gltf-v1\"}}}");
        } else if (request.path.ends_with("/artifacts/artifact-out-1")) {
            response.body = output_bytes;
            response.headers.emplace_back("x-cartographer-content-sha256",
                alter_download_hash ? std::string(64U, '0') : output_digest);
            response.headers.emplace_back("x-cartographer-artifact-kind", "gltf-v1");
        } else {
            return carto::core::Result<Response>::failure(carto::core::Diagnostic(
                carto::core::ErrorCode::not_found, "unexpected fake transport route"));
        }
        return carto::core::Result<Response>::success(std::move(response));
    }
};

Config config() {
    return Config{"https://hub.example.test", "function-key-fixture",
        "fixture-token", "hidden-canopy"};
}

struct FixtureSource {
    std::string identity;
    carto::core::Revision revision;
    std::string serialized;
};

FixtureSource source() {
    carto::botanical::Source data;
    data.identity = "botanical-fixture";
    data.revision = carto::core::Revision{1U};
    return FixtureSource{data.identity, data.revision, data.serialize()};
}

void construction_and_reading_do_not_send_project_data() {
    auto transport = std::make_shared<FakeTransport>();
    Client client(config(), transport);
    require(transport->requests.empty(),
        "constructing a client must not send source data");
    const auto fixture = source();
    const auto job = client.submit_botanical_source(fixture.identity, fixture.revision,
        fixture.serialized, "explicit generation test", "request-1");
    if (!job) std::cerr << "submit diagnostic: " << job.error().message << '\n';
    require(job && transport->requests.size() == 2U,
        "only explicit submit uploads one source and creates one job");
    require(transport->requests[0].path == "/api/cartographer/runtime/v1/artifacts" &&
        transport->requests[0].method == "POST" &&
        transport->requests[0].content_type == "application/octet-stream",
        "source is sent to the fixed artifact route as binary");
    require(header_value(transport->requests[0], "x-cartographer-artifact-kind") ==
        "botanical-source-v1", "source artifact kind is fixed");
    require(header_value(transport->requests[0], "x-cartographer-content-sha256") ==
        sha256(transport->requests[0].body).hex(),
        "source upload binds its exact digest");
    require(header_value(transport->requests[0], "Authorization") ==
        "Bearer fixture-token" &&
        header_value(transport->requests[0], "x-functions-key") == "function-key-fixture",
        "credentials are carried only in transport headers");
    require(header_value(transport->requests[0], "x-cartographer-org") ==
        "hidden-canopy" &&
        header_value(transport->requests[1], "x-cartographer-org") == "hidden-canopy",
        "every request carries the configured organization scope");
    const std::string job_body = as_text(transport->requests[1].body);
    require(job_body.find("explicit generation test") != std::string::npos &&
        job_body.find("function-key-fixture") == std::string::npos &&
        job_body.find("fixture-token") == std::string::npos,
        "job body includes justification but never copies credentials");
    require(job.value().status == "queued" && job.value().job_id == "job-1" &&
        job.value().source_revision.value() == 1U &&
        job.value().organization_scope == "hidden-canopy",
        "submission returns the exact accepted job and source revision");
}

void missing_configuration_and_justification_fail_closed() {
    auto transport = std::make_shared<FakeTransport>();
    const auto fixture = source();
    Client missing(Config{}, transport);
    require(!missing.submit_botanical_source(fixture.identity, fixture.revision,
            fixture.serialized, "why", "request-1") &&
        transport->requests.empty(), "missing credentials do not attempt network access");
    Client configured(config(), transport);
    require(!configured.submit_botanical_source(fixture.identity, fixture.revision,
            fixture.serialized, "  \n", "request-1") &&
        transport->requests.empty(), "blank justification does not upload project data");
    require(!configured.submit_botanical_source(fixture.identity, fixture.revision,
            fixture.serialized, "why", "../request") &&
        transport->requests.empty(), "path-shaped request identifiers are rejected");
    Config insecure = config();
    insecure.base_url = "http://hub.example.test";
    Client insecure_client(std::move(insecure), transport);
    require(!insecure_client.submit_botanical_source(fixture.identity, fixture.revision,
            fixture.serialized, "why", "request-2") &&
        transport->requests.empty(), "non-HTTPS endpoints are rejected before transport");
    Config credential_bearing_origin = config();
    credential_bearing_origin.base_url = "https://user:password@hub.example.test";
    Client credential_bearing_client(std::move(credential_bearing_origin), transport);
    require(!credential_bearing_client.submit_botanical_source(fixture.identity,
            fixture.revision, fixture.serialized, "why", "request-2b") &&
        transport->requests.empty(),
        "credential-bearing origins are rejected before source upload");
    require(!configured.submit_botanical_source(fixture.identity,
            carto::core::Revision{2U}, fixture.serialized, "why", "request-3") &&
        transport->requests.empty(), "payload identity/revision mismatch is rejected before upload");
    require(!configured.submit_botanical_source(fixture.identity, fixture.revision,
            "not a botanical source", "why", "request-4") &&
        transport->requests.empty(), "unparseable source payload is rejected before upload");
}

void portable_source_contract_round_trips_without_local_generation() {
    const auto fixture = source();
    const auto decoded = carto::botanical::Source::deserialize(fixture.serialized);
    require(decoded && decoded.value().identity == fixture.identity &&
        decoded.value().revision == fixture.revision &&
        decoded.value().serialize() == fixture.serialized,
        "portable source bytes round-trip canonically for the fixed runtime contract");
    auto invalid_source = decoded.value();
    invalid_source.recipe.growth_axis = {0.0, 1.0, 0.0};
    invalid_source.recipe.reference_axis = {0.0, 2.0, 0.0};
    require(!invalid_source.validate(), "parallel source axes fail validation");
}

void source_revision_and_output_digest_are_revalidated() {
    const auto fixture = source();
    auto altered = std::make_shared<FakeTransport>();
    altered->alter_revision = true;
    Client bad_revision(config(), altered);
    require(!bad_revision.submit_botanical_source(fixture.identity, fixture.revision,
            fixture.serialized, "why", "request-3"),
        "server response with a different source revision is rejected");

    auto transport = std::make_shared<FakeTransport>();
    Client client(config(), transport);
    auto submitted = client.submit_botanical_source(fixture.identity, fixture.revision,
        fixture.serialized, "why", "request-4");
    require(static_cast<bool>(submitted), "valid source can be submitted");
    auto succeeded = client.get_job(submitted.value());
    require(succeeded && succeeded.value().status == "succeeded" &&
        succeeded.value().output_kind == "gltf-v1" &&
        !succeeded.value().output_digest.is_zero(),
        "poll result exposes only the bound output contract");
    auto artifact = client.download_output(succeeded.value());
    require(artifact && artifact.value() == transport->output_bytes,
        "download verifies exact bytes against the succeeded job digest");
    for (const auto& request : transport->requests) {
        require(header_value(request, "x-cartographer-org") == "hidden-canopy",
            "poll and artifact requests preserve the configured organization scope");
    }

    Config other_organization = config();
    other_organization.organization = "another-organization";
    auto other_scope_transport = std::make_shared<FakeTransport>();
    Client other_scope_client(std::move(other_organization), other_scope_transport);
    require(!other_scope_client.get_job(submitted.value()) &&
        other_scope_transport->requests.empty(),
        "a job receipt cannot be polled from another organization scope");
    require(!other_scope_client.download_output(succeeded.value()) &&
        other_scope_transport->requests.empty(),
        "an output receipt cannot be downloaded from another organization scope");

    auto altered_hash = std::make_shared<FakeTransport>();
    Client altered_client(config(), altered_hash);
    auto other_job = altered_client.submit_botanical_source(fixture.identity,
        fixture.revision, fixture.serialized, "why", "request-5");
    auto other_succeeded = altered_client.get_job(other_job.value());
    altered_hash->alter_download_hash = true;
    require(!altered_client.download_output(other_succeeded.value()),
        "artifact download with a mismatched digest header is rejected");
}

void idempotent_submit_recovers_a_terminal_job_receipt() {
    const auto fixture = source();
    auto transport = std::make_shared<FakeTransport>();
    transport->created_status = "cancelled";
    Client client(config(), transport);
    const auto recovered = client.submit_botanical_source(fixture.identity,
        fixture.revision, fixture.serialized, "recover an idempotent request", "request-replay");
    require(recovered && recovered.value().status == "cancelled",
        "terminal idempotent replay returns its exact job receipt instead of losing it");
    require(transport->requests.size() == 2U,
        "recovered job remains scoped to one input upload and one fixed job submission");
}

} // namespace

int main() {
    construction_and_reading_do_not_send_project_data();
    missing_configuration_and_justification_fail_closed();
    portable_source_contract_round_trips_without_local_generation();
    source_revision_and_output_digest_are_revalidated();
    idempotent_submit_recovers_a_terminal_job_receipt();
    std::cout << "private runtime client tests passed\n";
    return 0;
}
