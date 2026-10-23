#include <carto/biological/provider_contract.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

namespace {

using namespace carto;
using namespace carto::biological;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

AuthoringSourceIdentity source(std::uint64_t revision, std::string_view content) {
    return AuthoringSourceIdentity{
        "fixtures/biology", 91U, core::Revision{revision}, digest(content)};
}

BiologicalProviderRequest request_fixture() {
    return BiologicalProviderRequest{
        source(3U, "source-r3"), "field.sample", digest("input-set"), digest("parameters")};
}

BiologicalProviderResult result_for(const BiologicalProviderRequest& request) {
    return BiologicalProviderResult{
        request.source, request.digest(), digest("provider-output"), 15U};
}

std::span<const std::uint8_t> bytes(std::string_view text) {
    return std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void exact_source_request_and_budget_are_admitted_read_only() {
    const auto request = request_fixture();
    const auto result = result_for(request);
    require(static_cast<bool>(request.validate()), "valid provider request passes");
    require(request.digest() == request_fixture().digest(), "request digest is deterministic");
    require(static_cast<bool>(validate_biological_provider_result(
                request.source, request, result, 15U)),
            "exact source, request, and output budget are admitted");
    require(static_cast<bool>(validate_biological_provider_artifact(
                result, bytes("provider-output"), 15U)),
            "artifact bytes match declared result digest and size");
    require(!validate_biological_provider_result(request.source, request, result, 14U),
            "output exceeding caller budget is refused");
    require(!validate_biological_provider_result(
                request.source, request, result, BiologicalProviderResult::kMaxOutputBytes + 1U),
            "caller cannot expand output budget past hard bound");
}

void stale_source_revisions_and_digests_are_refused() {
    const auto request = request_fixture();
    const auto result = result_for(request);
    const auto stale_revision = validate_biological_provider_result(
        source(4U, "source-r4"), request, result, 64U);
    require(!stale_revision && stale_revision.error().code == core::ErrorCode::stale_data,
            "changed current source revision and digest refuse the old result");
    const auto stale_digest = validate_biological_provider_result(
        source(3U, "different-source-r3"), request, result, 64U);
    require(!stale_digest && stale_digest.error().code == core::ErrorCode::stale_data,
            "same revision with changed source digest is stale");
}

void mismatched_request_and_result_bindings_are_refused() {
    const auto request = request_fixture();
    auto changed_request = request;
    changed_request.parameters_digest = digest("changed-parameters");
    const auto changed_inputs = validate_biological_provider_result(
        request.source, changed_request, result_for(request), 64U);
    require(!changed_inputs && changed_inputs.error().code == core::ErrorCode::stale_data,
            "result for old parameters cannot answer a changed request");

    auto wrong_request_digest = result_for(request);
    wrong_request_digest.request_digest = digest("another-request");
    const auto mismatched_request = validate_biological_provider_result(
        request.source, request, wrong_request_digest, 64U);
    require(!mismatched_request &&
                mismatched_request.error().code == core::ErrorCode::stale_data,
            "mismatched request digest is refused");

    auto wrong_result_source = result_for(request);
    wrong_result_source.source = source(3U, "different-source-r3");
    const auto mismatched_source = validate_biological_provider_result(
        request.source, request, wrong_result_source, 64U);
    require(!mismatched_source && mismatched_source.error().code == core::ErrorCode::stale_data,
            "mismatched result source identity is refused");
}

void malformed_operations_and_results_fail_closed() {
    auto malformed_request = request_fixture();
    malformed_request.operation = "field.sample\nwrite";
    require(!malformed_request.validate(), "operation control bytes are rejected");

    const auto request = request_fixture();
    auto empty_result = result_for(request);
    empty_result.output_digest = {};
    require(!validate_biological_provider_result(
                request.source, request, empty_result, 64U),
            "unbound output digest is rejected");

    auto oversized_result = result_for(request);
    oversized_result.output_bytes = BiologicalProviderResult::kMaxOutputBytes + 1U;
    require(!validate_biological_provider_result(
                request.source, request, oversized_result,
                BiologicalProviderResult::kMaxOutputBytes),
            "hard output size bound is enforced");

    const auto valid_result = result_for(request);
    require(!validate_biological_provider_artifact(
                valid_result, bytes("provider-outpuX"), valid_result.output_bytes),
            "artifact with a mismatched content digest is refused");
    require(!validate_biological_provider_artifact(
                valid_result, bytes("provider-output-extra"),
                BiologicalProviderResult::kMaxOutputBytes),
            "artifact size mismatch is refused");
    require(!validate_biological_provider_artifact(
                valid_result, bytes("provider-output"), valid_result.output_bytes - 1U),
            "artifact exceeding caller budget is refused");
}

} // namespace

int main() {
    exact_source_request_and_budget_are_admitted_read_only();
    stale_source_revisions_and_digests_are_refused();
    mismatched_request_and_result_bindings_are_refused();
    malformed_operations_and_results_fail_closed();
    std::cout << "PASS biological provider request/result contract boundaries\n";
    return EXIT_SUCCESS;
}
