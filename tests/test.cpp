#include "RefactorTool.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include <gtest/gtest.h>

// --- Инфраструктура для тестирования ---

// Фабрика действий для перехвата результатов рефакторинга в память.
class TestActionFactory : public clang::tooling::FrontendActionFactory {
private:
  std::string *Result;

  class ActionWithCallback : public CodeRefactorAction {
  private:
    std::string *Result;

  public:
    ActionWithCallback(std::string *Result) : Result(Result) {}

    void EndSourceFileAction() override {
      llvm::raw_string_ostream ostream(*Result);
      getRewriter()
          .getEditBuffer(getRewriter().getSourceMgr().getMainFileID())
          .write(ostream);
    }
  };

public:
  TestActionFactory(std::string &Result) : Result(&Result) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<ActionWithCallback>(Result);
  }
};

// Вспомогательная функция для запуска рефакторинга на строке кода.
std::string runRefactoring(const std::string &code) {
  std::vector<std::string> args = {"-std=c++17"};
  clang::tooling::FixedCompilationDatabase compilations(".", args);
  std::vector<std::string> source_paths = {"test.cpp"};
  clang::tooling::ClangTool tool(compilations, source_paths);
  tool.mapVirtualFile("test.cpp", code);

  std::string result_string;
  TestActionFactory factory(result_string);

  if (tool.run(&factory) != 0) {
    return "Error: ClangTool failed to run.";
  }

  return result_string;
}

// Добавляет `virtual` к деструктору базового класса.
// Проверяет, что `virtual` добавляется к деструктору класса, имеющего
// наследников, и не затрагивает классы без наследников или с уже виртуальным
// деструктором.
TEST(RefactorTest, AddsVirtualToDestructor) {
  const char *inputCode = R"(
class Base {
public:
    ~Base() {}
};
class Derived : public Base {};

class Standalone {
public:
    ~Standalone() {}
};

class BaseAlreadyVirtual {
public:
    virtual ~BaseAlreadyVirtual() {}
};
class DerivedFromVirtual : public BaseAlreadyVirtual {};
)";

  const char *expectedCode = R"(
class Base {
public:
    virtual ~Base() {}
};
class Derived : public Base {};

class Standalone {
public:
    ~Standalone() {}
};

class BaseAlreadyVirtual {
public:
    virtual ~BaseAlreadyVirtual() {}
};
class DerivedFromVirtual : public BaseAlreadyVirtual {};
)";

  std::string result = runRefactoring(inputCode);
  EXPECT_EQ(result, expectedCode);
}

// Добавляет `override` к переопределенным методам.
// Проверяет, что `override` добавляется к виртуальным методам без этого
// спецификатора и игнорирует деструкторы и методы, где `override` уже есть.
TEST(RefactorTest, AddsOverrideToMethods) {
  const char *inputCode = R"(
class Base {
public:
    virtual void func() {}
    virtual ~Base() {}
};

class Derived : public Base {
public:
    void func() {}
    ~Derived() {}
};

class AlreadyWithOverride : public Base {
public:
    void func() override {}
};
)";

  const char *expectedCode = R"(
class Base {
public:
    virtual void func() {}
    virtual ~Base() {}
};

class Derived : public Base {
public:
    void func() override {}
    ~Derived() {}
};

class AlreadyWithOverride : public Base {
public:
    void func() override {}
};
)";

  std::string result = runRefactoring(inputCode);
  EXPECT_EQ(result, expectedCode);
}

// Добавляет ссылку в range-based for циклах для предотвращения копирования.
// Проверяет, что `&` добавляется к переменным-объектам, передаваемым по
// значению, и игнорирует фундаментальные типы и переменные, уже являющиеся
// ссылками.
TEST(RefactorTest, AddsReferenceToRangeForLoopVariable) {
  const char *inputCode = R"(#include <vector>
struct CustomType { int x; };

void process() {
    std::vector<CustomType> vec;
    for (const auto x : vec) {
        (void)x;
    }

    std::vector<int> ints;
    for (const int i : ints) {
        (void)i;
    }

    std::vector<CustomType> vec2;
    for (const auto& x : vec2) {
        (void)x;
    }
}
)";

  const char *expectedCode = R"(#include <vector>
struct CustomType { int x; };

void process() {
    std::vector<CustomType> vec;
    for (const auto & x : vec) {
        (void)x;
    }

    std::vector<int> ints;
    for (const int i : ints) {
        (void)i;
    }

    std::vector<CustomType> vec2;
    for (const auto& x : vec2) {
        (void)x;
    }
}
)";

  std::string result = runRefactoring(inputCode);
  EXPECT_EQ(result, expectedCode);
}

// Корректно обрабатывает цепочки наследования.
// Проверяет, что `virtual` добавляется к деструкторам всех классов в иерархии,
// которые выступают как базовые (A и B).
TEST(RefactorTest, AddsVirtualToDestructorInInheritanceChain) {
  const char *inputCode = R"(
class A {
public:
    ~A() {}
};
class B : public A {
public:
    ~B() {}
};
class C : public B {};
)";

  const char *expectedCode = R"(
class A {
public:
    virtual ~A() {}
};
class B : public A {
public:
    virtual ~B() {}
};
class C : public B {};
)";

  std::string result = runRefactoring(inputCode);
  EXPECT_EQ(result, expectedCode);
}

// Добавляет ссылку в range-based for для переменных с явным типом.
// Проверяет, что рефакторинг работает не только с `auto`, но и с явно
// указанными пользовательскими типами.
TEST(RefactorTest, AddsReferenceToExplicitlyTypedRangeForLoopVariable) {
  const char *inputCode = R"(#include <vector>
struct MyStruct { int a; };
void foo() {
  std::vector<MyStruct> items;
  for (const MyStruct item : items) {
    (void)item;
  }
}
)";

  const char *expectedCode = R"(#include <vector>
struct MyStruct { int a; };
void foo() {
  std::vector<MyStruct> items;
  for (const MyStruct & item : items) {
    (void)item;
  }
}
)";

  std::string result = runRefactoring(inputCode);
  EXPECT_EQ(result, expectedCode);
}

// Инструмент не изменяет корректно написанный код.
// Проверяет отсутствие ложных срабатываний на коде, который уже
// соответствует всем правилам рефакторинга.
TEST(RefactorTest, DoesNotChangeCorrectlyWrittenCode) {
  const char *inputCode = R"(#include <vector>
struct S { int val; };

class Base {
public:
    virtual void check() = 0;
    virtual ~Base() = default;
};

class Derived : public Base {
public:
    void check() override {}
    ~Derived() {}
};

void process() {
    std::vector<S> items;
    for (const S& item : items) {
        (void)item;
    }
    std::vector<int> nums;
    for (int num : nums) {
        (void)num;
    }
}
)";

  std::string result = runRefactoring(inputCode);
  EXPECT_EQ(result, inputCode);
}