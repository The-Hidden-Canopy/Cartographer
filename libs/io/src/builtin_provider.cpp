#include <carto/io/builtin_provider.hpp>

#include <utility>

namespace carto::io {

namespace {

core::Result<void> make_ready(
    providers::Registry& registry,
    providers::ProviderDescriptor descriptor) {
    const std::string id = descriptor.id;
    if (auto result = registry.discover(std::move(descriptor)); !result) return result;
    for (const auto state : {
             providers::ProviderState::inspected,
             providers::ProviderState::registered,
             providers::ProviderState::verified,
             providers::ProviderState::ready,
         }) {
        if (auto result = registry.transition(id, state); !result) return result;
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> register_builtin_obj_providers(providers::Registry& registry) {
    if (auto result = make_ready(registry, {
            "cartographer.obj.import",
            providers::ProviderKind::geometry_import,
            {"geometry.import.obj"},
            {1U, 0U, 0U},
            providers::TrustClass::trusted_in_process,
        }); !result) {
        return result;
    }
    return make_ready(registry, {
        "cartographer.obj.export",
        providers::ProviderKind::geometry_export,
        {"geometry.export.obj"},
        {1U, 0U, 0U},
        providers::TrustClass::trusted_in_process,
    });
}

core::Result<void> register_builtin_gltf_providers(providers::Registry& registry) {
    return make_ready(registry, {
        "cartographer.gltf.export",
        providers::ProviderKind::geometry_export,
        {"geometry.export.gltf"},
        {1U, 0U, 0U},
        providers::TrustClass::trusted_in_process,
    });
}

core::Result<void> register_builtin_ply_providers(providers::Registry& registry) {
    if (auto result = make_ready(registry, {
            "cartographer.ply.import",
            providers::ProviderKind::geometry_import,
            {"geometry.import.ply"},
            {1U, 0U, 0U},
            providers::TrustClass::trusted_in_process,
        }); !result) {
        return result;
    }
    return make_ready(registry, {
        "cartographer.ply.export",
        providers::ProviderKind::geometry_export,
        {"geometry.export.ply"},
        {1U, 0U, 0U},
        providers::TrustClass::trusted_in_process,
    });
}

core::Result<void> register_builtin_stl_providers(providers::Registry& registry) {
    if (auto result = make_ready(registry, {
            "cartographer.stl.import",
            providers::ProviderKind::geometry_import,
            {"geometry.import.stl"},
            {1U, 0U, 0U},
            providers::TrustClass::trusted_in_process,
        }); !result) {
        return result;
    }
    return make_ready(registry, {
        "cartographer.stl.export",
        providers::ProviderKind::geometry_export,
        {"geometry.export.stl"},
        {1U, 0U, 0U},
        providers::TrustClass::trusted_in_process,
    });
}

} // namespace carto::io
