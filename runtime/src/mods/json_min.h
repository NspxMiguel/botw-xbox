// SPDX-License-Identifier: MIT
#pragma once
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace mods::json {
struct Value {
    using Object = std::map<std::string, Value, std::less<>>;
    using Array = std::vector<Value>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data = nullptr;
    const Value &operator[](std::string_view key) const;
    const Array &array() const;
    std::string string() const;
    double number() const;
    bool boolean() const;
};
// Throws invalid_argument on malformed input; depth and document size are bounded.
Value parse(std::string_view input);
} // namespace mods::json
