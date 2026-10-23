#include <carto/providers/registry.hpp>

#include <algorithm>
#include <utility>

namespace carto::providers {

namespace {

constexpr std::size_t kMaxIdBytes = 128U;
constexpr std::size_t kMaxCapabilityBytes = 128U;
constexpr std::size_t kMaxCapabilities = 64U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic not_found(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unavailable(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

bool safe_token(std::string_view value, std::size_t max_bytes) {
    return !value.empty() && value.size() <= max_bytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

bool is_failure(ProviderState state) noexcept {
    switch (state) {
    case ProviderState::unsupported:
    case ProviderState::version_mismatch:
    case ProviderState::capability_missing:
    case ProviderState::dependency_missing:
    case ProviderState::untrusted:
    case ProviderState::load_failed:
    case ProviderState::unavailable:
        return true;
    case ProviderState::discovered:
    case ProviderState::inspected:
    case ProviderState::registered:
    case ProviderState::verified:
    case ProviderState::ready:
    case ProviderState::loaded:
        return false;
    }
    return true;
}

const char* state_name(ProviderState state) noexcept {
    switch (state) {
    case ProviderState::discovered: return "discovered";
    case ProviderState::inspected: return "inspected";
    case ProviderState::registered: return "registered";
    case ProviderState::verified: return "verified";
    case ProviderState::ready: return "ready";
    case ProviderState::loaded: return "loaded";
    case ProviderState::unsupported: return "unsupported";
    case ProviderState::version_mismatch: return "version_mismatch";
    case ProviderState::capability_missing: return "capability_missing";
    case ProviderState::dependency_missing: return "dependency_missing";
    case ProviderState::untrusted: return "untrusted";
    case ProviderState::load_failed: return "load_failed";
    case ProviderState::unavailable: return "unavailable";
    }
    return "unknown";
}

} // namespace

core::Result<void> Registry::validate_descriptor(const ProviderDescriptor& descriptor) {
    if (!safe_token(descriptor.id, kMaxIdBytes)) {
        return core::Result<void>::failure(invalid("provider id is invalid or too long"));
    }
    if (descriptor.capabilities.empty() || descriptor.capabilities.size() > kMaxCapabilities) {
        return core::Result<void>::failure(invalid("provider capabilities are empty or too numerous"));
    }
    for (const auto& capability : descriptor.capabilities) {
        if (!safe_token(capability, kMaxCapabilityBytes)) {
            return core::Result<void>::failure(invalid("provider capability is invalid or too long"));
        }
    }
    return core::Result<void>::success();
}

bool Registry::can_transition(ProviderState from, ProviderState to) noexcept {
    if (is_failure(from)) {
        return to == ProviderState::discovered;
    }
    switch (from) {
    case ProviderState::discovered:
        return to == ProviderState::inspected || is_failure(to);
    case ProviderState::inspected:
        return to == ProviderState::registered || is_failure(to);
    case ProviderState::registered:
        return to == ProviderState::verified || is_failure(to);
    case ProviderState::verified:
        return to == ProviderState::ready || is_failure(to);
    case ProviderState::ready:
        return to == ProviderState::loaded || to == ProviderState::load_failed ||
            to == ProviderState::unavailable;
    case ProviderState::loaded:
        return to == ProviderState::load_failed || to == ProviderState::unavailable;
    case ProviderState::unsupported:
    case ProviderState::version_mismatch:
    case ProviderState::capability_missing:
    case ProviderState::dependency_missing:
    case ProviderState::untrusted:
    case ProviderState::load_failed:
    case ProviderState::unavailable:
        return to == ProviderState::discovered;
    }
    return false;
}

core::Result<void> Registry::discover(ProviderDescriptor descriptor) {
    if (auto result = validate_descriptor(descriptor); !result) {
        return result;
    }
    if (descriptor.state != ProviderState::discovered) {
        return core::Result<void>::failure(
            invalid("provider discovery must begin in the discovered state"));
    }
    if (providers_.contains(descriptor.id)) {
        return core::Result<void>::failure(
            validation("provider id is already registered"));
    }
    providers_.emplace(descriptor.id, std::move(descriptor));
    return core::Result<void>::success();
}

core::Result<void> Registry::transition(
    std::string_view provider_id,
    ProviderState target) {
    const auto found = providers_.find(std::string(provider_id));
    if (found == providers_.end()) {
        return core::Result<void>::failure(not_found("provider id is not registered"));
    }
    if (!can_transition(found->second.state, target)) {
        return core::Result<void>::failure(validation("provider lifecycle transition is invalid"));
    }
    found->second.state = target;
    return core::Result<void>::success();
}

core::Result<ProviderDescriptor> Registry::resolve(std::string_view capability) const {
    if (!safe_token(capability, kMaxCapabilityBytes)) {
        return core::Result<ProviderDescriptor>::failure(
            invalid("provider capability query is invalid or too long"));
    }
    bool capability_seen = false;
    for (const auto& [id, descriptor] : providers_) {
        static_cast<void>(id);
        if (std::find(descriptor.capabilities.begin(), descriptor.capabilities.end(), capability) ==
            descriptor.capabilities.end()) {
            continue;
        }
        capability_seen = true;
        if (descriptor.state == ProviderState::ready || descriptor.state == ProviderState::loaded) {
            return core::Result<ProviderDescriptor>::success(descriptor);
        }
    }
    if (!capability_seen) {
        return core::Result<ProviderDescriptor>::failure(
            not_found("no provider advertises the requested capability"));
    }
    std::string states = "matching providers exist but none are ready:";
    for (const auto& [id, descriptor] : providers_) {
        if (std::find(descriptor.capabilities.begin(), descriptor.capabilities.end(), capability) ==
            descriptor.capabilities.end()) {
            continue;
        }
        states += " " + id + "=" + state_name(descriptor.state);
    }
    return core::Result<ProviderDescriptor>::failure(unavailable(std::move(states)));
}

} // namespace carto::providers
