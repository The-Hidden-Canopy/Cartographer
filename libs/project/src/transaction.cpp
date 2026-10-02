#include <carto/project/transaction.hpp>

#include <algorithm>
#include <exception>
#include <span>
#include <utility>

namespace carto::project {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

bool safe_text(std::string_view value, std::size_t max_bytes) {
    return !value.empty() && value.size() <= max_bytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' && byte != '\n';
        });
}

} // namespace

core::Result<ProjectTransaction> ProjectTransaction::begin(
    ProjectDocument& document,
    journal::Journal& journal,
    std::string actor) {
    if (auto result = validate_actor(actor); !result) {
        return core::Result<ProjectTransaction>::failure(result.error());
    }
    const auto journal_revision = journal.current_revision();
    if (!journal_revision) {
        return core::Result<ProjectTransaction>::failure(journal_revision.error());
    }
    if (journal_revision.value() != document.revision()) {
        return core::Result<ProjectTransaction>::failure(stale(
            "project document revision does not match the journal tail"));
    }
    try {
        return core::Result<ProjectTransaction>::success(
            ProjectTransaction(document, ProjectDocument(document), journal, std::move(actor)));
    } catch (const std::exception& exception) {
        return core::Result<ProjectTransaction>::failure(io_error(
            std::string("unable to stage project transaction: ") + exception.what()));
    } catch (...) {
        return core::Result<ProjectTransaction>::failure(
            io_error("unable to stage project transaction"));
    }
}

core::Result<void> ProjectTransaction::ensure_active() const {
    if (!active_) {
        return core::Result<void>::failure(invalid_state("project transaction is no longer active"));
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectTransaction::validate_actor(std::string_view actor) {
    if (!safe_text(actor, 256U)) {
        return core::Result<void>::failure(invalid("transaction actor is invalid or too long"));
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectTransaction::validate_operation(std::string_view operation) {
    if (!safe_text(operation, 128U)) {
        return core::Result<void>::failure(invalid("transaction operation is invalid or too long"));
    }
    return core::Result<void>::success();
}

std::string ProjectTransaction::journal_payload(
    std::string_view operation,
    std::string_view serialized_document) const {
    return std::string("CARTOGRAPHER_PROJECT_TRANSACTION_V1\n") +
        "actor=" + actor_ + "\n" +
        "operation=" + std::string(operation) + "\n" +
        std::string("document_bytes=") + std::to_string(serialized_document.size()) + "\n" +
        std::string(serialized_document);
}

core::Result<scene::ObjectId> ProjectTransaction::create_object(
    std::string name,
    core::Transform transform) {
    if (auto result = ensure_active(); !result) {
        return core::Result<scene::ObjectId>::failure(result.error());
    }
    return staged_.create_object(std::move(name), transform);
}

core::Result<void> ProjectTransaction::insert_object(scene::SceneObject object) {
    if (auto result = ensure_active(); !result) return result;
    return staged_.insert_object(std::move(object));
}

core::Result<void> ProjectTransaction::remove_object(scene::ObjectId object) {
    if (auto result = ensure_active(); !result) return result;
    return staged_.remove_object(object);
}

core::Result<std::uint64_t> ProjectTransaction::add_mesh(geometry::EditableMesh mesh) {
    if (auto result = ensure_active(); !result) {
        return core::Result<std::uint64_t>::failure(result.error());
    }
    return staged_.add_mesh(std::move(mesh));
}

core::Result<void> ProjectTransaction::insert_mesh(
    std::uint64_t mesh_asset,
    geometry::EditableMesh mesh) {
    if (auto result = ensure_active(); !result) return result;
    return staged_.insert_mesh(mesh_asset, std::move(mesh));
}

core::Result<void> ProjectTransaction::replace_mesh(
    std::uint64_t mesh_asset,
    geometry::EditableMesh mesh,
    std::optional<geometry::TopologyEditReceipt> receipt) {
    if (auto result = ensure_active(); !result) return result;
    if (!receipt.has_value()) {
        return staged_.replace_mesh(mesh_asset, std::move(mesh));
    }
    const auto iterator = staged_.meshes().find(mesh_asset);
    if (iterator == staged_.meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "cannot replace a missing mesh asset"));
    }
    const auto replaced = staged_.replace_mesh_if_revision(
        mesh_asset, iterator->second.revision(), std::move(mesh), std::move(receipt));
    if (!replaced) {
        return core::Result<void>::failure(replaced.error());
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectTransaction::remove_mesh(std::uint64_t mesh_asset) {
    if (auto result = ensure_active(); !result) return result;
    return staged_.remove_mesh(mesh_asset);
}

core::Result<void> ProjectTransaction::attach_mesh(
    scene::ObjectId object,
    std::uint64_t mesh_asset) {
    if (auto result = ensure_active(); !result) return result;
    return staged_.attach_mesh(object, mesh_asset);
}

core::Result<void> ProjectTransaction::set_evaluation_graph_digest(
    std::optional<assets::Sha256Digest> digest) {
    if (auto result = ensure_active(); !result) return result;
    return staged_.set_evaluation_graph_digest(digest);
}

core::Result<void> ProjectTransaction::set_object_transform(
    scene::ObjectId object,
    core::Transform transform) {
    if (auto result = ensure_active(); !result) return result;
    return staged_.set_object_transform(object, transform);
}

core::Result<TransactionReceipt> ProjectTransaction::commit(std::string operation) {
    if (auto result = ensure_active(); !result) {
        return core::Result<TransactionReceipt>::failure(result.error());
    }
    if (auto result = validate_operation(operation); !result) {
        return core::Result<TransactionReceipt>::failure(result.error());
    }
    if (document_.revision() != expected_revision_) {
        return core::Result<TransactionReceipt>::failure(
            stale("project changed while the transaction was staged"));
    }
    if (staged_.revision() == expected_revision_) {
        return core::Result<TransactionReceipt>::failure(
            invalid_state("cannot commit an empty project transaction"));
    }
    if (auto result = staged_.validate(); !result) {
        return core::Result<TransactionReceipt>::failure(result.error());
    }
    const auto journal_revision = journal_.current_revision();
    if (!journal_revision) {
        return core::Result<TransactionReceipt>::failure(journal_revision.error());
    }
    if (journal_revision.value() != expected_revision_) {
        return core::Result<TransactionReceipt>::failure(
            stale("journal changed while the transaction was staged"));
    }

    const std::string serialized_document = staged_.serialize();
    const std::string payload = journal_payload(operation, serialized_document);
    const auto* payload_bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
    const auto appended = journal_.append(journal::JournalAppend{
        expected_revision_,
        staged_.revision(),
        std::string(operation),
        std::span<const std::uint8_t>(payload_bytes, payload.size()),
        std::nullopt,
    });
    if (!appended) {
        return core::Result<TransactionReceipt>::failure(appended.error());
    }

    const assets::Sha256Digest document_digest = assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(serialized_document.data()),
            serialized_document.size()));
    document_.swap(staged_);
    active_ = false;
    return core::Result<TransactionReceipt>::success(TransactionReceipt{
        expected_revision_, document_.revision(), actor_, std::move(operation), document_digest,
    });
}

core::Result<void> ProjectTransaction::rollback() noexcept {
    if (!active_) {
        return core::Result<void>::failure(invalid_state("project transaction is no longer active"));
    }
    active_ = false;
    return core::Result<void>::success();
}

} // namespace carto::project
