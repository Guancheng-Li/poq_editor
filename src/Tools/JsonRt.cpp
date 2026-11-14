// JsonRt —— json::parse / write 往返离线自检(不进主 exe)。

#include <cstdio>
#include <cstring>
#include <string>
#include "Core/Json.h"

namespace {

struct Case {
  const char* name;
  const char* input;
};

bool run_case(const Case& c) {
  try {
    json::Value v = json::parse(c.input);
    std::string out = json::write(v, false);
    json::Value v2 = json::parse(out);
    std::string out2 = json::write(v2, false);
    if (out != out2) {
      std::printf("[FAIL] %s: round-trip mismatch\n", c.name);
      return false;
    }
    std::printf("[OK] %s\n", c.name);
    return true;
  } catch (const std::exception& ex) {
    std::printf("[FAIL] %s: %s\n", c.name, ex.what());
    return false;
  }
}

}  // namespace

int main() {
  static const Case k_cases[] = {
      {"null", "null"},
      {"bool", "true"},
      {"int", "42"},
      {"float", "3.14"},
      {"string", "\"hello\""},
      {"array", "[1,2,3]"},
      {"object", "{\"a\":1,\"b\":[false]}"},
      {"unicode", "\"\\u4e2d\""},
  };
  int fail = 0;
  for (const Case& c : k_cases) {
    if (!run_case(c)) ++fail;
  }
  if (fail) {
    std::printf("JsonRt: %d failed\n", fail);
    return 1;
  }
  std::printf("JsonRt: all %zu cases passed\n", sizeof(k_cases) / sizeof(k_cases[0]));
  return 0;
}
