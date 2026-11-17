#include "core/grpc_template.h"

#include <google/protobuf/descriptor.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace watt::grpc_detail {
namespace {

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using Writer = rapidjson::PrettyWriter<rapidjson::StringBuffer>;

class TemplateBuilder {
 public:
  TemplateBuilder(Writer& w, std::vector<std::string>* notes, bool examples)
      : w_(w), notes_(notes), examples_(examples) {}

  void message(const Descriptor* d) {
    if (well_known(d)) return;
    w_.StartObject();
    fields(d);
    w_.EndObject();
  }

 private:
  // The fields of d, inside an object the caller opened.
  void fields(const Descriptor* d) {
    stack_.push_back(d);
    std::vector<const google::protobuf::OneofDescriptor*> seen_oneofs;
    for (int i = 0; i < d->field_count(); ++i) {
      const FieldDescriptor* f = d->field(i);
      if (const auto* o = f->real_containing_oneof()) {
        bool seen = false;
        for (auto* s : seen_oneofs) seen |= (s == o);
        if (seen) continue;
        seen_oneofs.push_back(o);
        note_oneof(o, f);
      }
      ++fields_;
      w_.Key(f->json_name().data(), static_cast<rapidjson::SizeType>(f->json_name().size()), true);
      path_.push_back(std::string(f->json_name()) + (f->is_repeated() && !f->is_map() ? "[]" : ""));
      // Scalars with explicit presence (proto3 `optional`, proto2): sending the zero value is not
      // the same as leaving the field out.
      if (f->has_presence() && !f->real_containing_oneof() && !is_message(f))
        note(path() + ": optional, leave it out to send it unset (the " + std::string(kind()) + " sets it)");
      field_ = f;
      field(f);
      path_.pop_back();
    }
    stack_.pop_back();
  }

  static constexpr int kMaxFields = 4000;  // keeps huge interlinked schemas from exploding

  // A message type may be expanded twice on the current path (one level of recursion, as
  // grpcurl does); deeper self-references are left unset.
  bool can_expand(const Descriptor* d) const {
    if (fields_ > kMaxFields) return false;
    int n = 0;
    for (const Descriptor* s : stack_) n += (s == d);
    return n < 2;
  }
  static bool is_message(const FieldDescriptor* f) { return f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE; }

  void field(const FieldDescriptor* f) {
    if (f->is_map()) {
      const Descriptor* entry = f->message_type();
      const FieldDescriptor* v = entry->map_value();
      w_.StartObject();
      if (!is_message(v) || can_expand(v->message_type())) {
        map_key(entry->map_key());
        value(v);
      }
      w_.EndObject();
    } else if (f->is_repeated()) {
      w_.StartArray();
      if (!is_message(f) || can_expand(f->message_type())) value(f);
      w_.EndArray();
    } else if (is_message(f) && !can_expand(f->message_type())) {
      w_.Null();  // unset message: valid proto3 JSON
    } else {
      value(f);
    }
  }

  void map_key(const FieldDescriptor* k) {
    switch (k->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING: w_.Key("key"); break;
      case FieldDescriptor::CPPTYPE_BOOL: w_.Key(examples_ ? "true" : "false"); break;
      default: w_.Key(examples_ ? "1" : "0"); break;
    }
  }

  // Example mode: a plausible value from the field's name, so the request means something as is.
  static std::string lower(std::string_view name) {
    std::string n(name);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return n;
  }
  void example_string(const FieldDescriptor* f) {
    const std::string n = f ? lower(f->name()) : std::string();
    auto has = [&](const char* part) { return n.find(part) != std::string::npos; };
    auto ends = [&](std::string_view suffix) {
      return n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    const char* v = "Hello";
    if (has("uuid") || ends("_id") || n == "id" || ends("ids")) v = "9b2d6f4e-3c1a-4b7e-8f5d-2a6c0e1b7d93";
    else if (has("email")) v = "user@example.com";
    else if (has("url") || has("uri") || has("link")) v = "https://example.com";
    else if (has("phone")) v = "+15555550100";
    else if (has("token")) v = "token";
    else if (has("name")) v = "example";
    w_.String(v);
  }

  void value(const FieldDescriptor* f) {
    switch (f->cpp_type()) {
      case FieldDescriptor::CPPTYPE_INT32:
      case FieldDescriptor::CPPTYPE_UINT32: w_.Int(examples_ ? 10 : 0); break;
      case FieldDescriptor::CPPTYPE_INT64:
      case FieldDescriptor::CPPTYPE_UINT64: w_.String(examples_ ? "10" : "0"); break;  // proto3 JSON: 64-bit as strings
      case FieldDescriptor::CPPTYPE_DOUBLE:
      case FieldDescriptor::CPPTYPE_FLOAT:
        if (examples_) w_.Double(1.5);
        else w_.Int(0);
        break;
      case FieldDescriptor::CPPTYPE_BOOL: w_.Bool(examples_); break;
      case FieldDescriptor::CPPTYPE_STRING:
        if (!examples_) w_.String("");
        else if (f->type() == FieldDescriptor::TYPE_BYTES) w_.String("SGVsbG8=");  // base64 of "Hello"
        else example_string(f);
        break;
      case FieldDescriptor::CPPTYPE_ENUM: {
        const auto* e = f->enum_type();
        if (e->full_name() == "google.protobuf.NullValue" || e->value_count() == 0) {
          w_.Null();
        } else {
          // Examples skip a zero "unspecified" value when the enum has a real one.
          const int pick = enum_pick(e);
          std::string_view n = e->value(pick)->name();
          w_.String(n.data(), static_cast<rapidjson::SizeType>(n.size()), true);
          note_enum(e);
        }
        break;
      }
      case FieldDescriptor::CPPTYPE_MESSAGE: message(f->message_type()); break;
    }
  }

  const char* kind() const { return examples_ ? "example" : "template"; }
  // The enum value written: examples skip a zero "unspecified" value when the enum has a real one.
  int enum_pick(const google::protobuf::EnumDescriptor* e) const {
    return examples_ && e->value_count() > 1 && e->value(0)->number() == 0 ? 1 : 0;
  }

  std::string path() const {
    std::string p;
    for (const auto& s : path_) p += (p.empty() ? "" : ".") + s;
    return p;
  }
  void note(std::string text) {
    if (notes_ && notes_->size() < 64) notes_->push_back(std::move(text));
  }
  void note_oneof(const google::protobuf::OneofDescriptor* o, const FieldDescriptor* used) {
    if (!notes_) return;
    std::string alts;
    for (int i = 0; i < o->field_count(); ++i) alts += (i ? " | " : "") + std::string(o->field(i)->json_name());
    std::string where = path();
    note((where.empty() ? "" : where + ".") + std::string(o->name()) + ": oneof, set exactly one of " + alts +
         " (" + std::string(kind()) + " uses " + std::string(used->json_name()) + ")");
  }

  void note_enum(const google::protobuf::EnumDescriptor* e) {
    if (!notes_ || e->value_count() < 2) return;
    std::string alts;
    const int shown = std::min(e->value_count(), 16);
    for (int i = 0; i < shown; ++i) alts += (i ? " | " : "") + std::string(e->value(i)->name());
    if (shown < e->value_count()) alts += " | ... (" + std::to_string(e->value_count()) + " values)";
    note(path() + ": enum " + std::string(e->full_name()) + ", one of " + alts + " (" + std::string(kind()) + " uses " +
         std::string(e->value(enum_pick(e))->name()) + ")");
  }

  // The message an Any packs: the template sends google.protobuf.Empty, which every receiver can
  // decode; examples pack a real message of the request's own file (its fields filled too), so
  // the request can be sent unedited either way.
  const Descriptor* any_pick() const {
    if (!examples_ || stack_.empty()) return nullptr;
    const auto* file = stack_.front()->file();
    for (int i = 0; i < file->message_type_count(); ++i) {
      const Descriptor* m = file->message_type(i);
      if (m->field_count() == 0 || m->map_key() != nullptr || fields_ > kMaxFields ||
          std::find(stack_.begin(), stack_.end(), m) != stack_.end())
        continue;
      return m;
    }
    return nullptr;
  }
  void any() {
    const Descriptor* m = any_pick();
    any_pick_name_ = m ? std::string(m->full_name()) : "google.protobuf.Empty";
    const std::string url = "type.googleapis.com/" + any_pick_name_;
    w_.StartObject();
    w_.Key("@type");
    w_.String(url.c_str(), static_cast<rapidjson::SizeType>(url.size()), true);
    if (m) {
      const std::string name = any_pick_name_;
      fields(m);
      any_pick_name_ = name;  // nested Anys inside m may have picked another
    }
    w_.EndObject();
  }

  // Writes the JSON form of a well-known type and returns true, or returns false.
  bool well_known(const Descriptor* d) {
    std::string_view n = d->full_name();
    if (n.substr(0, 16) != "google.protobuf.") return false;
    std::string_view s = n.substr(16);
    if (s == "Timestamp") w_.String(examples_ ? "2024-01-01T12:00:00Z" : "1970-01-01T00:00:00Z");
    else if (s == "Duration") w_.String(examples_ ? "1.5s" : "0s");
    else if (s == "FieldMask") w_.String("");
    else if (s == "Struct" && examples_) { w_.StartObject(); w_.Key("key"); w_.String("Hello"); w_.EndObject(); }
    else if (s == "Any") any();
    else if (s == "Struct" || s == "Empty") { w_.StartObject(); w_.EndObject(); }
    else if (s == "ListValue") { w_.StartArray(); if (examples_) w_.String("Hello"); w_.EndArray(); }
    else if (s == "Value") { if (examples_) w_.String("Hello"); else w_.Null(); }
    else if (s == "DoubleValue" || s == "FloatValue") {
      if (examples_) w_.Double(1.5);
      else w_.Int(0);
    } else if (s == "Int32Value" || s == "UInt32Value") w_.Int(examples_ ? 10 : 0);
    else if (s == "Int64Value" || s == "UInt64Value") w_.String(examples_ ? "10" : "0");
    else if (s == "BoolValue") w_.Bool(examples_);
    else if (s == "StringValue") {
      if (examples_) example_string(field_);
      else w_.String("");
    } else if (s == "BytesValue") w_.String(examples_ ? "SGVsbG8=" : "");
    else return false;
    if (s == "Struct") note(path() + ": google.protobuf.Struct, any JSON object");
    else if (s == "Value") note(path() + ": google.protobuf.Value, any JSON value");
    else if (s == "ListValue") note(path() + ": google.protobuf.ListValue, any JSON array");
    else if (s == "Any")
      note(path() + ": google.protobuf.Any, {\"@type\": \"type.googleapis.com/<message>\", ...its fields} (" +
           std::string(kind()) + " packs " + any_pick_name_ + ")");
    else if (s == "FieldMask") note(path() + ": google.protobuf.FieldMask, comma-separated lowerCamel paths");
    return true;
  }

  Writer& w_;
  std::vector<std::string>* notes_;
  bool examples_ = false;
  const FieldDescriptor* field_ = nullptr;  // the field being written (example strings use its name)
  std::vector<const Descriptor*> stack_;
  std::vector<std::string> path_;
  int fields_ = 0;
  std::string any_pick_name_ = "google.protobuf.Empty";
};

}  // namespace

std::string build_request_template(const Descriptor* d, std::vector<std::string>* notes, bool examples) {
  if (!d) return "{}";
  rapidjson::StringBuffer buf;
  Writer w(buf);
  w.SetIndent(' ', 2);
  TemplateBuilder(w, notes, examples).message(d);
  return std::string(buf.GetString(), buf.GetSize());
}

}  // namespace watt::grpc_detail
