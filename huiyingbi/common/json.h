#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

// 极简 JSON（无第三方依赖）：
//   - 支持 null / bool / number / string / array / object
//   - 支持 \uXXXX 转义（转成 UTF-8）
//   - 只用于本项目的前后端消息，够用即可，不做完整 RFC 校验
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue();
    JsonValue(bool b);
    JsonValue(double d);
    JsonValue(int i);
    JsonValue(long long i);
    JsonValue(const std::string& s);
    JsonValue(const char* s);

    static JsonValue makeObject();
    static JsonValue makeArray();

    Type type() const;
    bool isNull() const;
    bool isObject() const;
    bool isArray() const;
    bool isString() const;
    bool isNumber() const;

    bool asBool(bool def = false) const;
    double asNumber(double def = 0) const;
    std::string asString(const std::string& def = std::string()) const;

    // ---- object 访问 ----
    void set(const std::string& key, const JsonValue& v);
    void set(const std::string& key, const std::string& v);
    void set(const std::string& key, const char* v);
    void set(const std::string& key, bool v);
    void set(const std::string& key, double v);
    void set(const std::string& key, long long v);
    bool has(const std::string& key) const;
    JsonValue get(const std::string& key) const; // 不存在返回 Null
    std::string getString(const std::string& key, const std::string& def = std::string()) const;
    double getNumber(const std::string& key, double def = 0) const;
    bool getBool(const std::string& key, bool def = false) const;

    // ---- array 访问 ----
    void push(const JsonValue& v);
    size_t size() const;
    const JsonValue& at(size_t i) const;

    // ---- 序列化 / 反序列化 ----
    std::string dump() const;
    static JsonValue parse(const std::string& text, bool* ok = nullptr);

private:
    struct Data;
    std::shared_ptr<Data> m_d;
};

// 便捷构造
namespace jsonutil {
// 生成 {"type": t, ...} 形式的消息
std::string makeError(const std::string& reason);
JsonValue makeErrorValue(const std::string& reason);
} // namespace jsonutil
