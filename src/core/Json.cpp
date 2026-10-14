#include "core/Json.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace nb::json {

namespace {
const Value kNull;
const Array kEmptyArray;
const Object kEmptyObject;

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    bool lit(std::string_view w) {
        if (s.substr(i, w.size()) != w) return false;
        i += w.size();
        return true;
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& out) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return fail("bad hex digit");
        }
        return true;
    }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control char in string");
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (!lit("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& out) {
        if (++depth > 64) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        bool ok = true;
        char c = s[i];
        if (c == '{') {
            ++i;
            Object o;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; }
            else {
                for (;;) {
                    ws();
                    std::string k;
                    if (!str(k)) return false;
                    ws();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                    ++i;
                    Value v;
                    if (!value(v)) return false;
                    o[std::move(k)] = std::move(v);
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    return fail("expected ',' or '}'");
                }
            }
            out = Value(std::move(o));
        } else if (c == '[') {
            ++i;
            Array a;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; }
            else {
                for (;;) {
                    Value v;
                    if (!value(v)) return false;
                    a.push_back(std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    return fail("expected ',' or ']'");
                }
            }
            out = Value(std::move(a));
        } else if (c == '"') {
            std::string v;
            ok = str(v);
            out = Value(std::move(v));
        } else if (lit("true")) out = Value(true);
        else if (lit("false")) out = Value(false);
        else if (lit("null")) out = Value(nullptr);
        else {
            size_t start = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
            double d = 0;
            auto r = std::from_chars(s.data() + start, s.data() + i, d);
            if (r.ec != std::errc() || r.ptr != s.data() + i || start == i) return fail("bad number");
            out = Value(d);
        }
        --depth;
        return ok;
    }
};

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else out += static_cast<char>(c);
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v, bool pretty, int indent) {
    auto nl = [&](int ind) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(ind) * 2, ' ');
    };
    switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Number: {
        double d = v.asNumber();
        if (!std::isfinite(d)) { out += "0"; break; }
        char buf[32];
        if (d == std::floor(d) && std::fabs(d) < 1e15) snprintf(buf, sizeof buf, "%.0f", d);
        else {
            auto r = std::to_chars(buf, buf + sizeof buf - 1, d);
            *r.ptr = 0;
        }
        out += buf;
        break;
    }
    case Value::Type::String: escape(out, v.asString()); break;
    case Value::Type::Array: {
        const auto& a = v.asArray();
        out += '[';
        for (size_t k = 0; k < a.size(); ++k) {
            if (k) out += ',';
            nl(indent + 1);
            write(out, a[k], pretty, indent + 1);
        }
        if (!a.empty()) nl(indent);
        out += ']';
        break;
    }
    case Value::Type::Object: {
        const auto& o = v.asObject();
        out += '{';
        bool first = true;
        for (const auto& [k, val] : o) {
            if (!first) out += ',';
            first = false;
            nl(indent + 1);
            escape(out, k);
            out += pretty ? ": " : ":";
            write(out, val, pretty, indent + 1);
        }
        if (!o.empty()) nl(indent);
        out += '}';
        break;
    }
    }
}
} // namespace

const Array& Value::asArray() const { return isArray() ? *std::get<std::shared_ptr<Array>>(v_) : kEmptyArray; }
const Object& Value::asObject() const { return isObject() ? *std::get<std::shared_ptr<Object>>(v_) : kEmptyObject; }

Array& Value::mutArray() {
    if (!isArray()) v_ = std::make_shared<Array>();
    auto& p = std::get<std::shared_ptr<Array>>(v_);
    if (p.use_count() > 1) p = std::make_shared<Array>(*p); // copy-on-write
    return *p;
}

Object& Value::mutObject() {
    if (!isObject()) v_ = std::make_shared<Object>();
    auto& p = std::get<std::shared_ptr<Object>>(v_);
    if (p.use_count() > 1) p = std::make_shared<Object>(*p);
    return *p;
}

const Value& Value::operator[](const std::string& key) const {
    if (!isObject()) return kNull;
    const auto& o = asObject();
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}

Value& Value::set(const std::string& key, Value v) { return mutObject()[key] = std::move(v); }
bool Value::has(const std::string& key) const { return isObject() && asObject().count(key) > 0; }

std::optional<Value> Parse(std::string_view text, ParseError* error) {
    // Skip UTF-8 BOM if an editor added one.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    Parser p{text};
    Value v;
    bool ok = p.value(v);
    if (ok) {
        p.ws();
        if (p.i != text.size()) ok = p.fail("trailing characters");
    }
    if (!ok) {
        if (error) *error = {p.i, p.err};
        return std::nullopt;
    }
    return v;
}

std::string Serialize(const Value& v, bool pretty) {
    std::string out;
    write(out, v, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

} // namespace nb::json
