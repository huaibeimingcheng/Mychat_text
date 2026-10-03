#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

struct JsonValue::Data {
    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::map<std::string, JsonValue> objVal;
};

JsonValue::JsonValue() : m_d(std::make_shared<Data>()) {}
JsonValue::JsonValue(bool b) : JsonValue() { m_d->type = Type::Bool; m_d->boolVal = b; }
JsonValue::JsonValue(double d) : JsonValue() { m_d->type = Type::Number; m_d->numVal = d; }
JsonValue::JsonValue(int i) : JsonValue(static_cast<double>(i)) {}
JsonValue::JsonValue(long long i) : JsonValue(static_cast<double>(i)) {}
JsonValue::JsonValue(const std::string& s) : JsonValue() { m_d->type = Type::String; m_d->strVal = s; }
JsonValue::JsonValue(const char* s) : JsonValue(std::string(s ? s : "")) {}

JsonValue JsonValue::makeObject() {
    JsonValue v;
    v.m_d->type = Type::Object;
    return v;
}
JsonValue JsonValue::makeArray() {
    JsonValue v;
    v.m_d->type = Type::Array;
    return v;
}

JsonValue::Type JsonValue::type() const { return m_d->type; }
bool JsonValue::isNull() const { return m_d->type == Type::Null; }
bool JsonValue::isObject() const { return m_d->type == Type::Object; }
bool JsonValue::isArray() const { return m_d->type == Type::Array; }
bool JsonValue::isString() const { return m_d->type == Type::String; }
bool JsonValue::isNumber() const { return m_d->type == Type::Number; }

bool JsonValue::asBool(bool def) const {
    if (m_d->type == Type::Bool) return m_d->boolVal;
    if (m_d->type == Type::Number) return m_d->numVal != 0;
    return def;
}

double JsonValue::asNumber(double def) const {
    if (m_d->type == Type::Number) return m_d->numVal;
    if (m_d->type == Type::Bool) return m_d->boolVal ? 1 : 0;
    return def;
}

std::string JsonValue::asString(const std::string& def) const {
    if (m_d->type == Type::String) return m_d->strVal;
    if (m_d->type == Type::Number) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", m_d->numVal);
        return buf;
    }
    if (m_d->type == Type::Bool) return m_d->boolVal ? "true" : "false";
    return def;
}

// ---------------- object ----------------
void JsonValue::set(const std::string& key, const JsonValue& v) {
    if (m_d->type != Type::Object) return;
    m_d->objVal[key] = v;
}
void JsonValue::set(const std::string& key, const std::string& v) { set(key, JsonValue(v)); }
void JsonValue::set(const std::string& key, const char* v) { set(key, JsonValue(v)); }
void JsonValue::set(const std::string& key, bool v) { set(key, JsonValue(v)); }
void JsonValue::set(const std::string& key, double v) { set(key, JsonValue(v)); }
void JsonValue::set(const std::string& key, long long v) { set(key, JsonValue(v)); }

bool JsonValue::has(const std::string& key) const {
    return m_d->type == Type::Object && m_d->objVal.count(key) > 0;
}

JsonValue JsonValue::get(const std::string& key) const {
    if (m_d->type != Type::Object) return JsonValue();
    auto it = m_d->objVal.find(key);
    return it == m_d->objVal.end() ? JsonValue() : it->second;
}

std::string JsonValue::getString(const std::string& key, const std::string& def) const {
    JsonValue v = get(key);
    return v.isNull() ? def : v.asString(def);
}
double JsonValue::getNumber(const std::string& key, double def) const {
    JsonValue v = get(key);
    return v.isNull() ? def : v.asNumber(def);
}
bool JsonValue::getBool(const std::string& key, bool def) const {
    JsonValue v = get(key);
    return v.isNull() ? def : v.asBool(def);
}

// ---------------- array ----------------
void JsonValue::push(const JsonValue& v) {
    if (m_d->type == Type::Array) m_d->arrVal.push_back(v);
}
size_t JsonValue::size() const {
    if (m_d->type == Type::Array) return m_d->arrVal.size();
    if (m_d->type == Type::Object) return m_d->objVal.size();
    return 0;
}
const JsonValue& JsonValue::at(size_t i) const {
    static const JsonValue kNull;
    if (m_d->type != Type::Array || i >= m_d->arrVal.size()) return kNull;
    return m_d->arrVal[i];
}

// ---------------- dump ----------------
namespace {

void dumpString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c)); // UTF-8 原样输出
                }
        }
    }
    out.push_back('"');
}

} // namespace

// dump 实现为成员函数（需要访问 Data 里的 objVal 做遍历）
std::string JsonValue::dump() const {
    std::string out;
    switch (m_d->type) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += m_d->boolVal ? "true" : "false"; break;
        case Type::Number: {
            char buf[64];
            if (m_d->numVal == std::floor(m_d->numVal) && std::fabs(m_d->numVal) < 1e15) {
                snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_d->numVal));
            } else {
                snprintf(buf, sizeof(buf), "%.17g", m_d->numVal);
            }
            out += buf;
            break;
        }
        case Type::String:
            dumpString(m_d->strVal, out);
            break;
        case Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < m_d->arrVal.size(); ++i) {
                if (i) out.push_back(',');
                out += m_d->arrVal[i].dump();
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& kv : m_d->objVal) {
                if (!first) out.push_back(',');
                first = false;
                dumpString(kv.first, out);
                out.push_back(':');
                out += kv.second.dump();
            }
            out.push_back('}');
            break;
        }
    }
    return out;
}

// ---------------- parse ----------------
namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : m_s(s) {}

    bool parse(JsonValue& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        return m_i == m_s.size();
    }

private:
    static constexpr int kMaxDepth = 32;

    void skipWs() {
        while (m_i < m_s.size()) {
            char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++m_i;
            else break;
        }
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        skipWs();
        if (m_i >= m_s.size()) return false;
        char c = m_s[m_i];
        switch (c) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out = JsonValue(s);
                return true;
            }
            case 't':
                if (m_s.compare(m_i, 4, "true") == 0) { m_i += 4; out = JsonValue(true); return true; }
                return false;
            case 'f':
                if (m_s.compare(m_i, 5, "false") == 0) { m_i += 5; out = JsonValue(false); return true; }
                return false;
            case 'n':
                if (m_s.compare(m_i, 4, "null") == 0) { m_i += 4; out = JsonValue(); return true; }
                return false;
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        ++m_i; // '{'
        out = JsonValue::makeObject();
        skipWs();
        if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (m_i >= m_s.size() || m_s[m_i] != ':') return false;
            ++m_i;
            JsonValue v;
            if (!parseValue(v, depth + 1)) return false;
            out.set(key, v);
            skipWs();
            if (m_i >= m_s.size()) return false;
            if (m_s[m_i] == ',') { ++m_i; continue; }
            if (m_s[m_i] == '}') { ++m_i; return true; }
            return false;
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        ++m_i; // '['
        out = JsonValue::makeArray();
        skipWs();
        if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
        while (true) {
            JsonValue v;
            if (!parseValue(v, depth + 1)) return false;
            out.push(v);
            skipWs();
            if (m_i >= m_s.size()) return false;
            if (m_s[m_i] == ',') { ++m_i; continue; }
            if (m_s[m_i] == ']') { ++m_i; return true; }
            return false;
        }
    }

    bool parseString(std::string& out) {
        if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
        ++m_i;
        out.clear();
        while (m_i < m_s.size()) {
            unsigned char c = static_cast<unsigned char>(m_s[m_i]);
            if (c == '"') { ++m_i; return true; }
            if (c == '\\') {
                ++m_i;
                if (m_i >= m_s.size()) return false;
                char e = m_s[m_i];
                switch (e) {
                    case '"':  out.push_back('"');  ++m_i; break;
                    case '\\': out.push_back('\\'); ++m_i; break;
                    case '/':  out.push_back('/');  ++m_i; break;
                    case 'n':  out.push_back('\n'); ++m_i; break;
                    case 'r':  out.push_back('\r'); ++m_i; break;
                    case 't':  out.push_back('\t'); ++m_i; break;
                    case 'b':  out.push_back('\b'); ++m_i; break;
                    case 'f':  out.push_back('\f'); ++m_i; break;
                    case 'u': {
                        if (m_i + 4 >= m_s.size()) return false;
                        unsigned code = 0;
                        for (int k = 1; k <= 4; ++k) {
                            char h = m_s[m_i + k];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                            else return false;
                        }
                        m_i += 5;
                        appendUtf8(out, code);
                        break;
                    }
                    default: return false;
                }
                continue;
            }
            out.push_back(static_cast<char>(c));
            ++m_i;
        }
        return false;
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parseNumber(JsonValue& out) {
        const size_t start = m_i;
        if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
        bool any = false;
        while (m_i < m_s.size() && isdigit(static_cast<unsigned char>(m_s[m_i]))) { ++m_i; any = true; }
        if (m_i < m_s.size() && m_s[m_i] == '.') {
            ++m_i;
            while (m_i < m_s.size() && isdigit(static_cast<unsigned char>(m_s[m_i]))) { ++m_i; any = true; }
        }
        if (any && m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
            ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
            while (m_i < m_s.size() && isdigit(static_cast<unsigned char>(m_s[m_i]))) ++m_i;
        }
        if (!any) return false;
        out = JsonValue(strtod(m_s.substr(start, m_i - start).c_str(), nullptr));
        return true;
    }

    const std::string& m_s;
    size_t m_i = 0;
};

} // namespace

JsonValue JsonValue::parse(const std::string& text, bool* ok) {
    Parser p(text);
    JsonValue v;
    const bool success = p.parse(v);
    if (ok) *ok = success;
    return success ? v : JsonValue();
}

namespace jsonutil {

JsonValue makeErrorValue(const std::string& reason) {
    JsonValue v = JsonValue::makeObject();
    v.set("type", "error");
    v.set("reason", reason);
    return v;
}

std::string makeError(const std::string& reason) { return makeErrorValue(reason).dump(); }

} // namespace jsonutil
