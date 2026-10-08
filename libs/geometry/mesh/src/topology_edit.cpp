#include <carto/geometry/topology_edit.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace carto::geometry {

namespace {

constexpr std::uint64_t kMaxSerializedReceiptCount = 1'000'000U;
constexpr std::uint64_t kMaxSerializedReceiptElements = 2'000'000U;
constexpr std::size_t kMaxSerializedReceiptBytes = 32U * 1024U * 1024U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Result<void> require_token(std::istream& input, std::string_view expected) {
    std::string token;
    if (!(input >> token) || token != expected) {
        return core::Result<void>::failure(validation(
            "topology receipt is missing the expected " + std::string(expected) + " record"));
    }
    return core::Result<void>::success();
}

core::Result<std::uint64_t> read_uint(std::istream& input, std::string_view field) {
    std::string token;
    if (!(input >> token)) {
        return core::Result<std::uint64_t>::failure(validation(
            "topology receipt is missing " + std::string(field)));
    }
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return core::Result<std::uint64_t>::failure(invalid(
            "topology receipt field is not an unsigned integer: " + std::string(field)));
    }
    return core::Result<std::uint64_t>::success(value);
}

core::Result<std::uint64_t> read_bounded_count(
    std::istream& input,
    std::string_view field) {
    auto count = read_uint(input, field);
    if (!count) return count;
    if (count.value() > kMaxSerializedReceiptCount) {
        return core::Result<std::uint64_t>::failure(validation(
            "topology receipt " + std::string(field) + " exceeds the bounded limit"));
    }
    return count;
}

template <typename Id>
void write_ids(std::ostream& output, std::string_view tag, const std::vector<Id>& ids) {
    output << tag << ' ' << ids.size();
    for (const Id id : ids) output << ' ' << id.value;
    output << '\n';
}

template <typename Id>
void write_origins(
    std::ostream& output,
    std::string_view tag,
    const std::map<Id, ElementOrigin>& origins) {
    output << tag << ' ' << origins.size() << '\n';
    for (const auto& [id, origin] : origins) {
        output << "ORIGIN " << id.value << ' '
               << static_cast<std::uint64_t>(origin.kind) << ' '
               << origin.source_ids.size();
        for (const auto source_id : origin.source_ids) output << ' ' << source_id;
        output << '\n';
    }
}

void write_merged_vertices(
    std::ostream& output,
    const std::map<VertexId, VertexId>& merged_vertices) {
    output << "MERGED_VERTICES " << merged_vertices.size() << '\n';
    for (const auto& [source, target] : merged_vertices) {
        output << "MERGE " << source.value << ' ' << target.value << '\n';
    }
}

bool valid_origin_kind_value(std::uint64_t value) noexcept {
    return value <= static_cast<std::uint64_t>(OriginKind::subdivision_child);
}

} // namespace

std::string TopologyEditReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_TOPOLOGY_RECEIPT 2\n";
    output << "REVISION_BEFORE " << revision_before.value() << '\n';
    output << "REVISION_AFTER " << revision_after.value() << '\n';
    write_ids(output, "CREATED_VERTICES", created_vertices);
    write_ids(output, "CREATED_EDGES", created_edges);
    write_ids(output, "CREATED_FACES", created_faces);
    write_ids(output, "REMOVED_VERTICES", removed_vertices);
    write_ids(output, "REMOVED_EDGES", removed_edges);
    write_ids(output, "REMOVED_FACES", removed_faces);
    write_ids(output, "CREATED_CORNERS", created_corners);
    write_ids(output, "REMOVED_CORNERS", removed_corners);
    write_merged_vertices(output, merged_vertices);
    write_origins(output, "VERTEX_ORIGINS", vertex_origins);
    write_origins(output, "EDGE_ORIGINS", edge_origins);
    write_origins(output, "FACE_ORIGINS", face_origins);
    write_origins(output, "CORNER_ORIGINS", corner_origins);
    output << "END\n";
    return output.str();
}

core::Result<TopologyEditReceipt> TopologyEditReceipt::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedReceiptBytes) {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "topology receipt exceeds the bounded serialized size"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_token(input, "CARTOGRAPHER_TOPOLOGY_RECEIPT"); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    auto version = read_uint(input, "schema version");
    if (!version) return core::Result<TopologyEditReceipt>::failure(version.error());
    if (version.value() != 1U && version.value() != 2U) {
        return core::Result<TopologyEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "unsupported topology receipt schema version"));
    }

    TopologyEditReceipt receipt;
    std::uint64_t element_budget = 0U;
    const auto consume_element_budget = [
        &element_budget](std::uint64_t count, std::string_view field) -> core::Result<void> {
        if (count > kMaxSerializedReceiptElements - element_budget) {
            return core::Result<void>::failure(validation(
                "topology receipt aggregate " + std::string(field) +
                " exceeds the bounded element budget"));
        }
        element_budget += count;
        return core::Result<void>::success();
    };
    if (auto result = require_token(input, "REVISION_BEFORE"); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    auto revision_before = read_uint(input, "revision before");
    if (!revision_before) return core::Result<TopologyEditReceipt>::failure(revision_before.error());
    receipt.revision_before = core::Revision(revision_before.value());

    if (auto result = require_token(input, "REVISION_AFTER"); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    auto revision_after = read_uint(input, "revision after");
    if (!revision_after) return core::Result<TopologyEditReceipt>::failure(revision_after.error());
    receipt.revision_after = core::Revision(revision_after.value());

    const auto read_ids = [&](std::string_view tag, auto& destination) -> core::Result<void> {
        if (auto result = require_token(input, tag); !result) return result;
        auto count = read_bounded_count(input, std::string(tag) + " count");
        if (!count) return core::Result<void>::failure(count.error());
        if (auto result = consume_element_budget(count.value(), tag); !result) {
            return result;
        }
        destination.clear();
        destination.reserve(static_cast<std::size_t>(count.value()));
        using Id = typename std::decay_t<decltype(destination)>::value_type;
        for (std::uint64_t index = 0U; index < count.value(); ++index) {
            auto id = read_uint(input, std::string(tag) + " id");
            if (!id) return core::Result<void>::failure(id.error());
            destination.push_back(Id{id.value()});
        }
        return core::Result<void>::success();
    };

    if (auto result = read_ids("CREATED_VERTICES", receipt.created_vertices); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_ids("CREATED_EDGES", receipt.created_edges); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_ids("CREATED_FACES", receipt.created_faces); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_ids("REMOVED_VERTICES", receipt.removed_vertices); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_ids("REMOVED_EDGES", receipt.removed_edges); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_ids("REMOVED_FACES", receipt.removed_faces); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_ids("CREATED_CORNERS", receipt.created_corners); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_ids("REMOVED_CORNERS", receipt.removed_corners); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }

    std::string next_section;
    if (!(input >> next_section)) {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "topology receipt is missing the expected VERTEX_ORIGINS record"));
    }
    if (version.value() >= 2U) {
        if (next_section != "MERGED_VERTICES") {
            return core::Result<TopologyEditReceipt>::failure(validation(
                "topology receipt v2 is missing the expected MERGED_VERTICES record"));
        }
        auto count = read_bounded_count(input, "MERGED_VERTICES count");
        if (!count) return core::Result<TopologyEditReceipt>::failure(count.error());
        if (auto result = consume_element_budget(
                count.value() * 2U, "MERGED_VERTICES source/target IDs"); !result) {
            return core::Result<TopologyEditReceipt>::failure(result.error());
        }
        for (std::uint64_t index = 0U; index < count.value(); ++index) {
            if (auto result = require_token(input, "MERGE"); !result) {
                return core::Result<TopologyEditReceipt>::failure(result.error());
            }
            auto source = read_uint(input, "merged source vertex");
            auto target = read_uint(input, "merged target vertex");
            if (!source || !target) {
                return core::Result<TopologyEditReceipt>::failure(
                    !source ? source.error() : target.error());
            }
            const auto [iterator, inserted] = receipt.merged_vertices.emplace(
                VertexId{source.value()}, VertexId{target.value()});
            static_cast<void>(iterator);
            if (!inserted) {
                return core::Result<TopologyEditReceipt>::failure(validation(
                    "topology receipt contains a duplicate merged source vertex"));
            }
        }
        if (!(input >> next_section)) {
            return core::Result<TopologyEditReceipt>::failure(validation(
                "topology receipt is missing the expected VERTEX_ORIGINS record"));
        }
    } else if (next_section != "VERTEX_ORIGINS") {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "topology receipt is missing the expected VERTEX_ORIGINS record"));
    }

    const auto read_origins = [&](
        std::string_view tag,
        auto& destination,
        bool tag_already_consumed = false) -> core::Result<void> {
        if (!tag_already_consumed) {
            if (auto result = require_token(input, tag); !result) return result;
        }
        auto count = read_bounded_count(input, std::string(tag) + " count");
        if (!count) return core::Result<void>::failure(count.error());
        if (auto result = consume_element_budget(count.value(), tag); !result) {
            return result;
        }
        destination.clear();
        using Map = std::decay_t<decltype(destination)>;
        using Id = typename Map::key_type;
        for (std::uint64_t index = 0U; index < count.value(); ++index) {
            if (auto result = require_token(input, "ORIGIN"); !result) return result;
            auto id = read_uint(input, std::string(tag) + " destination id");
            auto kind = read_uint(input, std::string(tag) + " origin kind");
            auto source_count = read_bounded_count(input, std::string(tag) + " source count");
            if (!id || !kind || !source_count) {
                return core::Result<void>::failure(
                    !id ? id.error() : (!kind ? kind.error() : source_count.error()));
            }
            if (!valid_origin_kind_value(kind.value())) {
                return core::Result<void>::failure(validation(
                    "topology receipt contains an unknown origin kind"));
            }
            if (auto result = consume_element_budget(source_count.value(), "source IDs");
                !result) {
                return result;
            }
            ElementOrigin origin;
            origin.kind = static_cast<OriginKind>(kind.value());
            origin.source_ids.reserve(static_cast<std::size_t>(source_count.value()));
            for (std::uint64_t source_index = 0U; source_index < source_count.value(); ++source_index) {
                auto source_id = read_uint(input, std::string(tag) + " source id");
                if (!source_id) return core::Result<void>::failure(source_id.error());
                origin.source_ids.push_back(source_id.value());
            }
            const auto [iterator, inserted] = destination.emplace(
                Id{id.value()}, std::move(origin));
            static_cast<void>(iterator);
            if (!inserted) {
                return core::Result<void>::failure(validation(
                    "topology receipt contains a duplicate origin destination"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = read_origins("VERTEX_ORIGINS", receipt.vertex_origins, true); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_origins("EDGE_ORIGINS", receipt.edge_origins); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_origins("FACE_ORIGINS", receipt.face_origins); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = read_origins("CORNER_ORIGINS", receipt.corner_origins); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    if (auto result = require_token(input, "END"); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<TopologyEditReceipt>::failure(validation(
            "topology receipt contains unexpected trailing data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<TopologyEditReceipt>::failure(result.error());
    }
    return core::Result<TopologyEditReceipt>::success(std::move(receipt));
}

} // namespace carto::geometry
