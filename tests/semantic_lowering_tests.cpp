#include <carto/assets/blob_store.hpp>
#include <carto/semantic/lowering.hpp>
#include <carto/semantic/sidecar.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

carto::semantic::SemanticSidecar fixture_sidecar() {
    using namespace carto::semantic;
    SemanticSidecar sidecar;
    SemanticExtensionRecord record;
    record.schema = SchemaIdentity{"condition.example", "shelter"};
    record.schema_version = 2U;
    record.payload = "{\"state\":\"intact\"}";
    record.payload_digest = digest(record.payload);
    require(static_cast<bool>(sidecar.insert_record(std::move(record))),
        "fixture record enters sidecar");
    require(static_cast<bool>(sidecar.refresh_digest()),
        "fixture sidecar digest refreshes");
    return sidecar;
}

carto::semantic::LoweredTagBinding binding(
    std::string authored,
    std::string lowered,
    std::string evidence) {
    carto::semantic::LoweredTagBinding result;
    result.authored_tag = std::move(authored);
    result.lowered_tag = std::move(lowered);
    result.transformation_evidence = std::move(evidence);
    return result;
}

void lowering_maps_bind_and_fail_closed() {
    using namespace carto::semantic;
    const auto sidecar = fixture_sidecar();
    const auto sidecar_digest = sidecar.canonical_digest();
    require(static_cast<bool>(sidecar_digest), "fixture sidecar digest resolves");

    TagLoweringMap lowering(sidecar_digest.value());
    require(static_cast<bool>(lowering.insert_binding(
                binding("flammable", "vanta.material.flammable",
                    "direct tag identity; no precision narrowing"))),
        "first lowering binding enters the map");
    require(static_cast<bool>(lowering.insert_binding(
                binding("wet", "vanta.surface.wet",
                    "authored weather overlay lowers to native surface state"))),
        "second lowering binding enters the map");
    require(static_cast<bool>(lowering.validate()),
        "digest-bound lowering map validates");

    const auto* found = lowering.find("flammable");
    require(found != nullptr && found->lowered_tag == "vanta.material.flammable" &&
                !found->transformation_evidence.empty(),
        "authored tag resolves to its lowered tag with evidence");
    const auto missing = lowering.binding_for("rusted");
    require(!missing, "unbound authored tag fails closed");
    require(missing.error().code == carto::core::ErrorCode::not_found,
        "unbound authored tag reports a precise not-found failure");

    require(!lowering.insert_binding(
                binding("flammable", "vanta.material.fire", "duplicate probe")),
        "duplicate authored tag binding is rejected");
    require(!TagLoweringMap{}.validate(),
        "an unbound lowering map fails closed");

    const std::string encoded = lowering.serialize();
    const auto reopened = TagLoweringMap::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "lowering map survives deterministic round trip");
    require(static_cast<bool>(reopened.value().verify_against(sidecar)),
        "reopened lowering map still binds the fixture sidecar digest");
}

void lowering_maps_are_digest_bound_and_deterministic() {
    using namespace carto::semantic;
    const auto sidecar = fixture_sidecar();
    const auto sidecar_digest = sidecar.canonical_digest();
    require(static_cast<bool>(sidecar_digest), "fixture sidecar digest resolves");

    TagLoweringMap forward(sidecar_digest.value());
    require(static_cast<bool>(forward.insert_binding(
                binding("flammable", "vanta.material.flammable", "identity"))),
        "forward map binds the first tag");
    require(static_cast<bool>(forward.insert_binding(
                binding("wet", "vanta.surface.wet", "identity"))),
        "forward map binds the second tag");
    TagLoweringMap reversed(sidecar_digest.value());
    require(static_cast<bool>(reversed.insert_binding(
                binding("wet", "vanta.surface.wet", "identity"))),
        "reversed map binds the second tag");
    require(static_cast<bool>(reversed.insert_binding(
                binding("flammable", "vanta.material.flammable", "identity"))),
        "reversed map binds the first tag");
    const auto forward_digest = forward.canonical_digest();
    const auto reversed_digest = reversed.canonical_digest();
    require(forward_digest && reversed_digest &&
                forward_digest.value() == reversed_digest.value(),
        "binding authoring order does not alter the lowering map digest");
    require(forward.serialize() == reversed.serialize(),
        "binding authoring order does not alter the canonical bytes");

    auto mutated = fixture_sidecar();
    SemanticExtensionRecord extra;
    extra.schema = SchemaIdentity{"wolves.example", "den"};
    extra.schema_version = 1U;
    extra.payload = "{\"occupied\":true}";
    extra.payload_digest = digest(extra.payload);
    require(static_cast<bool>(mutated.insert_record(std::move(extra))),
        "mutated sidecar gains a record");
    require(static_cast<bool>(mutated.refresh_digest()),
        "mutated sidecar refreshes its digest");
    require(mutated.digest() != sidecar_digest.value(),
        "mutation changes the sidecar digest");

    const auto drifted = forward.verify_against(mutated);
    require(!drifted && drifted.error().code == carto::core::ErrorCode::stale_data,
        "lowering map bound to a drifted sidecar fails closed as stale data");
    const TagLoweringMap foreign(digest("other-sidecar"));
    require(!foreign.verify_against(sidecar),
        "lowering map from a different sidecar fails closed");
}

void malformed_lowering_maps_fail_closed() {
    using namespace carto::semantic;
    const auto sidecar = fixture_sidecar();
    const auto sidecar_digest = sidecar.canonical_digest();
    require(static_cast<bool>(sidecar_digest), "fixture sidecar digest resolves");
    TagLoweringMap lowering(sidecar_digest.value());
    require(static_cast<bool>(lowering.insert_binding(
                binding("flammable", "vanta.material.flammable", "identity"))),
        "fixture binding enters the map");
    const std::string encoded = lowering.serialize();

    require(!TagLoweringMap::deserialize(encoded + "trailing"),
        "lowering parser rejects trailing unbound data");
    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_TAG_LOWERING 1"}.size(),
        "CARTOGRAPHER_TAG_LOWERING 99");
    require(!TagLoweringMap::deserialize(future),
        "unknown future lowering versions fail precisely");
    require(!TagLoweringMap::deserialize(
                "CARTOGRAPHER_TAG_LOWERING 1\n"
                "SIDECAR_DIGEST " + digest("x").hex() + "\nBINDINGS 65537\n"),
        "hostile binding counts are rejected before allocation");
}

} // namespace

int main() {
    lowering_maps_bind_and_fail_closed();
    lowering_maps_are_digest_bound_and_deterministic();
    malformed_lowering_maps_fail_closed();
    std::cout << "semantic lowering tests passed\n";
    return EXIT_SUCCESS;
}
