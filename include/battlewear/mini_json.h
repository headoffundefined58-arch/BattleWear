// BattleWear - minimal JSON reader (header only).
// Used only at config-load time; the per-frame runtime path performs no allocation.
// Supports: null / bool / number / string / array / object, with \uXXXX escapes.
#ifndef BATTLEWEAR_MINI_JSON_H
#define BATTLEWEAR_MINI_JSON_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bw {
namespace mj {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Value::Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    const Value* Find(const std::string& key) const {
        if (type != Value::Type::Object) return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
    bool Has(const std::string& key) const { return Find(key) != nullptr; }
    double Number(double fallback = 0.0) const { return type == Value::Type::Number ? number : fallback; }
    bool Bool(bool fallback = false) const { return type == Value::Type::Bool ? boolean : fallback; }
    std::string Text(const std::string& fallback = std::string()) const {
        return type == Value::Type::String ? text : fallback;
    }
    int Integer(int fallback = 0) const { return type == Value::Type::Number ? static_cast<int>(number) : fallback; }
    const std::vector<Value>& Array() const { return array; }
    const std::map<std::string, Value>& Object() const { return object; }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool Parse(Value& out, std::string& error) {
        SkipWs();
        if (!ParseValue(out, error)) return false;
        SkipWs();
        if (pos_ != s_.size()) { Fail(error, "trailing content"); return false; }
        return true;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void Fail(std::string& error, const char* what) {
        error = std::string(what) + " at byte " + std::to_string(pos_);
    }
    void SkipWs() {
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos_;
            else break;
        }
    }
    bool Eat(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    bool Literal(const char* lit) {
        size_t n = 0;
        while (lit[n]) ++n;
        if (s_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }
    bool ParseValue(Value& out, std::string& error) {
        SkipWs();
        if (pos_ >= s_.size()) { Fail(error, "unexpected end"); return false; }
        char c = s_[pos_];
        if (c == '{') return ParseObject(out, error);
        if (c == '[') return ParseArray(out, error);
        if (c == '"') { out.type = Value::Type::String; return ParseString(out.text, error); }
        if (c == 't') { if (!Literal("true")) { Fail(error, "bad literal"); return false; } out.type = Value::Type::Bool; out.boolean = true; return true; }
        if (c == 'f') { if (!Literal("false")) { Fail(error, "bad literal"); return false; } out.type = Value::Type::Bool; out.boolean = false; return true; }
        if (c == 'n') { if (!Literal("null")) { Fail(error, "bad literal"); return false; } out.type = Value::Type::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out, error);
        Fail(error, "unexpected character");
        return false;
    }
    bool ParseNumber(Value& out, std::string& error) {
        size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ == start) { Fail(error, "bad number"); return false; }
        out.type = Value::Type::Number;
        out.number = std::stod(s_.substr(start, pos_ - start));
        return true;
    }
    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool ParseString(std::string& out, std::string& error) {
        if (!Eat('"')) { Fail(error, "expected string"); return false; }
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) break;
                char e = s_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!Hex4(cp)) { Fail(error, "bad unicode escape"); return false; }
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t lo = 0;
                            if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) { Fail(error, "bad surrogate pair"); return false; }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: Fail(error, "bad escape"); return false;
                }
            } else {
                out.push_back(c);
            }
        }
        Fail(error, "unterminated string");
        return false;
    }
    bool ParseArray(Value& out, std::string& error) {
        Eat('[');
        out.type = Value::Type::Array;
        SkipWs();
        if (Eat(']')) return true;
        for (;;) {
            Value item;
            if (!ParseValue(item, error)) return false;
            out.array.push_back(item);
            SkipWs();
            if (Eat(']')) return true;
            if (!Eat(',')) { Fail(error, "expected ',' or ']'"); return false; }
            SkipWs();
        }
    }
    bool ParseObject(Value& out, std::string& error) {
        Eat('{');
        out.type = Value::Type::Object;
        SkipWs();
        if (Eat('}')) return true;
        for (;;) {
            SkipWs();
            std::string key;
            if (!ParseString(key, error)) return false;
            SkipWs();
            if (!Eat(':')) { Fail(error, "expected ':'"); return false; }
            Value v;
            if (!ParseValue(v, error)) return false;
            out.object[key] = v;
            SkipWs();
            if (Eat('}')) return true;
            if (!Eat(',')) { Fail(error, "expected ',' or '}'"); return false; }
        }
    }
};

inline bool Parse(const std::string& text, Value& out, std::string& error) {
    Parser p(text);
    return p.Parse(out, error);
}

} // namespace mj
} // namespace bw

#endif // BATTLEWEAR_MINI_JSON_H
