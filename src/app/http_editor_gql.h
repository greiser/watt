#pragma once
// GraphQL schema support for the HTTP editor's GraphQL body (Postman / Insomnia / Bruno fetch the
// schema by introspection and use it to complete and check the query). No wx dependency: the
// parse runs on a worker thread, the walks run on the UI thread in microseconds for a typical
// query (one tokenizer pass, no allocation per character), and tests/test_http_editor.cpp
// compiles this file directly.
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace watt::app::http_editor {

// The standard IntrospectionQuery (graphql-js getIntrospectionQuery(), descriptions on), POSTed as
// {"query": ..., "operationName": "IntrospectionQuery"}.
const char* graphql_introspection_query();

struct GqlArg {
  std::string name;
  std::string type;   // as written in SDL: "[ID!]!"
  std::string named;  // the innermost named type: "ID"
  std::string desc;
  std::string default_value;
};

struct GqlField {
  std::string name;
  std::string type;
  std::string named;
  std::string desc;
  std::vector<GqlArg> args;  // sorted by name
  bool deprecated = false;
  const GqlArg* arg(std::string_view n) const;
};

struct GqlType {
  std::string name;
  std::string kind;  // OBJECT INTERFACE UNION ENUM INPUT_OBJECT SCALAR
  std::string desc;
  std::vector<GqlField> fields;        // OBJECT / INTERFACE, sorted by name
  std::vector<GqlArg> input_fields;    // INPUT_OBJECT, sorted by name
  std::vector<std::string> enum_values;
  const GqlField* field(std::string_view n) const;
  bool selectable() const { return kind == "OBJECT" || kind == "INTERFACE" || kind == "UNION"; }
};

struct GqlSchema {
  std::map<std::string, GqlType, std::less<>> types;
  std::string query, mutation, subscription;  // root type names (empty: none)
  const GqlType* type(std::string_view n) const;
  // Types the server defines, without the introspection types (__Schema, __Type, ...).
  size_t user_type_count() const;
};

// Parses an introspection response ({"data": {"__schema": ...}}, or the bare __schema object).
// Null with *err set for an error response ({"errors": [{"message": ...}]}), a body that is not
// JSON, or no __schema in it. About 15 ms per MB (run it on a worker).
std::shared_ptr<const GqlSchema> parse_gql_schema(std::string_view json, std::string* err);

// A problem in the query (unknown field / argument / type, undeclared variable) or in the
// variables JSON (undeclared key, required variable missing).
struct GqlIssue {
  size_t offset = 0;
  size_t length = 1;
  std::string message;
};

// Checks every selection, argument and input object field against the schema (null: only the
// variable references), and every $variable used in an operation against its declarations. At
// most 64 issues, in document order. Fragments are checked against their type condition;
// directives and SDL documents are not checked.
std::vector<GqlIssue> gql_validate(std::string_view doc, const GqlSchema* schema);

struct GqlVar {
  std::string name;  // without "$"
  std::string type;  // "ID!"
  bool has_default = false;
};
struct GqlOperation {
  std::string kind;  // query / mutation / subscription
  std::string name;  // empty for an anonymous one
  std::vector<GqlVar> vars;
};
std::vector<GqlOperation> gql_operations(std::string_view doc);

// The variables JSON against the variables the operation declares (operation_name, else the
// first operation): keys it does not declare, and declared non-null variables without a default
// that are missing. Lenient like the editor ({{var}} values and keys allowed; a text that is not
// an object is left to the JSON check). Offsets are into `vars`.
std::vector<GqlIssue> gql_check_variables(std::string_view doc, std::string_view vars,
                                          std::string_view operation_name = {});

// Completion at `caret`: what may be typed there and the word being typed (from..caret).
struct GqlItem {
  std::string name;
  std::string detail;  // "String!", "(id: ID!): User", "$id: ID!"
  std::string desc;
};
struct GqlCompletion {
  enum class Kind { None, Field, Argument, InputField, Variable, Type, EnumValue, Keyword };
  Kind kind = Kind::None;
  size_t from = 0;  // start of the word being typed
  std::string on;   // where: "User", "Query.user", "UserFilter"
  std::vector<GqlItem> items;  // those that start with the typed word, sorted
};
// Selection set fields of the type at the caret (aliases, inline fragments and nesting walked),
// arguments inside "(", input object fields inside "{" of an argument value, enum values and
// true / false for a value, "$" variables declared by the operation, type names after "on" and
// in variable definitions. No schema: only $variables and keywords.
GqlCompletion gql_complete(std::string_view doc, size_t caret, const GqlSchema* schema);

// "https://api.example.com/graphql" + environment: the cache key of a fetched schema.
std::string gql_cache_key(std::string_view url, std::string_view env);
// File name (hex of a 64-bit hash, ".json") for the copy kept in <ws>/.watt/graphql/.
std::string gql_cache_file_name(std::string_view key);

// "4 min ago", "2 h ago", "just now"
std::string gql_age(double seconds);

}  // namespace watt::app::http_editor
