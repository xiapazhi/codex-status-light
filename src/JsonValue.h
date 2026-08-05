/**
 * 文件作用：提供 P0 取证工具使用的轻量 JSON 值模型和解析器入口
 * 职责范围：
 * 1. 表达 JSON 对象、数组和基础类型
 * 2. 将单行 JSONL 文本解析为结构化对象
 * 3. 提供安全读取字段的基础能力
 *
 * 不负责：
 * - Codex 事件语义判断
 * - 文件读取和扫描
 *
 * 维护说明：
 * - 该解析器服务于本地 JSONL 取证，必须正确处理字符串转义和嵌套结构
 */
#pragma once

#include <map>
#include <string>
#include <vector>

enum class JsonKind {
    Null,
    Bool,
    Number,
    String,
    Object,
    Array
};

class JsonValue {
public:
    JsonKind kind = JsonKind::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::map<std::string, JsonValue> objectValue;
    std::vector<JsonValue> arrayValue;

    bool IsObject() const;
    bool IsArray() const;
    bool IsString() const;
    bool IsNumber() const;
    bool IsBool() const;
    bool IsNull() const;

    const JsonValue* GetObjectField(const std::string& name) const;
    std::string GetStringOrEmpty() const;
};

class JsonParser {
public:
    bool Parse(const std::string& source, JsonValue* output, std::string* error);

private:
    const std::string* source_ = nullptr;
    size_t position_ = 0;

    bool ParseValue(JsonValue* output, std::string* error);
    bool ParseObject(JsonValue* output, std::string* error);
    bool ParseArray(JsonValue* output, std::string* error);
    bool ParseString(std::string* output, std::string* error);
    bool ParseNumber(JsonValue* output, std::string* error);
    bool ParseLiteral(const std::string& literal, JsonKind kind, JsonValue* output, std::string* error);

    void SkipWhitespace();
    bool Consume(char expected);
    bool IsAtEnd() const;
    char Peek() const;
    char Advance();
    bool AppendUnicodeEscape(std::string* output, std::string* error);
};
