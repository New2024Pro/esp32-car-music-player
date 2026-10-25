#pragma once
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <functional>

struct MiniTest { const char* name; std::function<void()> fn; };
inline std::vector<MiniTest>& tests() { static std::vector<MiniTest> t; return t; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { tests().push_back({n, f}); } };

#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()
#define CHECK(c) do { if (!(c)) { std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures()++; } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { std::printf("    FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); failures()++; } } while (0)

inline int runAll() {
  for (auto& t : tests()) {
    int before = failures();
    t.fn();
    std::printf("[%s] %s\n", failures() == before ? " ok " : "FAIL", t.name);
  }
  std::printf("%zu tests, %d failed checks\n", tests().size(), failures());
  return failures() ? 1 : 0;
}

