#include <carto/production/closure.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace carto::production {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t max_bytes) {
    return !value.empty() && value.size() <= max_bytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
                byte != '\r' && byte != '\n';
        });
}

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool unit_interval(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool valid_artifact_kind(ArtifactKind kind) {
    switch (kind) {
    case ArtifactKind::render_mesh:
    case ArtifactKind::material_package:
    case ArtifactKind::residency_page:
    case ArtifactKind::publication_package:
    case ArtifactKind::evidence_bundle:
        return true;
    }
    return false;
}

bool valid_execution_form(ExecutionForm form) {
    switch (form) {
    case ExecutionForm::interactive:
    case ExecutionForm::cached:
    case ExecutionForm::offline:
    case ExecutionForm::learned_proposal:
    case ExecutionForm::temporal:
        return true;
    }
    return false;
}

bool valid_receipt_status(ReceiptStatus status) {
    switch (status) {
    case ReceiptStatus::admitted:
    case ReceiptStatus::failed:
    case ReceiptStatus::stale:
    case ReceiptStatus::cancelled:
        return true;
    }
    return false;
}

std::optional<ArtifactKind> artifact_kind_from_name(std::string_view name) {
    for (const auto kind : {ArtifactKind::render_mesh,
                            ArtifactKind::material_package,
                            ArtifactKind::residency_page,
                            ArtifactKind::publication_package,
                            ArtifactKind::evidence_bundle}) {
        if (name == artifact_kind_name(kind)) return kind;
    }
    return std::nullopt;
}

std::optional<ExecutionForm> execution_form_from_name(std::string_view name) {
    for (const auto form : {ExecutionForm::interactive,
                            ExecutionForm::cached,
                            ExecutionForm::offline,
                            ExecutionForm::learned_proposal,
                            ExecutionForm::temporal}) {
        if (name == execution_form_name(form)) return form;
    }
    return std::nullopt;
}

std::optional<ReceiptStatus> receipt_status_from_name(std::string_view name) {
    for (const auto status : {ReceiptStatus::admitted,
                              ReceiptStatus::failed,
                              ReceiptStatus::stale,
                              ReceiptStatus::cancelled}) {
        if (name == receipt_status_name(status)) return status;
    }
    return std::nullopt;
}

core::Result<assets::Sha256Digest> read_digest(
    std::istream& input,
    std::string_view field) {
    std::string encoded;
    if (!(input >> encoded)) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "production ledger is missing " + std::string(field)));
    }
    const auto digest = assets::Sha256Digest::from_hex(encoded);
    if (!digest) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "production ledger contains an invalid " + std::string(field)));
    }
    return digest;
}

core::Result<void> malformed_ledger(std::string message) {
    return core::Result<void>::failure(validation(std::move(message)));
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

} // namespace

const char* artifact_kind_name(ArtifactKind kind) noexcept {
    switch (kind) {
    case ArtifactKind::render_mesh: return "render-mesh";
    case ArtifactKind::material_package: return "material-package";
    case ArtifactKind::residency_page: return "residency-page";
    case ArtifactKind::publication_package: return "publication-package";
    case ArtifactKind::evidence_bundle: return "evidence-bundle";
    }
    return "unknown";
}

const char* execution_form_name(ExecutionForm form) noexcept {
    switch (form) {
    case ExecutionForm::interactive: return "interactive";
    case ExecutionForm::cached: return "cached";
    case ExecutionForm::offline: return "offline";
    case ExecutionForm::learned_proposal: return "learned-proposal";
    case ExecutionForm::temporal: return "temporal";
    }
    return "unknown";
}

const char* receipt_status_name(ReceiptStatus status) noexcept {
    switch (status) {
    case ReceiptStatus::admitted: return "admitted";
    case ReceiptStatus::failed: return "failed";
    case ReceiptStatus::stale: return "stale";
    case ReceiptStatus::cancelled: return "cancelled";
    }
    return "unknown";
}

core::Result<void> AuthoritativeSource::validate() const {
    if (!safe_text(identity, 256U)) {
        return core::Result<void>::failure(invalid(
            "production source identity is empty, unsafe, or too long"));
    }
    if (revision.exhausted() || content_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "production source requires a non-exhausted revision and content digest"));
    }
    return core::Result<void>::success();
}

std::string AuthoritativeSource::canonical() const {
    return "identity=" + identity + ";revision=" + std::to_string(revision.value()) +
        ";content=" + content_digest.hex();
}

core::Result<void> ProductionCookKey::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!valid_artifact_kind(kind) || !valid_execution_form(form)) {
        return core::Result<void>::failure(invalid("production cook key kind or form is invalid"));
    }
    if (!safe_text(algorithm, 128U) || settings_digest.is_zero() ||
        !safe_text(platform_profile, 128U)) {
        return core::Result<void>::failure(invalid(
            "production cook key requires safe algorithm/platform and settings digest"));
    }
    return core::Result<void>::success();
}

std::string ProductionCookKey::canonical() const {
    return "source{" + source.canonical() + "};kind=" + artifact_kind_name(kind) +
        ";form=" + execution_form_name(form) + ";algorithm=" + algorithm +
        ";settings=" + settings_digest.hex() + ";platform=" + platform_profile;
}

core::Result<void> QualityTelemetry::validate() const {
    if (!finite_nonnegative(measured_error) || !unit_interval(importance) ||
        !unit_interval(confidence)) {
        return core::Result<void>::failure(invalid(
            "production quality telemetry is non-finite or outside its bounds"));
    }
    return core::Result<void>::success();
}

std::string QualityTelemetry::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "error=" << measured_error
           << ";importance=" << importance << ";confidence=" << confidence;
    return output.str();
}

core::Result<void> CostTelemetry::validate() const {
    if (!finite_nonnegative(cook_milliseconds) ||
        !finite_nonnegative(cpu_milliseconds) ||
        !finite_nonnegative(gpu_milliseconds)) {
        return core::Result<void>::failure(invalid(
            "production cost telemetry is non-finite or negative"));
    }
    return core::Result<void>::success();
}

std::string CostTelemetry::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "cook-ms=" << cook_milliseconds
           << ";cpu-ms=" << cpu_milliseconds << ";gpu-ms=" << gpu_milliseconds
           << ";resident=" << resident_bytes << ";disk=" << disk_bytes;
    return output.str();
}

core::Result<void> ProductionArtifactReceipt::validate() const {
    if (auto result = key.validate(); !result) return result;
    if (!valid_receipt_status(status)) {
        return core::Result<void>::failure(invalid("production receipt status is invalid"));
    }
    if (auto result = quality.validate(); !result) return result;
    if (auto result = cost.validate(); !result) return result;
    if (!assets::Sha256Digest::from_hex(receipt_digest.hex())) {
        return core::Result<void>::failure(invalid("production receipt digest is invalid"));
    }
    const bool has_output = !output_digest.is_zero() && output_bytes != 0U;
    const bool has_no_output = output_digest.is_zero() && output_bytes == 0U;
    if (status == ReceiptStatus::admitted && !has_output) {
        return core::Result<void>::failure(validation(
            "admitted production receipts require a non-empty output"));
    }
    if (status != ReceiptStatus::admitted && !has_no_output) {
        return core::Result<void>::failure(validation(
            "failed, stale, or cancelled receipts may not publish output"));
    }
    if (receipt_digest.is_zero() ||
        receipt_digest != digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(validation(
            "production receipt digest does not match its canonical evidence"));
    }
    return core::Result<void>::success();
}

std::string ProductionArtifactReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << "production-receipt-v1{" << key.canonical() << "};status="
           << receipt_status_name(status) << ";output=" << output_digest.hex()
           << ";output-bytes=" << output_bytes << ";quality{" << quality.canonical()
           << "};cost{" << cost.canonical() << "}";
    return output.str();
}

core::Result<void> ProductionArtifactReceipt::refresh_digest() {
    if (auto result = key.validate(); !result) return result;
    if (!valid_receipt_status(status)) {
        return core::Result<void>::failure(invalid("production receipt status is invalid"));
    }
    if (auto result = quality.validate(); !result) return result;
    if (auto result = cost.validate(); !result) return result;
    const bool has_output = !output_digest.is_zero() && output_bytes != 0U;
    const bool has_no_output = output_digest.is_zero() && output_bytes == 0U;
    if ((status == ReceiptStatus::admitted && !has_output) ||
        (status != ReceiptStatus::admitted && !has_no_output)) {
        return core::Result<void>::failure(validation(
            "production receipt output does not match its status"));
    }
    receipt_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<void> AdmissionPolicy::validate() const {
    if (max_measured_error.has_value() &&
        !finite_nonnegative(max_measured_error.value())) {
        return core::Result<void>::failure(invalid(
            "production admission error budget is invalid"));
    }
    if (!unit_interval(min_confidence)) {
        return core::Result<void>::failure(invalid(
            "production admission confidence budget is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_publication(
    const ProductionArtifactReceipt& receipt,
    const AuthoritativeSource& current_source,
    const AdmissionPolicy& policy) {
    if (auto result = receipt.validate(); !result) return result;
    if (auto result = current_source.validate(); !result) return result;
    if (auto result = policy.validate(); !result) return result;
    if (receipt.status != ReceiptStatus::admitted) {
        return core::Result<void>::failure(validation(
            "only admitted production receipts may be published"));
    }
    if (receipt.key.source != current_source) {
        return core::Result<void>::failure(stale(
            "production receipt source revision or digest is stale"));
    }
    if (policy.max_measured_error.has_value() &&
        receipt.quality.measured_error > policy.max_measured_error.value()) {
        return core::Result<void>::failure(validation(
            "production receipt exceeds the measured-error admission budget"));
    }
    if (receipt.quality.confidence < policy.min_confidence) {
        return core::Result<void>::failure(validation(
            "production receipt confidence is below the admission budget"));
    }
    if (policy.max_resident_bytes.has_value() &&
        receipt.cost.resident_bytes > policy.max_resident_bytes.value()) {
        return core::Result<void>::failure(validation(
            "production receipt exceeds the resident-memory admission budget"));
    }
    return core::Result<void>::success();
}

core::Result<void> ProductionReceiptLedger::append(ProductionArtifactReceipt receipt) {
    if (records_.size() >= max_receipts) {
        return core::Result<void>::failure(validation(
            "production receipt ledger has reached its bounded capacity"));
    }
    if (auto result = receipt.validate(); !result) return result;
    records_.push_back(std::move(receipt));
    return core::Result<void>::success();
}

core::Result<std::optional<ProductionArtifactReceipt>> ProductionReceiptLedger::query(
    const ProductionCookKey& key) const {
    if (auto result = key.validate(); !result) {
        return core::Result<std::optional<ProductionArtifactReceipt>>::failure(result.error());
    }
    for (auto iterator = records_.rbegin(); iterator != records_.rend(); ++iterator) {
        if (iterator->key == key) {
            if (iterator->status != ReceiptStatus::admitted) {
                return core::Result<std::optional<ProductionArtifactReceipt>>::success(std::nullopt);
            }
            return core::Result<std::optional<ProductionArtifactReceipt>>::success(*iterator);
        }
    }
    return core::Result<std::optional<ProductionArtifactReceipt>>::success(std::nullopt);
}

core::Result<void> ProductionReceiptLedger::validate() const {
    if (records_.size() > max_receipts) {
        return core::Result<void>::failure(validation(
            "production receipt ledger exceeds its bounded capacity"));
    }
    for (const auto& record : records_) {
        if (auto result = record.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string ProductionReceiptLedger::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_PRODUCTION_LEDGER_V1\n"
           << "records " << records_.size() << '\n';
    output << std::setprecision(17);
    for (const auto& record : records_) {
        const auto& key = record.key;
        output << "record " << std::quoted(key.source.identity) << ' '
               << key.source.revision.value() << ' '
               << key.source.content_digest.hex() << ' '
               << artifact_kind_name(key.kind) << ' '
               << execution_form_name(key.form) << ' '
               << std::quoted(key.algorithm) << ' '
               << key.settings_digest.hex() << ' '
               << std::quoted(key.platform_profile) << ' '
               << receipt_status_name(record.status) << ' '
               << record.output_digest.hex() << ' ' << record.output_bytes << ' '
               << record.quality.measured_error << ' '
               << record.quality.importance << ' '
               << record.quality.confidence << ' '
               << record.cost.cook_milliseconds << ' '
               << record.cost.cpu_milliseconds << ' '
               << record.cost.gpu_milliseconds << ' '
               << record.cost.resident_bytes << ' '
               << record.cost.disk_bytes << ' '
               << record.receipt_digest.hex() << '\n';
    }
    return output.str();
}

core::Result<ProductionReceiptLedger> ProductionReceiptLedger::deserialize(
    std::string_view text) {
    if (text.size() > max_serialized_bytes) {
        return core::Result<ProductionReceiptLedger>::failure(validation(
            "production ledger serialization exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != "CARTOGRAPHER_PRODUCTION_LEDGER_V1") {
        return core::Result<ProductionReceiptLedger>::failure(validation(
            "production ledger magic or version is invalid"));
    }
    std::string records_token;
    std::uint64_t encoded_count = 0U;
    if (!(input >> records_token >> encoded_count) || records_token != "records" ||
        encoded_count > max_receipts) {
        return core::Result<ProductionReceiptLedger>::failure(validation(
            "production ledger record count is invalid or exceeds its bound"));
    }

    ProductionReceiptLedger ledger;
    for (std::uint64_t index = 0U; index < encoded_count; ++index) {
        std::string record_token;
        if (!(input >> record_token) || record_token != "record") {
            return core::Result<ProductionReceiptLedger>::failure(validation(
                "production ledger record marker is invalid"));
        }

        ProductionArtifactReceipt record;
        std::string kind_name_value;
        std::string form_name_value;
        std::string status_name_value;
        std::uint64_t source_revision = 0U;
        if (!(input >> std::quoted(record.key.source.identity) >> source_revision)) {
            return core::Result<ProductionReceiptLedger>::failure(validation(
                "production ledger source identity or revision is invalid"));
        }
        const auto source_digest = read_digest(input, "source digest");
        if (!source_digest) {
            return core::Result<ProductionReceiptLedger>::failure(source_digest.error());
        }
        if (!(input >> kind_name_value >> form_name_value >> std::quoted(record.key.algorithm))) {
            return core::Result<ProductionReceiptLedger>::failure(validation(
                "production ledger key kind, form, or algorithm is invalid"));
        }
        const auto kind = artifact_kind_from_name(kind_name_value);
        const auto form = execution_form_from_name(form_name_value);
        if (!kind.has_value() || !form.has_value()) {
            return core::Result<ProductionReceiptLedger>::failure(validation(
                "production ledger key kind or form is unsupported"));
        }
        const auto settings_digest = read_digest(input, "settings digest");
        if (!settings_digest) {
            return core::Result<ProductionReceiptLedger>::failure(settings_digest.error());
        }
        if (!(input >> std::quoted(record.key.platform_profile) >> status_name_value)) {
            return core::Result<ProductionReceiptLedger>::failure(validation(
                "production ledger platform or status is invalid"));
        }
        const auto status = receipt_status_from_name(status_name_value);
        if (!status.has_value()) {
            return core::Result<ProductionReceiptLedger>::failure(validation(
                "production ledger status is unsupported"));
        }
        const auto output_digest = read_digest(input, "output digest");
        if (!output_digest) {
            return core::Result<ProductionReceiptLedger>::failure(output_digest.error());
        }
        if (!(input >> record.output_bytes >> record.quality.measured_error >>
              record.quality.importance >> record.quality.confidence >>
              record.cost.cook_milliseconds >> record.cost.cpu_milliseconds >>
              record.cost.gpu_milliseconds >> record.cost.resident_bytes >>
              record.cost.disk_bytes)) {
            return core::Result<ProductionReceiptLedger>::failure(validation(
                "production ledger telemetry is malformed"));
        }
        const auto receipt_digest = read_digest(input, "receipt digest");
        if (!receipt_digest) {
            return core::Result<ProductionReceiptLedger>::failure(receipt_digest.error());
        }

        record.key.source.revision = core::Revision{source_revision};
        record.key.source.content_digest = source_digest.value();
        record.key.kind = kind.value();
        record.key.form = form.value();
        record.key.settings_digest = settings_digest.value();
        record.status = status.value();
        record.output_digest = output_digest.value();
        record.receipt_digest = receipt_digest.value();
        if (auto result = ledger.append(std::move(record)); !result) {
            return core::Result<ProductionReceiptLedger>::failure(result.error());
        }
    }

    input >> std::ws;
    if (!input.eof()) {
        return core::Result<ProductionReceiptLedger>::failure(
            malformed_ledger("production ledger contains trailing data").error());
    }
    return core::Result<ProductionReceiptLedger>::success(std::move(ledger));
}

} // namespace carto::production
