#include <cartographer/carto.h>

#include <carto/application/application.hpp>
#include <carto/core/json.hpp>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

struct carto_context {
    std::string config_json;
    std::filesystem::path project_root;
};

struct carto_document {
    std::filesystem::path project_root;
    carto::application::ApplicationSession session;
};

namespace {

constexpr std::uint32_t kAbiVersion = 1U;
constexpr std::size_t kMaxJsonBytes = 64U * 1024U;

void clear_error(carto_error* error) noexcept {
    if (error == nullptr) return;
    error->code = CARTO_OK;
    error->message = nullptr;
}

carto_status status_for(carto::core::ErrorCode code) noexcept {
    switch (code) {
    case carto::core::ErrorCode::invalid_argument:
        return CARTO_INVALID_ARGUMENT;
    case carto::core::ErrorCode::not_found:
        return CARTO_NOT_FOUND;
    case carto::core::ErrorCode::invalid_state:
    case carto::core::ErrorCode::stale_data:
        return CARTO_CONFLICT;
    case carto::core::ErrorCode::version_mismatch:
        return CARTO_UNSUPPORTED_VERSION;
    case carto::core::ErrorCode::io_error:
        return CARTO_IO_ERROR;
    case carto::core::ErrorCode::validation_failed:
        return CARTO_VALIDATION_FAILED;
    case carto::core::ErrorCode::cycle_detected:
    case carto::core::ErrorCode::unsupported:
        return CARTO_INVALID_ARGUMENT;
    }
    return CARTO_INTERNAL_ERROR;
}

carto_status fail(carto::core::Diagnostic diagnostic, carto_error* error) noexcept {
    if (error != nullptr) {
        error->code = static_cast<int32_t>(status_for(diagnostic.code));
        const std::string& message = diagnostic.message;
        error->message = static_cast<char*>(std::malloc(message.size() + 1U));
        if (error->message == nullptr) {
            error->code = CARTO_INTERNAL_ERROR;
            return CARTO_INTERNAL_ERROR;
        }
        std::memcpy(error->message, message.data(), message.size());
        error->message[message.size()] = '\0';
    }
    return status_for(diagnostic.code);
}

carto_status invalid(const char* message, carto_error* error) noexcept {
    return fail(carto::core::Diagnostic(carto::core::ErrorCode::invalid_argument, message), error);
}

bool valid_json_object(std::string_view text) noexcept {
    return carto::core::json::is_object(text);
}

bool bounded_json(const char* request) noexcept {
    if (request == nullptr) return false;
    const std::string_view text(request);
    return text.size() <= kMaxJsonBytes && valid_json_object(text);
}

bool contains_parent_component(const std::filesystem::path& path) {
    for (const auto& component : path) {
        if (component == "..") return true;
    }
    return false;
}

std::optional<std::filesystem::path> authorized_path(
    const std::filesystem::path& root,
    std::string_view requested,
    std::string& failure_message) {
    if (requested.empty()) {
        failure_message = "path must not be empty";
        return std::nullopt;
    }
    const std::filesystem::path input(requested);
    if (contains_parent_component(input)) {
        failure_message = "path traversal is not permitted";
        return std::nullopt;
    }
    const std::filesystem::path candidate = input.is_absolute() ? input : root / input;
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        failure_message = "unable to resolve path within the project root";
        return std::nullopt;
    }
    const auto relative = std::filesystem::relative(normalized, root, error);
    if (error || relative.empty() || relative.is_absolute() || contains_parent_component(relative)) {
        failure_message = "path is outside the configured project root";
        return std::nullopt;
    }
    return normalized;
}

std::optional<std::string> json_string_field(std::string_view request, std::string_view name) {
    const auto object = carto::core::json::parse_object(request);
    if (!object) return std::nullopt;
    const auto* member = carto::core::json::find_member(object.value(), name);
    if (member == nullptr) return std::nullopt;
    const auto value = carto::core::json::decode_string(member->raw_value);
    if (!value || value.value().size() > kMaxJsonBytes) return std::nullopt;
    return value.value();
}

std::string json_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        default:
            if (character < 0x20U) {
                escaped += "\\u00";
                escaped.push_back(hex[character >> 4U]);
                escaped.push_back(hex[character & 0x0fU]);
            } else {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

carto_status allocate_response(std::string response, char** output, carto_error* error) noexcept {
    if (output == nullptr) return invalid("output response pointer is null", error);
    if (response.size() > kMaxJsonBytes) return invalid("response exceeds the ABI size limit", error);
    char* allocated = static_cast<char*>(std::malloc(response.size() + 1U));
    if (allocated == nullptr) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error,
                                            "response allocation failed"), error);
    }
    std::memcpy(allocated, response.data(), response.size());
    allocated[response.size()] = '\0';
    *output = allocated;
    return CARTO_OK;
}

std::string snapshot_json(const carto::application::ApplicationSnapshot& snapshot) {
    return "{\"project_name\":\"" + json_escape(snapshot.project_name) +
        "\",\"project_revision\":" + std::to_string(snapshot.project_revision.value()) +
        ",\"dirty\":" + (snapshot.dirty ? "true" : "false") +
        ",\"object_count\":" + std::to_string(snapshot.objects.size()) +
        ",\"undo_count\":" + std::to_string(snapshot.undo_count) +
        ",\"redo_count\":" + std::to_string(snapshot.redo_count) +
        ",\"problem_count\":" + std::to_string(snapshot.problems.size()) + "}";
}

std::string receipt_json(const carto::application::DispatchReceipt& receipt) {
    return "{\"action\":\"" + json_escape(receipt.action) +
        "\",\"revision_before\":" + std::to_string(receipt.revision_before.value()) +
        ",\"revision_after\":" + std::to_string(receipt.revision_after.value()) +
        ",\"document_changed\":" + (receipt.document_changed ? "true" : "false") + "}";
}

carto_status dispatch_action(
    carto_document& document,
    std::string_view request,
    char** output,
    carto_error* error) noexcept {
    const auto operation = json_string_field(request, "op");
    if (!operation.has_value()) return invalid("execute request requires a string op", error);

    carto::application::ApplicationAction action;
    if (*operation == "save") {
        const auto path = json_string_field(request, "path");
        if (!path.has_value() || path->empty()) return invalid("save requires a non-empty path", error);
        std::string path_error;
        const auto authorized = authorized_path(document.project_root, *path, path_error);
        if (!authorized) return invalid(path_error.c_str(), error);
        action = carto::application::SaveProjectAction{authorized.value()};
    } else if (*operation == "create_box") {
        const auto name = json_string_field(request, "name");
        if (!name.has_value() || name->empty()) return invalid("create_box requires a non-empty name", error);
        action = carto::application::CreateBoxAction{*name};
    } else if (*operation == "create_plane") {
        const auto name = json_string_field(request, "name");
        if (!name.has_value() || name->empty()) return invalid("create_plane requires a non-empty name", error);
        action = carto::application::CreatePlaneAction{*name};
    } else if (*operation == "undo") {
        action = carto::application::UndoAction{};
    } else if (*operation == "redo") {
        action = carto::application::RedoAction{};
    } else {
        return invalid("execute request uses an unsupported op", error);
    }

    const auto result = carto::application::HumanApplicationAccess::dispatch(
        document.session, action);
    if (!result) return fail(result.error(), error);
    return allocate_response(receipt_json(result.value()), output, error);
}

} // namespace

extern "C" {

uint32_t carto_abi_version(void) {
    return kAbiVersion;
}

carto_status carto_context_create(
    const char* config_json,
    carto_context** out_context,
    carto_error* out_error) {
    clear_error(out_error);
    if (out_context == nullptr) return invalid("output context pointer is null", out_error);
    *out_context = nullptr;
    if (config_json != nullptr && std::strlen(config_json) > kMaxJsonBytes) {
        return invalid("context configuration exceeds the ABI size limit", out_error);
    }
    if (config_json != nullptr && std::strlen(config_json) != 0U &&
        !valid_json_object(config_json)) {
        return invalid("context configuration must be one JSON object", out_error);
    }
    try {
        auto* context = new carto_context;
        if (config_json != nullptr) context->config_json = config_json;
        std::filesystem::path configured_root = std::filesystem::current_path();
        if (config_json != nullptr && std::strlen(config_json) != 0U) {
            const auto root = json_string_field(config_json, "project_root");
            if (root.has_value()) {
                configured_root = std::filesystem::path(root.value());
            } else if (std::string_view(config_json).find("project_root") != std::string_view::npos) {
                delete context;
                return invalid("project_root must be a valid JSON string", out_error);
            }
        }
        std::error_code root_error;
        context->project_root = std::filesystem::weakly_canonical(configured_root, root_error);
        if (root_error || !std::filesystem::is_directory(context->project_root, root_error) || root_error) {
            delete context;
            return invalid("project_root must identify an existing directory", out_error);
        }
        *out_context = context;
        return CARTO_OK;
    } catch (const std::exception& exception) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, exception.what()), out_error);
    } catch (...) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, "context allocation failed"), out_error);
    }
}

carto_status carto_document_open(
    carto_context* context,
    const char* path_utf8,
    carto_document** out_document,
    carto_error* out_error) {
    clear_error(out_error);
    if (context == nullptr || path_utf8 == nullptr || out_document == nullptr) {
        return invalid("document_open received a null argument", out_error);
    }
    *out_document = nullptr;
    if (std::strlen(path_utf8) == 0U || std::strlen(path_utf8) > kMaxJsonBytes) {
        return invalid("document path is empty or too long", out_error);
    }
    try {
        std::string path_error;
        const auto authorized = authorized_path(context->project_root, path_utf8, path_error);
        if (!authorized) return invalid(path_error.c_str(), out_error);
        auto* document = new carto_document;
        document->project_root = context->project_root;
        const auto opened = carto::application::HumanApplicationAccess::dispatch(
            document->session,
            carto::application::OpenProjectAction{authorized.value(), true});
        if (!opened) {
            const carto_status result = fail(opened.error(), out_error);
            delete document;
            return result;
        }
        *out_document = document;
        return CARTO_OK;
    } catch (const std::exception& exception) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, exception.what()), out_error);
    } catch (...) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, "document allocation failed"), out_error);
    }
}

carto_status carto_document_query_json(
    carto_document* document,
    const char* request_json,
    char** out_response_json,
    carto_error* out_error) {
    clear_error(out_error);
    if (document == nullptr || !bounded_json(request_json) || out_response_json == nullptr) {
        return invalid("query received a null or oversized argument", out_error);
    }
    *out_response_json = nullptr;
    try {
        const auto operation = json_string_field(request_json, "op");
        if (!operation.has_value()) return invalid("query request requires a string op", out_error);
        if (*operation == "snapshot") {
            return allocate_response(snapshot_json(document->session.snapshot()), out_response_json, out_error);
        }
        if (*operation == "capabilities") {
            return allocate_response(
                "{\"abi_version\":1,\"operations\":[\"snapshot\",\"save\",\"create_box\",\"create_plane\",\"undo\",\"redo\"]}",
                out_response_json, out_error);
        }
        return invalid("query request uses an unsupported op", out_error);
    } catch (const std::exception& exception) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, exception.what()), out_error);
    } catch (...) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, "query failed unexpectedly"), out_error);
    }
}

carto_status carto_document_execute_json(
    carto_document* document,
    const char* request_json,
    char** out_response_json,
    carto_error* out_error) {
    clear_error(out_error);
    if (document == nullptr || !bounded_json(request_json) || out_response_json == nullptr) {
        return invalid("execute received a null or oversized argument", out_error);
    }
    *out_response_json = nullptr;
    try {
        return dispatch_action(*document, request_json, out_response_json, out_error);
    } catch (const std::exception& exception) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, exception.what()), out_error);
    } catch (...) {
        return fail(carto::core::Diagnostic(carto::core::ErrorCode::io_error, "execute failed unexpectedly"), out_error);
    }
}

void carto_document_close(carto_document* document) {
    delete document;
}

void carto_context_destroy(carto_context* context) {
    delete context;
}

void carto_free(void* allocation) {
    std::free(allocation);
}

} // extern "C"
