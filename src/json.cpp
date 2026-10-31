#include "json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace jisu::json {
namespace {

const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;
const Value kNullValue;

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
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

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool Run(Value& out, std::string* error) {
    SkipSpace();
    bool ok = ParseValue(out, 0);
    if (ok) {
      SkipSpace();
      if (pos_ != text_.size()) {
        ok = Fail("trailing characters");
      }
    }
    if (!ok && error) {
      *error = error_ + " at offset " + std::to_string(pos_);
    }
    return ok;
  }

 private:
  bool Fail(const char* message) {
    error_ = message;
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        break;
      }
      ++pos_;
    }
  }

  bool ParseValue(Value& out, int depth) {
    if (depth > 200) {
      return Fail("nesting too deep");
    }
    if (pos_ >= text_.size()) {
      return Fail("unexpected end");
    }
    switch (text_[pos_]) {
      case '{':
        return ParseObject(out, depth);
      case '[':
        return ParseArray(out, depth);
      case '"': {
        std::string text;
        if (!ParseString(text)) {
          return false;
        }
        out = Value::String(std::move(text));
        return true;
      }
      case 't':
        return ParseLiteral("true", Value::Bool(true), out);
      case 'f':
        return ParseLiteral("false", Value::Bool(false), out);
      case 'n':
        return ParseLiteral("null", Value(), out);
      default:
        return ParseNumber(out);
    }
  }

  bool ParseLiteral(std::string_view word, Value value, Value& out) {
    if (text_.substr(pos_, word.size()) != word) {
      return Fail("invalid literal");
    }
    pos_ += word.size();
    out = std::move(value);
    return true;
  }

  bool ParseNumber(Value& out) {
    const size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      const bool numeric = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
      if (!numeric) {
        break;
      }
      ++pos_;
    }
    if (pos_ == start) {
      return Fail("unexpected character");
    }
    const std::string token(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (!end || *end != '\0') {
      return Fail("invalid number");
    }
    out = Value::Number(value);
    return true;
  }

  bool ParseHex4(uint32_t& value) {
    if (pos_ + 4 > text_.size()) {
      return Fail("bad unicode escape");
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return Fail("bad unicode escape");
      }
    }
    return true;
  }

  bool ParseString(std::string& out) {
    ++pos_;  // 开头的引号
    while (pos_ < text_.size()) {
      // 没有转义的一段整段拷贝：书签、历史这类大文件在首帧前后解析，逐字符追加太慢
      const size_t stop = text_.find_first_of("\"\\", pos_);
      if (stop == std::string_view::npos) {
        break;
      }
      out.append(text_.data() + pos_, stop - pos_);
      pos_ = stop;
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (pos_ >= text_.size()) {
        return Fail("bad escape");
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          uint32_t cp = 0;
          if (!ParseHex4(cp)) {
            return false;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t low = 0;
            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
              pos_ += 2;
              if (!ParseHex4(low)) {
                return false;
              }
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              } else {
                AppendUtf8(out, 0xFFFD);
                cp = low;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("bad escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseArray(Value& out, int depth) {
    ++pos_;
    out = Value::MakeArray();
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      Value item;
      SkipSpace();
      if (!ParseValue(item, depth + 1)) {
        return false;
      }
      out.Push(std::move(item));
      SkipSpace();
      if (pos_ >= text_.size()) {
        return Fail("unterminated array");
      }
      const char c = text_[pos_++];
      if (c == ']') {
        return true;
      }
      if (c != ',') {
        return Fail("expected , or ]");
      }
    }
  }

  bool ParseObject(Value& out, int depth) {
    ++pos_;
    out = Value::MakeObject();
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return Fail("expected key");
      }
      std::string key;
      if (!ParseString(key)) {
        return false;
      }
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return Fail("expected :");
      }
      ++pos_;
      SkipSpace();
      Value item;
      if (!ParseValue(item, depth + 1)) {
        return false;
      }
      out.MutableMembers().emplace_back(std::move(key), std::move(item));
      SkipSpace();
      if (pos_ >= text_.size()) {
        return Fail("unterminated object");
      }
      const char c = text_[pos_++];
      if (c == '}') {
        return true;
      }
      if (c != ',') {
        return Fail("expected , or }");
      }
    }
  }

  std::string_view text_;
  size_t pos_ = 0;
  std::string error_;
};

void AppendNumber(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  char buffer[40];
  int length = 0;
  if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0) {
    length = snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    length = snprintf(buffer, sizeof(buffer), "%.17g", value);
  }
  if (length > 0) {
    out.append(buffer, static_cast<size_t>(length));
  }
}

void SerializeTo(std::string& out, const Value& value) {
  switch (value.type()) {
    case Value::Type::kNull:
      out += "null";
      break;
    case Value::Type::kBool:
      out += value.AsBool() ? "true" : "false";
      break;
    case Value::Type::kNumber:
      AppendNumber(out, value.AsNumber());
      break;
    case Value::Type::kString:
      AppendQuoted(out, value.AsString());
      break;
    case Value::Type::kArray: {
      out.push_back('[');
      const Array& items = value.Items();
      for (size_t i = 0; i < items.size(); ++i) {
        if (i) {
          out.push_back(',');
        }
        SerializeTo(out, items[i]);
      }
      out.push_back(']');
      break;
    }
    case Value::Type::kObject: {
      out.push_back('{');
      const Object& members = value.Members();
      for (size_t i = 0; i < members.size(); ++i) {
        if (i) {
          out.push_back(',');
        }
        AppendQuoted(out, members[i].first);
        out.push_back(':');
        SerializeTo(out, members[i].second);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

Value Value::Bool(bool value) {
  Value v;
  v.type_ = Type::kBool;
  v.bool_ = value;
  return v;
}

Value Value::Number(double value) {
  Value v;
  v.type_ = Type::kNumber;
  v.number_ = value;
  return v;
}

Value Value::String(std::string value) {
  Value v;
  v.type_ = Type::kString;
  v.string_ = std::move(value);
  return v;
}

Value Value::MakeArray() {
  Value v;
  v.type_ = Type::kArray;
  v.array_ = std::make_shared<Array>();
  return v;
}

Value Value::MakeObject() {
  Value v;
  v.type_ = Type::kObject;
  v.object_ = std::make_shared<Object>();
  return v;
}

const std::string& Value::AsString() const { return type_ == Type::kString ? string_ : kEmptyString; }

const Array& Value::Items() const { return type_ == Type::kArray && array_ ? *array_ : kEmptyArray; }

Array& Value::MutableItems() {
  if (type_ != Type::kArray || !array_) {
    *this = MakeArray();
  } else if (array_.use_count() > 1) {
    array_ = std::make_shared<Array>(*array_);  // 写时复制：Value 拷贝之间互不影响
  }
  return *array_;
}

const Object& Value::Members() const { return type_ == Type::kObject && object_ ? *object_ : kEmptyObject; }

Object& Value::MutableMembers() {
  if (type_ != Type::kObject || !object_) {
    *this = MakeObject();
  } else if (object_.use_count() > 1) {
    object_ = std::make_shared<Object>(*object_);
  }
  return *object_;
}

const Value* Value::Find(std::string_view key) const {
  for (const auto& member : Members()) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

const Value& Value::operator[](std::string_view key) const {
  const Value* found = Find(key);
  return found ? *found : kNullValue;
}

void Value::Set(std::string key, Value value) {
  Object& members = MutableMembers();
  for (auto& member : members) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  members.emplace_back(std::move(key), std::move(value));
}

bool Parse(std::string_view text, Value& out, std::string* error) {
  Parser parser(text);
  return parser.Run(out, error);
}

std::string Serialize(const Value& value) {
  std::string out;
  SerializeTo(out, value);
  return out;
}

void AppendQuoted(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (c < 0x20) {
          char escaped[8];
          snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

}  // namespace jisu::json
