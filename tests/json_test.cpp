// The JSON parser and writer: what it accepts, what it must refuse, its limits,
// and that no input (random, or a valid document changed anywhere) breaks it.

#include <cstdio>
#include <random>

#include "check.hpp"
#include "npk/json/json.hpp"

using namespace npk::json;

namespace {

bool ok(std::string_view t) { return parse(t).has_value(); }

void accepts() {
  CHECK(ok("null") && ok("true") && ok("false") && ok(" 0 ") && ok("-0") && ok("1.5e-3") && ok("1E+2"));
  CHECK(ok("\"\"") && ok("[]") && ok("{}") && ok("[1, 2 ,3]") && ok("{\"a\" : [ {\"b\":null} ] }"));
  CHECK(ok("\"\\u00e9\\ud83d\\ude00 \\n\\/\"") && ok("\"héllo 😀\""));
  CHECK(ok("9223372036854775807") && ok("-9223372036854775808"));
  auto v = parse("{\"name\":\"x\",\"n\":3,\"f\":2.5,\"ok\":true,\"list\":[1,2],\"nil\":null}");
  CHECK(v && v->is_object());
  CHECK(*v->find("name")->as_string() == "x");
  CHECK(*v->find("n")->as_int() == 3 && *v->find("f")->as_double() == 2.5);
  CHECK(*v->find("ok")->as_bool() && v->find("list")->as_array()->size() == 2 && v->find("nil")->is_null());
  CHECK(v->find("missing") == nullptr && !v->find("n")->as_string() && !v->find("name")->as_int());
  CHECK(*parse("1e2")->as_int() == 100);           // a double that is a whole number
  CHECK(!parse("1.5")->as_int());
  auto big = parse("9223372036854775808");          // past int64: a double
  CHECK(big && big->is_number() && !big->is_int());
  CHECK(*parse("\"\\u00e9\"")->as_string() == "\xc3\xa9");
  CHECK(*parse("\"\\ud83d\\ude00\"")->as_string() == "\xf0\x9f\x98\x80");
}

void refuses() {
  for (const char* t : {"", " ", "nul", "tru", "[1,]", "[,1]", "{\"a\":1,}", "{,}", "{\"a\"}", "{a:1}", "{'a':1}",
                        "[1 2]", "01", "1.", ".5", "+1", "-", "1e", "1e+", "0x10", "NaN", "Infinity", "[1]x", "[1] [2]",
                        "\"abc", "\"a\nb\"", "\"\\x41\"", "\"\\u12\"", "\"\\u12g4\"", "\"\\ud800\"", "\"\\udc00\"",
                        "\"\\ud800\\u0041\"", "\"\\u0000\"", "// c\n1", "/* c */ 1", "{\"a\":1,\"a\":2}", "1e999",
                        "\"\xc3\"", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"", "\"\xff\"", "\"\x80\""}) {
    CHECK(!ok(t));
  }
  std::string err;
  CHECK(!parse("[1,", {}, &err) && !err.empty());
}

void limits() {
  Limits l;
  l.max_depth = 4;
  CHECK(parse("[[[1]]]", l));
  CHECK(!parse("[[[[1]]]]", l));
  std::string deep(100000, '[');
  CHECK(!parse(deep));  // no stack overflow
  l = {};
  l.max_bytes = 10;
  CHECK(parse("[1,2,3,4]", l) && !parse("[1,2,3,4,5,6]", l));
  l = {};
  l.max_values = 5;
  CHECK(parse("[1,2,3,4]", l) && !parse("[1,2,3,4,5]", l));
  l = {};
  l.max_string = 8;
  CHECK(parse("\"12345678\"", l) && !parse("\"123456789\"", l) && !parse("{\"123456789\":1}", l));
  CHECK(!parse("12345678901234567890123456789012345678901234567890"));  // number too long
}

void writes() {
  Value o;
  o.set("s", "a\"b\\c\n\t\x01 é");
  o.set("i", 7);
  o.set("d", 0.5);
  o.set("b", false);
  o.set("n", nullptr);
  Value a;
  a.push_back(1);
  a.push_back("x");
  o.set("a", a);
  const std::string t = dump(o);
  CHECK(t == "{\"s\":\"a\\\"b\\\\c\\n\\t\\u0001 \xc3\xa9\",\"i\":7,\"d\":0.5,\"b\":false,\"n\":null,\"a\":[1,\"x\"]}");
  auto back = parse(t);
  CHECK(back && dump(*back) == t);
  CHECK(*back->find("s")->as_string() == "a\"b\\c\n\t\x01 \xc3\xa9");
  o.set("i", 8);  // replaces
  CHECK(*o.find("i")->as_int() == 8 && o.as_object()->size() == 6);
  CHECK(dump(Value(1.0 / 0.0)) == "null");
  CHECK(dump(Value(0.1)) == "0.10000000000000001");
  CHECK(*parse(dump(Value(0.1)))->as_double() == 0.1);
}

void garbage() {
  std::mt19937_64 rng(7);
  const std::string seed =
      "{\"a\":[1,-2.5e3,true,null,\"x\\u00e9\\ud83d\\ude00\"],\"b\":{\"c\":\"d\",\"e\":[[],{}]},\"f\":\"\xc3\xa9\"}";
  CHECK(ok(seed));
  const char alphabet[] = "{}[]\",:\\-+.eE0123456789tfnu \n\xc3\xa9\x80\xff";
  for (int i = 0; i < 200000; ++i) {
    std::string t;
    if (i % 3 == 0) {
      t.resize(rng() % 64);
      for (auto& c : t) c = alphabet[rng() % (sizeof alphabet - 1)];
    } else {
      t = seed;
      for (int k = 0, n = 1 + static_cast<int>(rng() % 4); k < n; ++k) {
        const std::size_t at = rng() % t.size();
        switch (rng() % 3) {
          case 0: t[at] = alphabet[rng() % (sizeof alphabet - 1)]; break;
          case 1: t.erase(at, 1 + rng() % 3); break;
          default: t.insert(at, 1, alphabet[rng() % (sizeof alphabet - 1)]);
        }
        if (t.empty()) t = "x";
      }
    }
    auto v = parse(t);
    if (v) {  // whatever parses writes and parses again to the same document
      auto again = parse(dump(*v));
      CHECK(again && dump(*again) == dump(*v));
    }
  }
}

}  // namespace

int main() {
  accepts();
  refuses();
  limits();
  writes();
  garbage();
  std::puts("json_test: ok");
  return 0;
}
