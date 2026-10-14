#pragma once

// Minimal JSON reader for this project's own files (the .scan.json
// sidecar): objects, arrays, numbers, strings (no \u escapes beyond
// ASCII), true/false/null. Processing reads scan settings through it;
// nothing here is used on the scan path.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pakon/errors/error.hpp"

namespace pakon::process {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    Json() = default;
    explicit Json(double v) : value_(v) {}
    explicit Json(bool v) : value_(v) {}
    explicit Json(std::string v) : value_(std::move(v)) {}
    explicit Json(Array v) : value_(std::make_shared<Array>(std::move(v))) {}
    explicit Json(Object v) : value_(std::make_shared<Object>(std::move(v))) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(value_); }
    std::optional<double> number() const;
    std::optional<std::string> string() const;
    std::optional<bool> boolean() const;
    const Array* array() const;
    const Object* object() const;

    // obj["a"]["b"] style lookup; a null Json when absent.
    const Json& operator[](std::string_view key) const;
    const Json& operator[](std::size_t index) const;

private:
    std::variant<std::monostate, double, bool, std::string, std::shared_ptr<Array>,
                 std::shared_ptr<Object>>
        value_;
};

Result<Json> parse_json(std::string_view text);

} // namespace pakon::process
