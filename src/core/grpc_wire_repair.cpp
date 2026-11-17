#include "core/grpc_wire_repair.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace watt::detail {

namespace pb = google::protobuf;

namespace {

// Length of the valid UTF-8 sequence at s[0], or 0 when s[0] does not start one.
size_t utf8_seq(const unsigned char* s, size_t n) {
  const unsigned c = s[0];
  if (c < 0x80) return 1;
  auto cont = [&](size_t i, unsigned lo = 0x80, unsigned hi = 0xBF) { return i < n && s[i] >= lo && s[i] <= hi; };
  if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
  if (c == 0xE0) return cont(1, 0xA0) && cont(2) ? 3 : 0;
  if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) return cont(1) && cont(2) ? 3 : 0;
  if (c == 0xED) return cont(1, 0x80, 0x9F) && cont(2) ? 3 : 0;  // no UTF-16 surrogates
  if (c == 0xF0) return cont(1, 0x90) && cont(2) && cont(3) ? 4 : 0;
  if (c >= 0xF1 && c <= 0xF3) return cont(1) && cont(2) && cont(3) ? 4 : 0;
  if (c == 0xF4) return cont(1, 0x80, 0x8F) && cont(2) && cont(3) ? 4 : 0;
  return 0;
}

void put_varint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out += char((v & 0x7F) | 0x80);
    v >>= 7;
  }
  out += char(v);
}

struct Walker {
  std::vector<std::string>& notes;
  std::string& error;
  const char* begin;  // start of the whole message, for byte offsets in errors

  size_t off(const char* p) const { return size_t(p - begin); }

  bool fail(const char* at, const std::string& what) {
    error = what + " at byte " + std::to_string(off(at));
    return false;
  }

  bool varint(const char*& p, const char* end, uint64_t& v) {
    const char* start = p;
    v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
      if (p >= end) return fail(start, "truncated varint");
      const auto b = static_cast<unsigned char>(*p++);
      if (shift == 63 && b > 1) return fail(start, "varint longer than 64 bits");
      v |= uint64_t(b & 0x7F) << shift;
      if (!(b & 0x80)) return true;
    }
    return fail(start, "varint longer than 10 bytes");
  }

  // Skips one field's payload after its tag (groups up to their end tag).
  bool skip(const char*& p, const char* end, uint32_t field, int wt, int depth) {
    uint64_t v = 0;
    switch (wt) {
      case 0:
        return varint(p, end, v);
      case 1:
        if (end - p < 8) return fail(p, "truncated fixed64 of field " + std::to_string(field));
        p += 8;
        return true;
      case 5:
        if (end - p < 4) return fail(p, "truncated fixed32 of field " + std::to_string(field));
        p += 4;
        return true;
      case 2: {
        const char* at = p;
        if (!varint(p, end, v)) return false;
        if (v > uint64_t(end - p)) return fail(at, "length " + std::to_string(v) + " of field " + std::to_string(field) + " runs past the end");
        p += v;
        return true;
      }
      case 3: {
        if (depth > 100) return fail(p, "groups nested too deeply");
        for (;;) {
          const char* at = p;
          if (p >= end) return fail(at, "unterminated group " + std::to_string(field));
          uint64_t tag;
          if (!varint(p, end, tag)) return false;
          const int iwt = int(tag & 7);
          const uint32_t ifield = uint32_t(tag >> 3);
          if (iwt == 4) {
            if (ifield != field) return fail(at, "mismatched end of group " + std::to_string(field));
            return true;
          }
          if (ifield == 0) return fail(at, "invalid tag (field number 0)");
          if (!skip(p, end, ifield, iwt, depth + 1)) return false;
        }
      }
      case 4:
        return fail(p, "unexpected end-group tag for field " + std::to_string(field));
      default:
        return fail(p, "invalid wire type " + std::to_string(wt) + " for field " + std::to_string(field));
    }
  }

  // The label of a map entry for paths: its key, as the JSON form would print it.
  std::string map_key(std::string_view entry, const pb::Descriptor* d) {
    const pb::FieldDescriptor* kf = d->FindFieldByNumber(1);
    std::string label;
    const char* p = entry.data();
    const char* end = p + entry.size();
    std::string ignored_err;
    std::vector<std::string> ignored_notes;
    Walker w{ignored_notes, ignored_err, begin};
    while (p < end) {
      uint64_t tag;
      if (!w.varint(p, end, tag)) break;
      const uint32_t field = uint32_t(tag >> 3);
      const int wt = int(tag & 7);
      const char* val = p;
      if (!w.skip(p, end, field, wt, 0)) break;
      if (field != 1 || !kf) continue;
      if (wt == 2) {
        const char* q = val;
        uint64_t len;
        w.varint(q, end, len);
        std::string_view s(q, size_t(len));
        std::string fixed;
        label = fix_utf8(s, fixed) ? fixed : std::string(s);
      } else if (wt == 0) {
        const char* q = val;
        uint64_t v;
        w.varint(q, end, v);
        switch (kf->type()) {
          case pb::FieldDescriptor::TYPE_BOOL: label = v ? "true" : "false"; break;
          case pb::FieldDescriptor::TYPE_INT32: label = std::to_string(int32_t(uint32_t(v))); break;
          case pb::FieldDescriptor::TYPE_INT64: label = std::to_string(int64_t(v)); break;
          case pb::FieldDescriptor::TYPE_SINT32:
          case pb::FieldDescriptor::TYPE_SINT64: label = std::to_string(int64_t(v >> 1) ^ -int64_t(v & 1)); break;
          case pb::FieldDescriptor::TYPE_UINT32: label = std::to_string(uint32_t(v)); break;
          default: label = std::to_string(v); break;
        }
      } else if (wt == 5) {
        uint32_t v;
        std::memcpy(&v, val, 4);
        label = kf->type() == pb::FieldDescriptor::TYPE_SFIXED32 ? std::to_string(int32_t(v)) : std::to_string(v);
      } else if (wt == 1) {
        uint64_t v;
        std::memcpy(&v, val, 8);
        label = kf->type() == pb::FieldDescriptor::TYPE_SFIXED64 ? std::to_string(int64_t(v)) : std::to_string(v);
      }
    }
    return label;
  }

  // Copies a message of type d from [p, end) to out, fixing string fields. path names it in notes.
  bool message(const char* p, const char* end, const pb::Descriptor* d, const std::string& path, std::string& out,
               int depth) {
    if (depth > 100) return fail(p, "messages nested too deeply");
    const bool entry = d && d->options().map_entry();
    std::vector<std::pair<int, int>> counts;  // repeated field number -> next index
    auto next_index = [&](int field) {
      for (auto& [f, n] : counts)
        if (f == field) return n++;
      counts.emplace_back(field, 1);
      return 0;
    };
    while (p < end) {
      const char* field_start = p;
      uint64_t tag;
      if (!varint(p, end, tag)) return false;
      const uint32_t field = uint32_t(tag >> 3);
      const int wt = int(tag & 7);
      if (field == 0) return fail(field_start, "invalid tag (field number 0)");
      const pb::FieldDescriptor* fd = (d && wt == 2) ? d->FindFieldByNumber(int(field)) : nullptr;
      const bool str = fd && fd->type() == pb::FieldDescriptor::TYPE_STRING;
      const bool msg = fd && fd->type() == pb::FieldDescriptor::TYPE_MESSAGE;
      if (!str && !msg) {
        const char* body = p;
        if (!skip(p, end, field, wt, depth)) return false;
        out.append(field_start, size_t(body - field_start));
        out.append(body, size_t(p - body));
        continue;
      }
      const char* at = p;
      uint64_t len;
      if (!varint(p, end, len)) return false;
      if (len > uint64_t(end - p))
        return fail(at, "length " + std::to_string(len) + " of field " + std::string(fd->name()) + " runs past the end");
      std::string_view payload(p, size_t(len));
      p += len;

      std::string sub;
      if (entry) {
        sub = field == 1 ? path + " (key)" : path;
      } else {
        sub = path.empty() ? std::string(fd->name()) : path + "." + std::string(fd->name());
        if (fd->is_map())
          sub += "[" + map_key(payload, fd->message_type()) + "]";
        else if (fd->is_repeated())
          sub += "[" + std::to_string(next_index(int(field))) + "]";
      }

      out.append(field_start, size_t(at - field_start));  // the tag
      if (str) {
        std::string fixed;
        if (fix_utf8(payload, fixed)) {
          notes.push_back(sub + ": invalid UTF-8 replaced with U+FFFD");
          put_varint(out, fixed.size());
          out += fixed;
        } else {
          put_varint(out, payload.size());
          out.append(payload);
        }
      } else {
        std::string inner;
        inner.reserve(payload.size());
        if (!message(payload.data(), payload.data() + payload.size(), fd->message_type(), sub, inner, depth + 1))
          return false;
        put_varint(out, inner.size());
        out += inner;
      }
    }
    return true;
  }
};

}  // namespace

bool fix_utf8(std::string_view in, std::string& out) {
  const auto* s = reinterpret_cast<const unsigned char*>(in.data());
  const size_t n = in.size();
  size_t i = 0;
  while (i < n) {
    const size_t k = utf8_seq(s + i, n - i);
    if (!k) break;
    i += k;
  }
  if (i == n) return false;
  out.assign(in.data(), i);
  while (i < n) {
    const size_t k = utf8_seq(s + i, n - i);
    if (k) {
      out.append(in.data() + i, k);
      i += k;
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  return true;
}

WireRepair repair_utf8(std::string_view wire, const pb::Descriptor* type) {
  WireRepair r;
  r.bytes.reserve(wire.size() + 16);
  Walker w{r.notes, r.error, wire.data()};
  r.ok = w.message(wire.data(), wire.data() + wire.size(), type, "", r.bytes, 0);
  if (!r.ok) {
    r.bytes.clear();
    r.notes.clear();
  }
  return r;
}

}  // namespace watt::detail
