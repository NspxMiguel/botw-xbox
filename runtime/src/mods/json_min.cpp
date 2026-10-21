// SPDX-License-Identifier: MIT
#include "json_min.h"
#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>
namespace mods::json {
const Value &Value::operator[](std::string_view key) const {
    static const Value empty;
    auto object = std::get_if<Object>(&data);
    if (!object)
        return empty;
    auto it = object->find(key);
    return it == object->end() ? empty : it->second;
}
const Value::Array &Value::array() const {
    static const Array empty;
    auto value = std::get_if<Array>(&data);
    return value ? *value : empty;
}
std::string Value::string() const {
    auto v = std::get_if<std::string>(&data);
    return v ? *v : "";
}
double Value::number() const {
    auto v = std::get_if<double>(&data);
    return v ? *v : 0;
}
bool Value::boolean() const {
    auto v = std::get_if<bool>(&data);
    return v && *v;
}
namespace {
struct Reader {
    std::string_view s;
    size_t p = 0;
    [[noreturn]] void fail() const { throw std::invalid_argument("Invalid JSON"); }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t'))
            ++p;
    }
    bool take(char c) {
        ws();
        if (p < s.size() && s[p] == c) {
            ++p;
            return true;
        }
        return false;
    }
    unsigned hex() {
        unsigned n = 0;
        for (int i = 0; i < 4; ++i) {
            if (p == s.size())
                fail();
            char c = s[p++];
            unsigned d = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
            if (d == 16)
                fail();
            n = n * 16 + d;
        }
        return n;
    }
    std::string str() {
        if (!take('"'))
            fail();
        std::string out;
        while (p < s.size()) {
            unsigned char c = s[p++];
            if (c == '"')
                return out;
            if (c < 32)
                fail();
            if (c != '\\') {
                out += char(c);
                continue;
            }
            if (p == s.size())
                fail();
            char e = s[p++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                unsigned u = hex();
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (p + 2 > s.size() || s.substr(p, 2) != "\\u") {
                        u = 0xfffd;
                        goto encode;
                    }
                    p += 2;
                    unsigned low = hex();
                    // A lone surrogate in one mod name must not reject the whole page: use U+FFFD.
                    u = low >= 0xdc00 && low <= 0xdfff ? 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00) : 0xfffd;
                } else if (u >= 0xdc00 && u <= 0xdfff)
                    u = 0xfffd;
            encode:
                if (u < 128)
                    out += char(u);
                else if (u < 2048) {
                    out += char(0xc0 | (u >> 6));
                    out += char(0x80 | (u & 63));
                } else if (u < 65536) {
                    out += char(0xe0 | (u >> 12));
                    out += char(0x80 | ((u >> 6) & 63));
                    out += char(0x80 | (u & 63));
                } else {
                    out += char(0xf0 | (u >> 18));
                    out += char(0x80 | ((u >> 12) & 63));
                    out += char(0x80 | ((u >> 6) & 63));
                    out += char(0x80 | (u & 63));
                }
                break;
            }
            default:
                fail();
            }
        }
        fail();
    }
    Value value(int depth = 0) {
        ws();
        if (depth > 64 || p == s.size())
            fail();
        if (s[p] == '"')
            return Value{str()};
        if (take('{')) {
            Value::Object o;
            if (take('}'))
                return Value{o};
            do {
                auto key = str();
                if (!take(':'))
                    fail();
                o.insert_or_assign(key, value(depth + 1));
            } while (take(','));
            if (!take('}'))
                fail();
            return Value{o};
        }
        if (take('[')) {
            Value::Array a;
            if (take(']'))
                return Value{a};
            do {
                a.push_back(value(depth + 1));
            } while (take(','));
            if (!take(']'))
                fail();
            return Value{a};
        }
        for (auto literal : {std::string_view("true"), std::string_view("false"), std::string_view("null")})
            if (s.substr(p, literal.size()) == literal) {
                p += literal.size();
                return literal == "null" ? Value{} : Value{literal == "true"};
            }
        size_t start = p;
        if (s[p] == '-')
            ++p;
        if (p == s.size())
            fail();
        if (s[p] == '0')
            ++p;
        else {
            if (s[p] < '1' || s[p] > '9')
                fail();
            while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                ++p;
        }
        auto digits = [&] {
            size_t at = p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                ++p;
            if (at == p)
                fail();
        };
        if (p < s.size() && s[p] == '.') {
            ++p;
            digits();
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-'))
                ++p;
            digits();
        }
        double n = 0;
        std::istringstream number(std::string(s.substr(start, p - start)));
        number.imbue(std::locale::classic());
        number >> n;
        if (!number || !std::isfinite(n))
            fail();
        return Value{n};
    }
};
} // namespace
Value parse(std::string_view input) {
    if (input.size() > 8 * 1024 * 1024)
        throw std::invalid_argument("JSON too large");
    Reader r{input};
    auto result = r.value();
    r.ws();
    if (r.p != input.size())
        r.fail();
    return result;
}
} // namespace mods::json
