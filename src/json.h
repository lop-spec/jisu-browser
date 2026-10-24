// 小型 JSON：解析成树、序列化回字符串。字符串一律 UTF-8。
// 用于智能体桥的 relay 消息与书签文件；CDP 参数和结果大多直接按原文拼接转发，不经这里。
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jisu::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // 保留键的原始顺序

class Value {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Value() = default;
  static Value Bool(bool value);
  static Value Number(double value);
  static Value String(std::string value);
  static Value MakeArray();
  static Value MakeObject();

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::kNull; }
  bool IsBool() const { return type_ == Type::kBool; }
  bool IsNumber() const { return type_ == Type::kNumber; }
  bool IsString() const { return type_ == Type::kString; }
  bool IsArray() const { return type_ == Type::kArray; }
  bool IsObject() const { return type_ == Type::kObject; }

  bool AsBool(bool fallback = false) const { return type_ == Type::kBool ? bool_ : fallback; }
  double AsNumber(double fallback = 0) const { return type_ == Type::kNumber ? number_ : fallback; }
  const std::string& AsString() const;  // 非字符串返回空串

  // 数组
  const Array& Items() const;  // 非数组返回空数组
  Array& MutableItems();       // 先转成数组
  void Push(Value value) { MutableItems().push_back(std::move(value)); }

  // 对象
  const Object& Members() const;  // 非对象返回空对象
  Object& MutableMembers();       // 先转成对象
  const Value* Find(std::string_view key) const;
  const Value& operator[](std::string_view key) const;  // 缺失返回 null
  void Set(std::string key, Value value);

 private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  double number_ = 0;
  std::string string_;
  std::shared_ptr<Array> array_;
  std::shared_ptr<Object> object_;
};

bool Parse(std::string_view text, Value& out, std::string* error = nullptr);
std::string Serialize(const Value& value);
void AppendQuoted(std::string& out, std::string_view text);  // 按 JSON 规则转义并加引号

}  // namespace jisu::json
