#include "pakon/process/json.hpp"

#include <cctype>
#include <charconv>
#include <format>

namespace pakon::process {

namespace {

const Json kNull{};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Result<Json> document() {
        auto v = value(0);
        if (!v) {
            return v;
        }
        skip();
        if (pos_ != s_.size()) {
            return fail("trailing characters");
        }
        return v;
    }

private:
    Result<Json> fail(std::string_view what) const {
        return failure<Json>(ErrorKind::image_bad_input,
                             std::format("JSON: {} at offset {}", what, pos_));
    }
    void skip() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }
    bool eat(char c) {
        skip();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool word(std::string_view w) {
        if (s_.substr(pos_, w.size()) == w) {
            pos_ += w.size();
            return true;
        }
        return false;
    }

    Result<std::string> str() {
        if (!eat('"')) {
            return failure<std::string>(ErrorKind::image_bad_input,
                                        std::format("JSON: string expected at offset {}", pos_));
        }
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char c = s_[pos_++];
            if (c == '\\' && pos_ < s_.size()) {
                const char e = s_[pos_++];
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
            }
            out.push_back(c);
        }
        if (pos_ >= s_.size()) {
            return failure<std::string>(ErrorKind::image_bad_input, "JSON: unterminated string");
        }
        ++pos_;
        return out;
    }

    Result<Json> value(int depth) {
        if (depth > 64) {
            return fail("nesting too deep");
        }
        skip();
        if (pos_ >= s_.size()) {
            return fail("value expected");
        }
        const char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            Json::Object obj;
            if (eat('}')) {
                return Json(std::move(obj));
            }
            do {
                auto k = str();
                if (!k) {
                    return k.error();
                }
                if (!eat(':')) {
                    return fail("':' expected");
                }
                auto v = value(depth + 1);
                if (!v) {
                    return v;
                }
                obj.insert_or_assign(std::move(*k), std::move(*v));
            } while (eat(','));
            if (!eat('}')) {
                return fail("'}' expected");
            }
            return Json(std::move(obj));
        }
        if (c == '[') {
            ++pos_;
            Json::Array arr;
            if (eat(']')) {
                return Json(std::move(arr));
            }
            do {
                auto v = value(depth + 1);
                if (!v) {
                    return v;
                }
                arr.push_back(std::move(*v));
            } while (eat(','));
            if (!eat(']')) {
                return fail("']' expected");
            }
            return Json(std::move(arr));
        }
        if (c == '"') {
            auto v = str();
            if (!v) {
                return v.error();
            }
            return Json(std::move(*v));
        }
        if (word("true")) {
            return Json(true);
        }
        if (word("false")) {
            return Json(false);
        }
        if (word("null")) {
            return Json();
        }
        double d = 0;
        const auto* first = s_.data() + pos_;
        const auto [ptr, ec] = std::from_chars(first, s_.data() + s_.size(), d);
        if (ec != std::errc{} || ptr == first) {
            return fail("number expected");
        }
        pos_ += static_cast<std::size_t>(ptr - first);
        return Json(d);
    }

    std::string_view s_;
    std::size_t pos_{0};
};

} // namespace

std::optional<double> Json::number() const {
    if (const auto* d = std::get_if<double>(&value_)) {
        return *d;
    }
    return std::nullopt;
}
std::optional<std::string> Json::string() const {
    if (const auto* s = std::get_if<std::string>(&value_)) {
        return *s;
    }
    return std::nullopt;
}
std::optional<bool> Json::boolean() const {
    if (const auto* b = std::get_if<bool>(&value_)) {
        return *b;
    }
    return std::nullopt;
}
const Json::Array* Json::array() const {
    const auto* a = std::get_if<std::shared_ptr<Array>>(&value_);
    return a ? a->get() : nullptr;
}
const Json::Object* Json::object() const {
    const auto* o = std::get_if<std::shared_ptr<Object>>(&value_);
    return o ? o->get() : nullptr;
}
const Json& Json::operator[](std::string_view key) const {
    if (const auto* o = object()) {
        if (const auto it = o->find(key); it != o->end()) {
            return it->second;
        }
    }
    return kNull;
}
const Json& Json::operator[](std::size_t index) const {
    if (const auto* a = array(); a && index < a->size()) {
        return (*a)[index];
    }
    return kNull;
}

Result<Json> parse_json(std::string_view text) { return Parser(text).document(); }

} // namespace pakon::process
