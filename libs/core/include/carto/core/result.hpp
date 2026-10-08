#pragma once

#include <carto/core/diagnostic.hpp>

#include <type_traits>
#include <optional>
#include <utility>
#include <variant>

namespace carto::core {

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Diagnostic diagnostic) { return Result(std::move(diagnostic)); }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(value_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const T& value() const & { return std::get<T>(value_); }
    [[nodiscard]] T& value() & { return std::get<T>(value_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(value_)); }

    [[nodiscard]] const Diagnostic& error() const & {
        return std::get<Diagnostic>(value_);
    }

    [[nodiscard]] Diagnostic& error() & { return std::get<Diagnostic>(value_); }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Diagnostic diagnostic) : value_(std::move(diagnostic)) {}

    std::variant<T, Diagnostic> value_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(); }
    static Result failure(Diagnostic diagnostic) {
        return Result(std::move(diagnostic));
    }

    [[nodiscard]] bool has_value() const noexcept { return !diagnostic_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Diagnostic& error() const & { return diagnostic_.value(); }
    [[nodiscard]] Diagnostic& error() & { return diagnostic_.value(); }

private:
    Result() = default;
    explicit Result(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {}

    std::optional<Diagnostic> diagnostic_;
};

} // namespace carto::core
