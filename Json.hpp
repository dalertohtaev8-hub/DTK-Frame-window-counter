#pragma once
// Small strict JSON parser for Window Counter macro files.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace wcj {

struct V {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<V> a;
    std::vector<std::pair<std::string, V>> o;

    V const* get(char const* key) const {
        if (t != Obj) return nullptr;
        for (auto const& item : o) if (item.first == key) return &item.second;
        return nullptr;
    }
};

struct Parser {
    std::string const& input;
    size_t pos = 0;
    bool ok = true;

    explicit Parser(std::string const& text) : input(text) {}

    void ws() {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\n' || input[pos] == '\r')) ++pos;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
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

    bool hex4(unsigned& value) {
        if (pos + 4 > input.size()) return false;
        value = 0;
        for (int j = 0; j < 4; ++j) {
            char c = input[pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    std::string parseString() {
        std::string out;
        if (pos >= input.size() || input[pos++] != '"') { ok = false; return out; }
        while (pos < input.size()) {
            unsigned char c = static_cast<unsigned char>(input[pos++]);
            if (c == '"') return out;
            if (c < 0x20) { ok = false; return {}; }
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos >= input.size()) break;
            char e = input[pos++];
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
                    unsigned cp = 0;
                    if (!hex4(cp)) { ok = false; return {}; }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos + 2 > input.size() || input[pos++] != '\\' || input[pos++] != 'u') { ok = false; return {}; }
                        unsigned low = 0;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) { ok = false; return {}; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) { ok = false; return {}; }
                    appendUtf8(out, cp);
                    break;
                }
                default: ok = false; return {};
            }
        }
        ok = false;
        return {};
    }

    V value(unsigned depth = 0) {
        V v;
        ws();
        if (pos >= input.size() || depth > 64) { ok = false; return v; }
        char c = input[pos];
        if (c == '{') {
            v.t = V::Obj; ++pos; ws();
            if (pos < input.size() && input[pos] == '}') { ++pos; return v; }
            while (ok) {
                ws();
                if (pos >= input.size() || input[pos] != '"') { ok = false; break; }
                auto key = parseString(); ws();
                if (pos >= input.size() || input[pos++] != ':') { ok = false; break; }
                auto item = value(depth + 1);
                v.o.emplace_back(std::move(key), std::move(item));
                ws();
                if (pos < input.size() && input[pos] == ',') { ++pos; continue; }
                if (pos < input.size() && input[pos] == '}') { ++pos; break; }
                ok = false;
            }
            return v;
        }
        if (c == '[') {
            v.t = V::Arr; ++pos; ws();
            if (pos < input.size() && input[pos] == ']') { ++pos; return v; }
            while (ok) {
                v.a.push_back(value(depth + 1)); ws();
                if (pos < input.size() && input[pos] == ',') { ++pos; continue; }
                if (pos < input.size() && input[pos] == ']') { ++pos; break; }
                ok = false;
            }
            return v;
        }
        if (c == '"') { v.t = V::Str; v.s = parseString(); return v; }
        if (input.compare(pos, 4, "true") == 0) { pos += 4; v.t = V::Bool; v.b = true; return v; }
        if (input.compare(pos, 5, "false") == 0) { pos += 5; v.t = V::Bool; return v; }
        if (input.compare(pos, 4, "null") == 0) { pos += 4; return v; }

        size_t start = pos;
        if (input[pos] == '-') ++pos;
        if (pos >= input.size()) { ok = false; return v; }
        if (input[pos] == '0') ++pos;
        else if (input[pos] >= '1' && input[pos] <= '9') while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
        else { ok = false; return v; }
        if (pos < input.size() && input[pos] == '.') {
            ++pos; size_t digits = pos;
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (digits == pos) { ok = false; return v; }
        }
        if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
            ++pos; if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) ++pos;
            size_t digits = pos;
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (digits == pos) { ok = false; return v; }
        }
        char* end = nullptr;
        double number = std::strtod(input.c_str() + start, &end);
        if (end != input.c_str() + pos || !std::isfinite(number)) { ok = false; return v; }
        v.t = V::Num; v.n = number;
        return v;
    }
};

inline bool parse(std::string const& text, V& out) {
    if (text.size() > 32 * 1024 * 1024) return false;
    size_t start = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                   static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF ? 3 : 0;
    Parser parser(text);
    parser.pos = start;
    out = parser.value();
    parser.ws();
    return parser.ok && parser.pos == text.size();
}

} // namespace wcj
