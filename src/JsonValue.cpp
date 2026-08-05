/**
 * 文件作用：实现 P0 取证工具使用的轻量 JSON 解析器
 * 职责范围：
 * 1. 解析 JSONL 单行文本
 * 2. 处理字符串转义、Unicode 转义、对象和数组
 * 3. 返回可诊断的解析错误
 *
 * 不负责：
 * - 通过正则表达式猜测 JSON 结构
 * - 保存或输出 Codex 对话正文
 *
 * 维护说明：
 * - 解析失败只影响当前行，调用方必须继续处理后续 JSONL 行
 */
#include "JsonValue.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace {

bool IsHex(char value)
{
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

int HexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + value - 'a';
    }
    return 10 + value - 'A';
}

void AppendUtf8(uint32_t codePoint, std::string* output)
{
    if (codePoint <= 0x7F) {
        output->push_back(static_cast<char>(codePoint));
        return;
    }

    if (codePoint <= 0x7FF) {
        output->push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
        output->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        return;
    }

    if (codePoint <= 0xFFFF) {
        output->push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
        output->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        return;
    }

    output->push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
    output->push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
}

} // namespace

bool JsonValue::IsObject() const
{
    return kind == JsonKind::Object;
}

bool JsonValue::IsArray() const
{
    return kind == JsonKind::Array;
}

bool JsonValue::IsString() const
{
    return kind == JsonKind::String;
}

bool JsonValue::IsNumber() const
{
    return kind == JsonKind::Number;
}

bool JsonValue::IsBool() const
{
    return kind == JsonKind::Bool;
}

bool JsonValue::IsNull() const
{
    return kind == JsonKind::Null;
}

const JsonValue* JsonValue::GetObjectField(const std::string& name) const
{
    if (!IsObject()) {
        return nullptr;
    }

    const auto item = objectValue.find(name);
    if (item == objectValue.end()) {
        return nullptr;
    }

    return &item->second;
}

std::string JsonValue::GetStringOrEmpty() const
{
    if (!IsString()) {
        return std::string();
    }

    return stringValue;
}

bool JsonParser::Parse(const std::string& source, JsonValue* output, std::string* error)
{
    source_ = &source;
    position_ = 0;

    if (!ParseValue(output, error)) {
        return false;
    }

    SkipWhitespace();
    if (!IsAtEnd()) {
        if (error != nullptr) {
            *error = "unexpected trailing characters";
        }
        return false;
    }

    return true;
}

bool JsonParser::ParseValue(JsonValue* output, std::string* error)
{
    SkipWhitespace();
    if (IsAtEnd()) {
        if (error != nullptr) {
            *error = "unexpected end of input";
        }
        return false;
    }

    const char current = Peek();
    if (current == '{') {
        return ParseObject(output, error);
    }
    if (current == '[') {
        return ParseArray(output, error);
    }
    if (current == '"') {
        output->kind = JsonKind::String;
        return ParseString(&output->stringValue, error);
    }
    if (current == '-' || (current >= '0' && current <= '9')) {
        return ParseNumber(output, error);
    }
    if (current == 't') {
        return ParseLiteral("true", JsonKind::Bool, output, error);
    }
    if (current == 'f') {
        return ParseLiteral("false", JsonKind::Bool, output, error);
    }
    if (current == 'n') {
        return ParseLiteral("null", JsonKind::Null, output, error);
    }

    if (error != nullptr) {
        *error = "unexpected JSON token";
    }
    return false;
}

bool JsonParser::ParseObject(JsonValue* output, std::string* error)
{
    output->kind = JsonKind::Object;
    output->objectValue.clear();

    Consume('{');
    SkipWhitespace();
    if (Consume('}')) {
        return true;
    }

    while (!IsAtEnd()) {
        std::string key;
        if (!ParseString(&key, error)) {
            return false;
        }

        SkipWhitespace();
        if (!Consume(':')) {
            if (error != nullptr) {
                *error = "expected ':' after object key";
            }
            return false;
        }

        JsonValue value;
        if (!ParseValue(&value, error)) {
            return false;
        }

        output->objectValue[key] = std::move(value);
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        if (!Consume(',')) {
            if (error != nullptr) {
                *error = "expected ',' or '}' in object";
            }
            return false;
        }
        SkipWhitespace();
    }

    if (error != nullptr) {
        *error = "unterminated object";
    }
    return false;
}

bool JsonParser::ParseArray(JsonValue* output, std::string* error)
{
    output->kind = JsonKind::Array;
    output->arrayValue.clear();

    Consume('[');
    SkipWhitespace();
    if (Consume(']')) {
        return true;
    }

    while (!IsAtEnd()) {
        JsonValue value;
        if (!ParseValue(&value, error)) {
            return false;
        }

        output->arrayValue.push_back(std::move(value));
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        if (!Consume(',')) {
            if (error != nullptr) {
                *error = "expected ',' or ']' in array";
            }
            return false;
        }
        SkipWhitespace();
    }

    if (error != nullptr) {
        *error = "unterminated array";
    }
    return false;
}

bool JsonParser::ParseString(std::string* output, std::string* error)
{
    output->clear();

    if (!Consume('"')) {
        if (error != nullptr) {
            *error = "expected string";
        }
        return false;
    }

    while (!IsAtEnd()) {
        const char current = Advance();
        if (current == '"') {
            return true;
        }

        if (static_cast<unsigned char>(current) < 0x20) {
            if (error != nullptr) {
                *error = "control character in string";
            }
            return false;
        }

        if (current != '\\') {
            output->push_back(current);
            continue;
        }

        if (IsAtEnd()) {
            if (error != nullptr) {
                *error = "unterminated string escape";
            }
            return false;
        }

        const char escaped = Advance();
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            output->push_back(escaped);
            break;
        case 'b':
            output->push_back('\b');
            break;
        case 'f':
            output->push_back('\f');
            break;
        case 'n':
            output->push_back('\n');
            break;
        case 'r':
            output->push_back('\r');
            break;
        case 't':
            output->push_back('\t');
            break;
        case 'u':
            if (!AppendUnicodeEscape(output, error)) {
                return false;
            }
            break;
        default:
            if (error != nullptr) {
                *error = "invalid string escape";
            }
            return false;
        }
    }

    if (error != nullptr) {
        *error = "unterminated string";
    }
    return false;
}

bool JsonParser::ParseNumber(JsonValue* output, std::string* error)
{
    const size_t start = position_;

    if (Peek() == '-') {
        Advance();
    }

    if (IsAtEnd()) {
        if (error != nullptr) {
            *error = "incomplete number";
        }
        return false;
    }

    if (Peek() == '0') {
        Advance();
    } else if (Peek() >= '1' && Peek() <= '9') {
        while (!IsAtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            Advance();
        }
    } else {
        if (error != nullptr) {
            *error = "invalid number";
        }
        return false;
    }

    if (!IsAtEnd() && Peek() == '.') {
        Advance();
        if (IsAtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
            if (error != nullptr) {
                *error = "invalid number fraction";
            }
            return false;
        }
        while (!IsAtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            Advance();
        }
    }

    if (!IsAtEnd() && (Peek() == 'e' || Peek() == 'E')) {
        Advance();
        if (!IsAtEnd() && (Peek() == '+' || Peek() == '-')) {
            Advance();
        }
        if (IsAtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
            if (error != nullptr) {
                *error = "invalid number exponent";
            }
            return false;
        }
        while (!IsAtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            Advance();
        }
    }

    const std::string numberText = source_->substr(start, position_ - start);
    output->kind = JsonKind::Number;
    output->numberValue = std::strtod(numberText.c_str(), nullptr);
    return true;
}

bool JsonParser::ParseLiteral(const std::string& literal, JsonKind kind, JsonValue* output, std::string* error)
{
    if (source_->compare(position_, literal.size(), literal) != 0) {
        if (error != nullptr) {
            *error = "invalid literal";
        }
        return false;
    }

    position_ += literal.size();
    output->kind = kind;
    if (literal == "true") {
        output->boolValue = true;
    } else if (literal == "false") {
        output->boolValue = false;
    }
    return true;
}

void JsonParser::SkipWhitespace()
{
    while (!IsAtEnd()) {
        const char current = Peek();
        if (current != ' ' && current != '\n' && current != '\r' && current != '\t') {
            return;
        }
        Advance();
    }
}

bool JsonParser::Consume(char expected)
{
    if (IsAtEnd() || Peek() != expected) {
        return false;
    }

    Advance();
    return true;
}

bool JsonParser::IsAtEnd() const
{
    return source_ == nullptr || position_ >= source_->size();
}

char JsonParser::Peek() const
{
    return (*source_)[position_];
}

char JsonParser::Advance()
{
    return (*source_)[position_++];
}

bool JsonParser::AppendUnicodeEscape(std::string* output, std::string* error)
{
    if (position_ + 4 > source_->size()) {
        if (error != nullptr) {
            *error = "incomplete unicode escape";
        }
        return false;
    }

    uint32_t codePoint = 0;
    for (int index = 0; index < 4; ++index) {
        const char value = Advance();
        if (!IsHex(value)) {
            if (error != nullptr) {
                *error = "invalid unicode escape";
            }
            return false;
        }
        codePoint = (codePoint << 4) | static_cast<uint32_t>(HexValue(value));
    }

    // JSON 中的非 BMP 字符会用代理对表示；这里合并后再写入 UTF-8，避免路径或事件字段被破坏。
    if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
        if (position_ + 6 > source_->size() || Advance() != '\\' || Advance() != 'u') {
            if (error != nullptr) {
                *error = "missing low surrogate";
            }
            return false;
        }

        uint32_t low = 0;
        for (int index = 0; index < 4; ++index) {
            const char value = Advance();
            if (!IsHex(value)) {
                if (error != nullptr) {
                    *error = "invalid low surrogate";
                }
                return false;
            }
            low = (low << 4) | static_cast<uint32_t>(HexValue(value));
        }

        if (low < 0xDC00 || low > 0xDFFF) {
            if (error != nullptr) {
                *error = "invalid low surrogate range";
            }
            return false;
        }

        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
    }

    AppendUtf8(codePoint, output);
    return true;
}
