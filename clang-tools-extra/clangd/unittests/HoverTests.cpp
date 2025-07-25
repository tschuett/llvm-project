//===-- HoverTests.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AST.h"
#include "Annotations.h"
#include "Config.h"
#include "Hover.h"
#include "TestFS.h"
#include "TestIndex.h"
#include "TestTU.h"
#include "index/MemIndex.h"
#include "clang/AST/Attr.h"
#include "clang/Format/Format.h"
#include "clang/Index/IndexSymbol.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include "gtest/gtest.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace clangd {
namespace {

using PassMode = HoverInfo::PassType::PassMode;

std::string guard(llvm::StringRef Code) {
  return "#pragma once\n" + Code.str();
}

TEST(Hover, Structured) {
  struct {
    const char *const Code;
    const std::function<void(HoverInfo &)> ExpectedBuilder;
  } Cases[] = {
      // Global scope.
      {R"cpp(
          // Best foo ever.
          void [[fo^o]]() {}
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "foo";
         HI.Kind = index::SymbolKind::Function;
         HI.Documentation = "Best foo ever.";
         HI.Definition = "void foo()";
         HI.ReturnType = "void";
         HI.Type = "void ()";
         HI.Parameters.emplace();
       }},
      // Inside namespace
      {R"cpp(
          namespace ns1 { namespace ns2 {
            /// Best foo ever.
            void [[fo^o]]() {}
          }}
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "ns1::ns2::";
         HI.Name = "foo";
         HI.Kind = index::SymbolKind::Function;
         HI.Documentation = "Best foo ever.";
         HI.Definition = "void foo()";
         HI.ReturnType = "void";
         HI.Type = "void ()";
         HI.Parameters.emplace();
       }},
      // Field
      {R"cpp(
          namespace ns1 { namespace ns2 {
            class Foo {
              char [[b^ar]];
              double y[2];
            };
          }}
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "ns1::ns2::";
         HI.LocalScope = "Foo::";
         HI.Name = "bar";
         HI.Kind = index::SymbolKind::Field;
         HI.Definition = "char bar";
         HI.Type = "char";
         HI.Offset = 0;
         HI.Size = 8;
         HI.Padding = 56;
         HI.Align = 8;
         HI.AccessSpecifier = "private";
       }},
      // Union field
      {R"cpp(
            union Foo {
              char [[b^ar]];
              double y[2];
            };
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "Foo::";
         HI.Name = "bar";
         HI.Kind = index::SymbolKind::Field;
         HI.Definition = "char bar";
         HI.Type = "char";
         HI.Size = 8;
         HI.Padding = 120;
         HI.Align = 8;
         HI.AccessSpecifier = "public";
       }},
      // Bitfield
      {R"cpp(
            struct Foo {
              int [[^x]] : 1;
              int y : 1;
            };
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "Foo::";
         HI.Name = "x";
         HI.Kind = index::SymbolKind::Field;
         HI.Definition = "int x : 1";
         HI.Type = "int";
         HI.Offset = 0;
         HI.Size = 1;
         HI.Align = 32;
         HI.AccessSpecifier = "public";
       }},
      // Local to class method.
      {R"cpp(
          namespace ns1 { namespace ns2 {
            struct Foo {
              void foo() {
                int [[b^ar]];
              }
            };
          }}
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "ns1::ns2::";
         HI.LocalScope = "Foo::foo::";
         HI.Name = "bar";
         HI.Kind = index::SymbolKind::Variable;
         HI.Definition = "int bar";
         HI.Type = "int";
       }},
      // Predefined variable
      {R"cpp(
          void foo() {
            [[__f^unc__]];
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "__func__";
         HI.Kind = index::SymbolKind::Variable;
         HI.Documentation =
             "Name of the current function (predefined variable)";
         HI.Value = "\"foo\"";
         HI.Type = "const char[4]";
       }},
      // Predefined variable (dependent)
      {R"cpp(
          template<int> void foo() {
            [[__f^unc__]];
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "__func__";
         HI.Kind = index::SymbolKind::Variable;
         HI.Documentation =
             "Name of the current function (predefined variable)";
         HI.Type = "const char[]";
       }},
      // Anon namespace and local scope.
      {R"cpp(
          namespace ns1 { namespace {
            struct {
              char [[b^ar]];
            } T;
          }}
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "ns1::";
         HI.LocalScope = "(anonymous struct)::";
         HI.Name = "bar";
         HI.Kind = index::SymbolKind::Field;
         HI.Definition = "char bar";
         HI.Type = "char";
         HI.Offset = 0;
         HI.Size = 8;
         HI.Align = 8;
         HI.AccessSpecifier = "public";
       }},
      // Struct definition shows size.
      {R"cpp(
          struct [[^X]]{};
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "X";
         HI.Kind = index::SymbolKind::Struct;
         HI.Definition = "struct X {}";
         HI.Size = 8;
         HI.Align = 8;
       }},
      // Variable with template type
      {R"cpp(
          template <typename T, class... Ts> class Foo { public: Foo(int); };
          Foo<int, char, bool> [[fo^o]] = Foo<int, char, bool>(5);
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "foo";
         HI.Kind = index::SymbolKind::Variable;
         HI.Definition = "Foo<int, char, bool> foo = Foo<int, char, bool>(5)";
         HI.Type = "Foo<int, char, bool>";
       }},
      // Implicit template instantiation
      {R"cpp(
          template <typename T> class vector{};
          [[vec^tor]]<int> foo;
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "vector<int>";
         HI.Kind = index::SymbolKind::Class;
         HI.Definition = "template <> class vector<int> {}";
       }},
      // Class template
      {R"cpp(
          template <template<typename, bool...> class C,
                    typename = char,
                    int = 0,
                    bool Q = false,
                    class... Ts> class Foo final {};
          template <template<typename, bool...> class T>
          [[F^oo]]<T> foo;
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "Foo";
         HI.Kind = index::SymbolKind::Class;
         HI.Definition =
             R"cpp(template <template <typename, bool...> class C, typename = char, int = 0,
          bool Q = false, class... Ts>
class Foo final {})cpp";
         HI.TemplateParameters = {
             {{"template <typename, bool...> class"},
              std::string("C"),
              std::nullopt},
             {{"typename"}, std::nullopt, std::string("char")},
             {{"int"}, std::nullopt, std::string("0")},
             {{"bool"}, std::string("Q"), std::string("false")},
             {{"class..."}, std::string("Ts"), std::nullopt},
         };
       }},
      // Function template
      {R"cpp(
          template <template<typename, bool...> class C,
                    typename = char,
                    int = 0,
                    bool Q = false,
                    class... Ts> void foo();
          template<typename, bool...> class Foo;

          void bar() {
            [[fo^o]]<Foo>();
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "foo";
         HI.Kind = index::SymbolKind::Function;
         HI.Definition = "template <> void foo<Foo, char, 0, false, <>>()";
         HI.ReturnType = "void";
         HI.Type = "void ()";
         HI.Parameters.emplace();
       }},
      // Function decl
      {R"cpp(
          template<typename, bool...> class Foo {};
          Foo<bool, true, false> foo(int, bool T = false);

          void bar() {
            [[fo^o]](3);
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "foo";
         HI.Kind = index::SymbolKind::Function;
         HI.Definition = "Foo<bool, true, false> foo(int, bool T = false)";
         HI.ReturnType = "Foo<bool, true, false>";
         HI.Type = "Foo<bool, true, false> (int, bool)";
         HI.Parameters = {
             {{"int"}, std::nullopt, std::nullopt},
             {{"bool"}, std::string("T"), std::string("false")},
         };
       }},
      // Pointers to lambdas
      {R"cpp(
        void foo() {
          auto lamb = [](int T, bool B) -> bool { return T && B; };
          auto *b = &lamb;
          auto *[[^c]] = &b;
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "foo::";
         HI.Name = "c";
         HI.Kind = index::SymbolKind::Variable;
         HI.Definition = "auto *c = &b";
         HI.Type = "(lambda) **";
         HI.ReturnType = "bool";
         HI.Parameters = {
             {{"int"}, std::string("T"), std::nullopt},
             {{"bool"}, std::string("B"), std::nullopt},
         };
         return HI;
       }},
      // Lambda parameter with decltype reference
      {R"cpp(
        auto lamb = [](int T, bool B) -> bool { return T && B; };
        void foo(decltype(lamb)& bar) {
          [[ba^r]](0, false);
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "foo::";
         HI.Name = "bar";
         HI.Kind = index::SymbolKind::Parameter;
         HI.Definition = "decltype(lamb) &bar";
         HI.Type = {"decltype(lamb) &", "(lambda) &"};
         HI.ReturnType = "bool";
         HI.Parameters = {
             {{"int"}, std::string("T"), std::nullopt},
             {{"bool"}, std::string("B"), std::nullopt},
         };
         return HI;
       }},
      // Lambda parameter with decltype
      {R"cpp(
        auto lamb = [](int T, bool B) -> bool { return T && B; };
        void foo(decltype(lamb) bar) {
          [[ba^r]](0, false);
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "foo::";
         HI.Name = "bar";
         HI.Kind = index::SymbolKind::Parameter;
         HI.Definition = "decltype(lamb) bar";
         HI.Type = "class (lambda)";
         HI.ReturnType = "bool";
         HI.Parameters = {
             {{"int"}, std::string("T"), std::nullopt},
             {{"bool"}, std::string("B"), std::nullopt},
         };
         HI.Value = "false";
         return HI;
       }},
      // Lambda variable
      {R"cpp(
        void foo() {
          int bar = 5;
          auto lamb = [&bar](int T, bool B) -> bool { return T && B && bar; };
          bool res = [[lam^b]](bar, false);
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "foo::";
         HI.Name = "lamb";
         HI.Kind = index::SymbolKind::Variable;
         HI.Definition = "auto lamb = [&bar](int T, bool B) -> bool {}";
         HI.Type = "class (lambda)";
         HI.ReturnType = "bool";
         HI.Parameters = {
             {{"int"}, std::string("T"), std::nullopt},
             {{"bool"}, std::string("B"), std::nullopt},
         };
         return HI;
       }},
      // Local variable in lambda
      {R"cpp(
        void foo() {
          auto lamb = []{int [[te^st]];};
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "foo::(anonymous class)::operator()::";
         HI.Name = "test";
         HI.Kind = index::SymbolKind::Variable;
         HI.Definition = "int test";
         HI.Type = "int";
       }},
      // Partially-specialized class template. (formerly type-parameter-0-0)
      {R"cpp(
        template <typename T> class X;
        template <typename T> class [[^X]]<T*> {};
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "X<T *>";
         HI.NamespaceScope = "";
         HI.Kind = index::SymbolKind::Class;
         HI.Definition = "template <typename T> class X<T *> {}";
       }},
      // Constructor of partially-specialized class template
      {R"cpp(
          template<typename, typename=void> struct X;
          template<typename T> struct X<T*>{ [[^X]](); };
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "X";
         HI.LocalScope = "X<T *>::"; // FIXME: X<T *, void>::
         HI.Kind = index::SymbolKind::Constructor;
         HI.Definition = "X()";
         HI.Parameters.emplace();
         HI.AccessSpecifier = "public";
       }},
      {"class X { [[^~]]X(); };", // FIXME: Should be [[~X]]()
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "~X";
         HI.LocalScope = "X::";
         HI.Kind = index::SymbolKind::Destructor;
         HI.Definition = "~X()";
         HI.Parameters.emplace();
         HI.AccessSpecifier = "private";
       }},
      {"class X { [[op^erator]] int(); };",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "operator int";
         HI.LocalScope = "X::";
         HI.Kind = index::SymbolKind::ConversionFunction;
         HI.Definition = "operator int()";
         HI.Parameters.emplace();
         HI.AccessSpecifier = "private";
       }},
      {"class X { operator [[^X]](); };",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "X";
         HI.Kind = index::SymbolKind::Class;
         HI.Definition = "class X {}";
       }},

      // auto on structured bindings
      {R"cpp(
        void foo() {
          struct S { int x; float y; };
          [[au^to]] [x, y] = S();
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "auto";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "S";
       }},
      // undeduced auto
      {R"cpp(
        template<typename T>
        void foo() {
          [[au^to]] x = T{};
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "auto";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "/* not deduced */";
       }},
      // constrained auto
      {R"cpp(
        template <class T> concept F = true;
        F [[au^to]] x = 1;
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "auto";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "int";
       }},
      {R"cpp(
        template <class T> concept F = true;
        [[^F]] auto x = 1;
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "F";
         HI.Kind = index::SymbolKind::Concept;
         HI.Definition = "template <class T>\nconcept F = true";
       }},
      // auto on lambda
      {R"cpp(
        void foo() {
          [[au^to]] lamb = []{};
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "auto";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "class(lambda)";
       }},
      // auto on template instantiation
      {R"cpp(
        template<typename T> class Foo{};
        void foo() {
          [[au^to]] x = Foo<int>();
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "auto";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "Foo<int>";
       }},
      // auto on specialized template
      {R"cpp(
        template<typename T> class Foo{};
        template<> class Foo<int>{};
        void foo() {
          [[au^to]] x = Foo<int>();
        }
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "auto";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "Foo<int>";
       }},
      // constrained template parameter
      {R"cpp(
        template<class T> concept Fooable = true;
        template<[[Foo^able]] T>
        void bar(T t) {}
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "Fooable";
         HI.Kind = index::SymbolKind::Concept;
         HI.Definition = "template <class T>\nconcept Fooable = true";
       }},
      {R"cpp(
        template<class T> concept Fooable = true;
        template<Fooable [[T^T]]>
        void bar(TT t) {}
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "TT";
         HI.Type = "class";
         HI.AccessSpecifier = "public";
         HI.NamespaceScope = "";
         HI.LocalScope = "bar::";
         HI.Kind = index::SymbolKind::TemplateTypeParm;
         HI.Definition = "Fooable TT";
       }},
      {R"cpp(
        template<class T> concept Fooable = true;
        void bar([[Foo^able]] auto t) {}
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "Fooable";
         HI.Kind = index::SymbolKind::Concept;
         HI.Definition = "template <class T>\nconcept Fooable = true";
       }},
      // concept reference
      {R"cpp(
        template<class T> concept Fooable = true;
        auto X = [[Fooa^ble]]<int>;
        )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.Name = "Fooable";
         HI.Kind = index::SymbolKind::Concept;
         HI.Definition = "template <class T>\nconcept Fooable = true";
         HI.Value = "true";
       }},

      // empty macro
      {R"cpp(
        #define MACRO
        [[MAC^RO]]
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "MACRO";
         HI.Kind = index::SymbolKind::Macro;
         HI.Definition = "#define MACRO";
       }},

      // object-like macro
      {R"cpp(
        #define MACRO 41
        int x = [[MAC^RO]];
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "MACRO";
         HI.Kind = index::SymbolKind::Macro;
         HI.Value = "41 (0x29)";
         HI.Type = "int";
         HI.Definition = "#define MACRO 41\n\n"
                         "// Expands to\n"
                         "41";
       }},

      // function-like macro
      {R"cpp(
        // Best MACRO ever.
        #define MACRO(x,y,z) void foo(x, y, z)
        [[MAC^RO]](int, double d, bool z = false);
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "MACRO";
         HI.Kind = index::SymbolKind::Macro;
         HI.Definition = "#define MACRO(x, y, z) void foo(x, y, z)\n\n"
                         "// Expands to\n"
                         "void foo(int, double d, bool z = false)";
       }},

      // nested macro
      {R"cpp(
        #define STRINGIFY_AUX(s) #s
        #define STRINGIFY(s) STRINGIFY_AUX(s)
        #define DECL_STR(NAME, VALUE) const char *v_##NAME = STRINGIFY(VALUE)
        #define FOO 41

        [[DECL^_STR]](foo, FOO);
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "DECL_STR";
         HI.Kind = index::SymbolKind::Macro;
         HI.Type = HoverInfo::PrintedType("const char *");
         HI.Definition = "#define DECL_STR(NAME, VALUE) const char *v_##NAME = "
                         "STRINGIFY(VALUE)\n\n"
                         "// Expands to\n"
                         "const char *v_foo = \"41\"";
       }},

      // constexprs
      {R"cpp(
        constexpr int add(int a, int b) { return a + b; }
        int [[b^ar]] = add(1, 2);
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "bar";
         HI.Definition = "int bar = add(1, 2)";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = "int";
         HI.NamespaceScope = "";
         HI.Value = "3";
       }},
      {R"cpp(
        int [[b^ar]] = sizeof(char);
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "bar";
         HI.Definition = "int bar = sizeof(char)";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = "int";
         HI.NamespaceScope = "";
         HI.Value = "1";
       }},
      {R"cpp(
        template<int a, int b> struct Add {
          static constexpr int result = a + b;
        };
        int [[ba^r]] = Add<1, 2>::result;
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "bar";
         HI.Definition = "int bar = Add<1, 2>::result";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = "int";
         HI.NamespaceScope = "";
         HI.Value = "3";
       }},
      {R"cpp(
        enum Color { RED = -123, GREEN = 5, };
        Color x = [[GR^EEN]];
       )cpp",
       [](HoverInfo &HI) {
         HI.Name = "GREEN";
         HI.NamespaceScope = "";
         HI.LocalScope = "Color::";
         HI.Definition = "GREEN = 5";
         HI.Kind = index::SymbolKind::EnumConstant;
         HI.Type = "enum Color";
         HI.Value = "5"; // Numeric on the enumerator name, no hex as small.
       }},
      {R"cpp(
        enum Color { RED = -123, GREEN = 5, };
        Color x = RED;
        Color y = [[^x]];
       )cpp",
       [](HoverInfo &HI) {
         HI.Name = "x";
         HI.NamespaceScope = "";
         HI.Definition = "Color x = RED";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = "Color";
         HI.Value = "RED (0xffffff85)"; // Symbolic on an expression.
       }},
      {R"cpp(
        template<int a, int b> struct Add {
          static constexpr int result = a + b;
        };
        int bar = Add<1, 2>::[[resu^lt]];
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "result";
         HI.Definition = "static constexpr int result = a + b";
         HI.Kind = index::SymbolKind::StaticProperty;
         HI.Type = "const int";
         HI.NamespaceScope = "";
         HI.LocalScope = "Add<1, 2>::";
         HI.Value = "3";
         HI.AccessSpecifier = "public";
       }},
      {R"cpp(
        using my_int = int;
        constexpr my_int answer() { return 40 + 2; }
        int x = [[ans^wer]]();
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "answer";
         HI.Definition = "constexpr my_int answer()";
         HI.Kind = index::SymbolKind::Function;
         HI.Type = {"my_int ()", "int ()"};
         HI.ReturnType = {"my_int", "int"};
         HI.Parameters.emplace();
         HI.NamespaceScope = "";
         HI.Value = "42 (0x2a)";
       }},
      {R"cpp(
        const char *[[ba^r]] = "1234";
        )cpp",
       [](HoverInfo &HI) {
         HI.Name = "bar";
         HI.Definition = "const char *bar = \"1234\"";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = "const char *";
         HI.NamespaceScope = "";
         HI.Value = "&\"1234\"[0]";
       }},
      {R"cpp(// Should not crash
        template <typename T>
        struct Tmpl {
          Tmpl(int name);
        };

        template <typename A>
        void boom(int name) {
          new Tmpl<A>([[na^me]]);
        })cpp",
       [](HoverInfo &HI) {
         HI.Name = "name";
         HI.Definition = "int name";
         HI.Kind = index::SymbolKind::Parameter;
         HI.Type = "int";
         HI.NamespaceScope = "";
         HI.LocalScope = "boom::";
       }},
      {
          R"cpp(// Should not print inline or anon namespaces.
          namespace ns {
            inline namespace in_ns {
              namespace a {
                namespace {
                  namespace b {
                    inline namespace in_ns2 {
                      class Foo {};
                    } // in_ns2
                  } // b
                } // anon
              } // a
            } // in_ns
          } // ns
          void foo() {
            ns::a::b::[[F^oo]] x;
            (void)x;
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "Foo";
            HI.Kind = index::SymbolKind::Class;
            HI.NamespaceScope = "ns::a::b::";
            HI.Definition = "class Foo {}";
          }},
      {
          R"cpp(
          template <typename T> class Foo {};
          class X;
          void foo() {
            [[^auto]] x = Foo<X>();
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Foo<X>";
          }},
      {// Falls back to primary template, when the type is not instantiated.
       R"cpp(
          // comment from primary
          template <typename T> class Foo {};
          // comment from specialization
          template <typename T> class Foo<T*> {};
          void foo() {
            [[Fo^o]]<int*> *x = nullptr;
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "Foo<int *>";
         HI.Kind = index::SymbolKind::Class;
         HI.NamespaceScope = "";
         HI.Definition = "template <> class Foo<int *>";
         // FIXME: Maybe force instantiation to make use of real template
         // pattern.
         HI.Documentation = "comment from primary";
       }},
      {// Template Type Parameter
       R"cpp(
          template <typename [[^T]] = int> void foo();
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "T";
         HI.Kind = index::SymbolKind::TemplateTypeParm;
         HI.NamespaceScope = "";
         HI.Definition = "typename T = int";
         HI.LocalScope = "foo::";
         HI.Type = "typename";
         HI.AccessSpecifier = "public";
       }},
      {// TemplateTemplate Type Parameter
       R"cpp(
          template <template<typename> class [[^T]]> void foo();
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "T";
         HI.Kind = index::SymbolKind::TemplateTemplateParm;
         HI.NamespaceScope = "";
         HI.Definition = "template <typename> class T";
         HI.LocalScope = "foo::";
         HI.Type = "template <typename> class";
         HI.AccessSpecifier = "public";
       }},
      {// NonType Template Parameter
       R"cpp(
          template <int [[^T]] = 5> void foo();
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "T";
         HI.Kind = index::SymbolKind::NonTypeTemplateParm;
         HI.NamespaceScope = "";
         HI.Definition = "int T = 5";
         HI.LocalScope = "foo::";
         HI.Type = "int";
         HI.AccessSpecifier = "public";
       }},

      {// Getter
       R"cpp(
          struct X { int Y; float [[^y]]() { return Y; } };
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "y";
         HI.Kind = index::SymbolKind::InstanceMethod;
         HI.NamespaceScope = "";
         HI.Definition = "float y()";
         HI.LocalScope = "X::";
         HI.Documentation = "Trivial accessor for `Y`.";
         HI.Type = "float ()";
         HI.ReturnType = "float";
         HI.Parameters.emplace();
         HI.AccessSpecifier = "public";
       }},
      {// Setter
       R"cpp(
          struct X { int Y; void [[^setY]](float v) { Y = v; } };
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "setY";
         HI.Kind = index::SymbolKind::InstanceMethod;
         HI.NamespaceScope = "";
         HI.Definition = "void setY(float v)";
         HI.LocalScope = "X::";
         HI.Documentation = "Trivial setter for `Y`.";
         HI.Type = "void (float)";
         HI.ReturnType = "void";
         HI.Parameters.emplace();
         HI.Parameters->emplace_back();
         HI.Parameters->back().Type = "float";
         HI.Parameters->back().Name = "v";
         HI.AccessSpecifier = "public";
       }},
      {// Setter (builder)
       R"cpp(
          struct X { int Y; X& [[^setY]](float v) { Y = v; return *this; } };
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "setY";
         HI.Kind = index::SymbolKind::InstanceMethod;
         HI.NamespaceScope = "";
         HI.Definition = "X &setY(float v)";
         HI.LocalScope = "X::";
         HI.Documentation = "Trivial setter for `Y`.";
         HI.Type = "X &(float)";
         HI.ReturnType = "X &";
         HI.Parameters.emplace();
         HI.Parameters->emplace_back();
         HI.Parameters->back().Type = "float";
         HI.Parameters->back().Name = "v";
         HI.AccessSpecifier = "public";
       }},
      {// Setter (move)
       R"cpp(
          namespace std { template<typename T> T&& move(T&& t); }
          struct X { int Y; void [[^setY]](float v) { Y = std::move(v); } };
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "setY";
         HI.Kind = index::SymbolKind::InstanceMethod;
         HI.NamespaceScope = "";
         HI.Definition = "void setY(float v)";
         HI.LocalScope = "X::";
         HI.Documentation = "Trivial setter for `Y`.";
         HI.Type = "void (float)";
         HI.ReturnType = "void";
         HI.Parameters.emplace();
         HI.Parameters->emplace_back();
         HI.Parameters->back().Type = "float";
         HI.Parameters->back().Name = "v";
         HI.AccessSpecifier = "public";
       }},
      {// Field type initializer.
       R"cpp(
          struct X { int x = 2; };
          X ^[[x]];
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "x";
         HI.Kind = index::SymbolKind::Variable;
         HI.NamespaceScope = "";
         HI.Definition = "X x";
         HI.Type = "X";
       }},
      {// Don't crash on null types.
       R"cpp(auto [^[[x]]] = 1; /*error-ok*/)cpp",
       [](HoverInfo &HI) {
         HI.Name = "x";
         HI.Kind = index::SymbolKind::Variable;
         HI.NamespaceScope = "";
         HI.Definition = "";
         HI.Type = "NULL TYPE";
         // Bindings are in theory public members of an anonymous struct.
         HI.AccessSpecifier = "public";
       }},
      {// Don't crash on invalid decl with invalid init expr.
       R"cpp(
          Unknown [[^abc]] = invalid;
          // error-ok
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "abc";
         HI.Kind = index::SymbolKind::Variable;
         HI.NamespaceScope = "";
         HI.Definition = "int abc";
         HI.Type = "int";
         HI.AccessSpecifier = "public";
       }},
      {// Extra info for function call.
       R"cpp(
          void fun(int arg_a, int &arg_b) {};
          void code() {
            int a = 1, b = 2;
            fun(a, [[^b]]);
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "b";
         HI.Kind = index::SymbolKind::Variable;
         HI.NamespaceScope = "";
         HI.Definition = "int b = 2";
         HI.LocalScope = "code::";
         HI.Value = "2";
         HI.Type = "int";
         HI.CalleeArgInfo.emplace();
         HI.CalleeArgInfo->Name = "arg_b";
         HI.CalleeArgInfo->Type = "int &";
         HI.CallPassType = HoverInfo::PassType{PassMode::Ref, false};
       }},
      {// make_unique-like function call
       R"cpp(
          struct Foo {
            explicit Foo(int arg_a) {}
          };
          template<class T, class... Args>
          T make(Args&&... args)
          {
              return T(args...);
          }

          void code() {
            int a = 1;
            auto foo = make<Foo>([[^a]]);
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "a";
         HI.Kind = index::SymbolKind::Variable;
         HI.NamespaceScope = "";
         HI.Definition = "int a = 1";
         HI.LocalScope = "code::";
         HI.Value = "1";
         HI.Type = "int";
         HI.CalleeArgInfo.emplace();
         HI.CalleeArgInfo->Name = "arg_a";
         HI.CalleeArgInfo->Type = "int";
         HI.CallPassType = HoverInfo::PassType{PassMode::Value, false};
       }},
      {
          R"cpp(
          void foobar(const float &arg);
          int main() {
            int a = 0;
            foobar([[^a]]);
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "a";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Definition = "int a = 0";
            HI.LocalScope = "main::";
            HI.Value = "0";
            HI.Type = "int";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "arg";
            HI.CalleeArgInfo->Type = "const float &";
            HI.CallPassType = HoverInfo::PassType{PassMode::Value, true};
          }},
      {
          R"cpp(
          struct Foo {
            explicit Foo(const float& arg) {}
          };
          int main() {
            int a = 0;
            Foo foo([[^a]]);
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "a";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Definition = "int a = 0";
            HI.LocalScope = "main::";
            HI.Value = "0";
            HI.Type = "int";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "arg";
            HI.CalleeArgInfo->Type = "const float &";
            HI.CallPassType = HoverInfo::PassType{PassMode::Value, true};
          }},
      {// Literal passed to function call
       R"cpp(
          void fun(int arg_a, const int &arg_b) {};
          void code() {
            int a = 1;
            fun(a, [[^2]]);
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "literal";
         HI.Kind = index::SymbolKind::Unknown;
         HI.CalleeArgInfo.emplace();
         HI.CalleeArgInfo->Name = "arg_b";
         HI.CalleeArgInfo->Type = "const int &";
         HI.CallPassType = HoverInfo::PassType{PassMode::ConstRef, false};
       }},
      {// Expression passed to function call
       R"cpp(
          void fun(int arg_a, const int &arg_b) {};
          void code() {
            int a = 1;
            fun(a, 1 [[^+]] 2);
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "expression";
         HI.Kind = index::SymbolKind::Unknown;
         HI.Type = "int";
         HI.Value = "3";
         HI.CalleeArgInfo.emplace();
         HI.CalleeArgInfo->Name = "arg_b";
         HI.CalleeArgInfo->Type = "const int &";
         HI.CallPassType = HoverInfo::PassType{PassMode::ConstRef, false};
       }},
      {
          R"cpp(
        int add(int lhs, int rhs);
        int main() {
          add(1 [[^+]] 2, 3);
        }
        )cpp",
          [](HoverInfo &HI) {
            HI.Name = "expression";
            HI.Kind = index::SymbolKind::Unknown;
            HI.Type = "int";
            HI.Value = "3";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "lhs";
            HI.CalleeArgInfo->Type = "int";
            HI.CallPassType = HoverInfo::PassType{PassMode::Value, false};
          }},
      {
          R"cpp(
        void foobar(const float &arg);
        int main() {
          foobar([[^0]]);
        }
        )cpp",
          [](HoverInfo &HI) {
            HI.Name = "literal";
            HI.Kind = index::SymbolKind::Unknown;
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "arg";
            HI.CalleeArgInfo->Type = "const float &";
            HI.CallPassType = HoverInfo::PassType{PassMode::Value, true};
          }},
      {// Extra info for method call.
       R"cpp(
          class C {
           public:
            void fun(int arg_a = 3, int arg_b = 4) {}
          };
          void code() {
            int a = 1, b = 2;
            C c;
            c.fun([[^a]], b);
          }
          )cpp",
       [](HoverInfo &HI) {
         HI.Name = "a";
         HI.Kind = index::SymbolKind::Variable;
         HI.NamespaceScope = "";
         HI.Definition = "int a = 1";
         HI.LocalScope = "code::";
         HI.Value = "1";
         HI.Type = "int";
         HI.CalleeArgInfo.emplace();
         HI.CalleeArgInfo->Name = "arg_a";
         HI.CalleeArgInfo->Type = "int";
         HI.CalleeArgInfo->Default = "3";
         HI.CallPassType = HoverInfo::PassType{PassMode::Value, false};
       }},
      {
          R"cpp(
          struct Foo {
            Foo(const int &);
          };
          void foo(Foo);
          void bar() {
            const int x = 0;
            foo([[^x]]);
          }
       )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Definition = "const int x = 0";
            HI.LocalScope = "bar::";
            HI.Value = "0";
            HI.Type = "const int";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Type = "Foo";
            HI.CallPassType = HoverInfo::PassType{PassMode::ConstRef, true};
          }},
      {// Dont crash on invalid decl
       R"cpp(
        // error-ok
        struct Foo {
          Bar [[x^x]];
        };)cpp",
       [](HoverInfo &HI) {
         HI.Name = "xx";
         HI.Kind = index::SymbolKind::Field;
         HI.NamespaceScope = "";
         HI.Definition = "int xx";
         HI.LocalScope = "Foo::";
         HI.Type = "int";
         HI.AccessSpecifier = "public";
       }},
      {R"cpp(
        // error-ok
        struct Foo {
          Bar xx;
          int [[y^y]];
        };)cpp",
       [](HoverInfo &HI) {
         HI.Name = "yy";
         HI.Kind = index::SymbolKind::Field;
         HI.NamespaceScope = "";
         HI.Definition = "int yy";
         HI.LocalScope = "Foo::";
         HI.Type = "int";
         HI.AccessSpecifier = "public";
       }},
      {// No crash on InitListExpr.
       R"cpp(
          struct Foo {
            int a[10];
          };
          constexpr Foo k2 = {
            ^[[{]]1} // FIXME: why the hover range is 1 character?
          };
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "expression";
         HI.Kind = index::SymbolKind::Unknown;
         HI.Type = "int[10]";
         HI.Value = "{1}";
       }},
      {// Var template decl
       R"cpp(
          using m_int = int;

          template <int Size> m_int ^[[arr]][Size];
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "arr";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = {"m_int[Size]", "int[Size]"};
         HI.NamespaceScope = "";
         HI.Definition = "template <int Size> m_int arr[Size]";
         HI.TemplateParameters = {{{"int"}, {"Size"}, std::nullopt}};
       }},
      {// Var template decl specialization
       R"cpp(
          using m_int = int;

          template <int Size> m_int arr[Size];

          template <> m_int ^[[arr]]<4>[4];
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "arr<4>";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = {"m_int[4]", "int[4]"};
         HI.NamespaceScope = "";
         HI.Definition = "m_int arr[4]";
       }},
      {// Canonical type
       R"cpp(
          template<typename T>
          struct TestHover {
            using Type = T;
          };

          void code() {
            TestHover<int>::Type ^[[a]];
          }
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "a";
         HI.NamespaceScope = "";
         HI.LocalScope = "code::";
         HI.Definition = "TestHover<int>::Type a";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = {"TestHover<int>::Type", "int"};
       }},
      {// Canonical template type
       R"cpp(
          template<typename T>
          void ^[[foo]](T arg) {}
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "foo";
         HI.Kind = index::SymbolKind::Function;
         HI.NamespaceScope = "";
         HI.Definition = "template <typename T> void foo(T arg)";
         HI.Type = "void (T)";
         HI.ReturnType = "void";
         HI.Parameters = {{{"T"}, std::string("arg"), std::nullopt}};
         HI.TemplateParameters = {
             {{"typename"}, std::string("T"), std::nullopt}};
       }},
      {// TypeAlias Template
       R"cpp(
          template<typename T>
          using ^[[alias]] = T;
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "alias";
         HI.NamespaceScope = "";
         HI.LocalScope = "";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "template <typename T> using alias = T";
         HI.Type = "T";
         HI.TemplateParameters = {
             {{"typename"}, std::string("T"), std::nullopt}};
       }},
      {// TypeAlias Template
       R"cpp(
          template<typename T>
          using A = T;

          template<typename T>
          using ^[[AA]] = A<T>;
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "AA";
         HI.NamespaceScope = "";
         HI.LocalScope = "";
         HI.Kind = index::SymbolKind::TypeAlias;
         HI.Definition = "template <typename T> using AA = A<T>";
         HI.Type = {"A<T>", "T"};
         HI.TemplateParameters = {
             {{"typename"}, std::string("T"), std::nullopt}};
       }},
      {// Constant array
       R"cpp(
          using m_int = int;

          m_int ^[[arr]][10];
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "arr";
         HI.NamespaceScope = "";
         HI.LocalScope = "";
         HI.Kind = index::SymbolKind::Variable;
         HI.Definition = "m_int arr[10]";
         HI.Type = {"m_int[10]", "int[10]"};
       }},
      {// Incomplete array
       R"cpp(
          using m_int = int;

          extern m_int ^[[arr]][];
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "arr";
         HI.NamespaceScope = "";
         HI.LocalScope = "";
         HI.Kind = index::SymbolKind::Variable;
         HI.Definition = "extern m_int arr[]";
         HI.Type = {"m_int[]", "int[]"};
       }},
      {// Dependent size array
       R"cpp(
          using m_int = int;

          template<int Size>
          struct Test {
            m_int ^[[arr]][Size];
          };
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "arr";
         HI.NamespaceScope = "";
         HI.LocalScope = "Test<Size>::";
         HI.AccessSpecifier = "public";
         HI.Kind = index::SymbolKind::Field;
         HI.Definition = "m_int arr[Size]";
         HI.Type = {"m_int[Size]", "int[Size]"};
       }},
      {// Bitfield offset, size and padding
       R"cpp(
            struct Foo {
              char x;
              char [[^y]] : 1;
              int z;
            };
          )cpp",
       [](HoverInfo &HI) {
         HI.NamespaceScope = "";
         HI.LocalScope = "Foo::";
         HI.Name = "y";
         HI.Kind = index::SymbolKind::Field;
         HI.Definition = "char y : 1";
         HI.Type = "char";
         HI.Offset = 8;
         HI.Size = 1;
         HI.Padding = 23;
         HI.Align = 8;
         HI.AccessSpecifier = "public";
       }}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Code);

    Annotations T(Case.Code);
    TestTU TU = TestTU::withCode(T.code());
    TU.ExtraArgs.push_back("-std=c++20");
    // Types might be different depending on the target triplet, we chose a
    // fixed one to make sure tests passes on different platform.
    TU.ExtraArgs.push_back("--target=x86_64-pc-linux-gnu");
    auto AST = TU.build();
    Config Cfg;
    Cfg.Hover.ShowAKA = true;
    WithContextValue WithCfg(Config::Key, std::move(Cfg));

    auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
    ASSERT_TRUE(H);
    HoverInfo Expected;
    Expected.SymRange = T.range();
    Case.ExpectedBuilder(Expected);

    EXPECT_EQ(H->NamespaceScope, Expected.NamespaceScope);
    EXPECT_EQ(H->LocalScope, Expected.LocalScope);
    EXPECT_EQ(H->Name, Expected.Name);
    EXPECT_EQ(H->Kind, Expected.Kind);
    EXPECT_EQ(H->Documentation, Expected.Documentation);
    EXPECT_EQ(H->Definition, Expected.Definition);
    EXPECT_EQ(H->Type, Expected.Type);
    EXPECT_EQ(H->ReturnType, Expected.ReturnType);
    EXPECT_EQ(H->Parameters, Expected.Parameters);
    EXPECT_EQ(H->TemplateParameters, Expected.TemplateParameters);
    EXPECT_EQ(H->SymRange, Expected.SymRange);
    EXPECT_EQ(H->Value, Expected.Value);
    EXPECT_EQ(H->Size, Expected.Size);
    EXPECT_EQ(H->Offset, Expected.Offset);
    EXPECT_EQ(H->Align, Expected.Align);
    EXPECT_EQ(H->AccessSpecifier, Expected.AccessSpecifier);
    EXPECT_EQ(H->CalleeArgInfo, Expected.CalleeArgInfo);
    EXPECT_EQ(H->CallPassType, Expected.CallPassType);
  }
}

TEST(Hover, DefinitionLanuage) {
  struct {
    const char *const Code;
    const std::string ClangLanguageFlag;
    const char *const ExpectedDefinitionLanguage;
  } Cases[] = {{R"cpp(
          void [[some^Global]]() {}
          )cpp",
                "", "cpp"},
               {R"cpp(
          void [[some^Global]]() {}
          )cpp",
                "-xobjective-c++", "objective-cpp"},
               {R"cpp(
          void [[some^Global]]() {}
          )cpp",
                "-xobjective-c", "objective-c"}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Code);

    Annotations T(Case.Code);
    TestTU TU = TestTU::withCode(T.code());
    if (!Case.ClangLanguageFlag.empty())
      TU.ExtraArgs.push_back(Case.ClangLanguageFlag);
    auto AST = TU.build();

    auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
    ASSERT_TRUE(H);

    EXPECT_STREQ(H->DefinitionLanguage, Case.ExpectedDefinitionLanguage);
  }
}

TEST(Hover, CallPassType) {
  const llvm::StringRef CodePrefix = R"cpp(
class Base {};
class Derived : public Base {};
class CustomClass {
 public:
  CustomClass() {}
  CustomClass(const Base &x) {}
  CustomClass(int &x) {}
  CustomClass(float x) {}
  CustomClass(int x, int y) {}
};

void int_by_ref(int &x) {}
void int_by_const_ref(const int &x) {}
void int_by_value(int x) {}
void base_by_ref(Base &x) {}
void base_by_const_ref(const Base &x) {}
void base_by_value(Base x) {}
void float_by_value(float x) {}
void custom_by_value(CustomClass x) {}

void fun() {
  int int_x;
  int &int_ref = int_x;
  const int &int_const_ref = int_x;
  Base base;
  const Base &base_const_ref = base;
  Derived derived;
  float float_x;
)cpp";
  const llvm::StringRef CodeSuffix = "}";

  struct {
    const char *const Code;
    HoverInfo::PassType::PassMode PassBy;
    bool Converted;
  } Tests[] = {
      // Integer tests
      {"int_by_value([[^int_x]]);", PassMode::Value, false},
      {"int_by_value([[^123]]);", PassMode::Value, false},
      {"int_by_ref([[^int_x]]);", PassMode::Ref, false},
      {"int_by_const_ref([[^int_x]]);", PassMode::ConstRef, false},
      {"int_by_const_ref([[^123]]);", PassMode::ConstRef, false},
      {"int_by_value([[^int_ref]]);", PassMode::Value, false},
      {"int_by_const_ref([[^int_ref]]);", PassMode::ConstRef, false},
      {"int_by_const_ref([[^int_ref]]);", PassMode::ConstRef, false},
      {"int_by_const_ref([[^int_const_ref]]);", PassMode::ConstRef, false},
      // Custom class tests
      {"base_by_ref([[^base]]);", PassMode::Ref, false},
      {"base_by_const_ref([[^base]]);", PassMode::ConstRef, false},
      {"base_by_const_ref([[^base_const_ref]]);", PassMode::ConstRef, false},
      {"base_by_value([[^base]]);", PassMode::Value, false},
      {"base_by_value([[^base_const_ref]]);", PassMode::Value, false},
      {"base_by_ref([[^derived]]);", PassMode::Ref, false},
      {"base_by_const_ref([[^derived]]);", PassMode::ConstRef, false},
      {"base_by_value([[^derived]]);", PassMode::Value, false},
      // Custom class constructor tests
      {"CustomClass c1([[^base]]);", PassMode::ConstRef, false},
      {"auto c2 = new CustomClass([[^base]]);", PassMode::ConstRef, false},
      {"CustomClass c3([[^int_x]]);", PassMode::Ref, false},
      {"CustomClass c3(int_x, [[^int_x]]);", PassMode::Value, false},
      // Converted tests
      {"float_by_value([[^int_x]]);", PassMode::Value, true},
      {"float_by_value([[^int_ref]]);", PassMode::Value, true},
      {"float_by_value([[^int_const_ref]]);", PassMode::Value, true},
      {"float_by_value([[^123.0f]]);", PassMode::Value, false},
      {"float_by_value([[^123]]);", PassMode::Value, true},
      {"custom_by_value([[^int_x]]);", PassMode::Ref, true},
      {"custom_by_value([[^float_x]]);", PassMode::Value, true},
      {"custom_by_value([[^base]]);", PassMode::ConstRef, true},
  };
  for (const auto &Test : Tests) {
    SCOPED_TRACE(Test.Code);

    const auto Code = (CodePrefix + Test.Code + CodeSuffix).str();
    Annotations T(Code);
    TestTU TU = TestTU::withCode(T.code());
    TU.ExtraArgs.push_back("-std=c++17");
    auto AST = TU.build();
    auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
    ASSERT_TRUE(H);
    EXPECT_EQ(H->CallPassType->PassBy, Test.PassBy);
    EXPECT_EQ(H->CallPassType->Converted, Test.Converted);
  }
}

TEST(Hover, NoHover) {
  llvm::StringRef Tests[] = {
      "^int main() {}",
      "void foo() {^}",
      // FIXME: "decltype(auto)" should be a single hover
      "decltype(au^to) x = 0;",
      // FIXME: not supported yet
      R"cpp(// Lambda auto parameter
            auto lamb = [](a^uto){};
          )cpp",
      R"cpp(// non-named decls don't get hover. Don't crash!
            ^static_assert(1, "");
          )cpp",
      R"cpp(// non-evaluatable expr
          template <typename T> void foo() {
            (void)[[size^of]](T);
          })cpp",
      R"cpp(// should not crash on invalid semantic form of init-list-expr.
            /*error-ok*/
            struct Foo {
              int xyz = 0;
            };
            class Bar {};
            constexpr Foo s = ^{
              .xyz = Bar(),
            };
          )cpp",
      // literals
      "auto x = t^rue;",
      "auto x = ^(int){42};",
      "auto x = ^42.;",
      "auto x = ^42.0i;",
      "auto x = ^42;",
      "auto x = ^nullptr;",
  };

  for (const auto &Test : Tests) {
    SCOPED_TRACE(Test);

    Annotations T(Test);
    TestTU TU = TestTU::withCode(T.code());
    TU.ExtraArgs.push_back("-std=c++17");
    auto AST = TU.build();
    auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
    ASSERT_FALSE(H);
  }
}

TEST(Hover, All) {
  struct {
    const char *const Code;
    const std::function<void(HoverInfo &)> ExpectedBuilder;
  } Cases[] = {
      {"auto x = [['^A']]; // character literal",
       [](HoverInfo &HI) {
         HI.Name = "expression";
         HI.Type = "char";
         HI.Value = "65 (0x41)";
       }},
      {"auto s = ^[[\"Hello, world!\"]]; // string literal",
       [](HoverInfo &HI) {
         HI.Name = "string-literal";
         HI.Size = 112;
         HI.Type = "const char[14]";
       }},
      {
          R"cpp(// Local variable
            int main() {
              int bonjour;
              ^[[bonjour]] = 2;
              int test1 = bonjour;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "bonjour";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.LocalScope = "main::";
            HI.Type = "int";
            HI.Definition = "int bonjour";
          }},
      {
          R"cpp(// Local variable in method
            struct s {
              void method() {
                int bonjour;
                ^[[bonjour]] = 2;
              }
            };
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "bonjour";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.LocalScope = "s::method::";
            HI.Type = "int";
            HI.Definition = "int bonjour";
          }},
      {
          R"cpp(// Struct
            namespace ns1 {
              struct MyClass {};
            } // namespace ns1
            int main() {
              ns1::[[My^Class]]* Params;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MyClass";
            HI.Kind = index::SymbolKind::Struct;
            HI.NamespaceScope = "ns1::";
            HI.Definition = "struct MyClass {}";
          }},
      {
          R"cpp(// Class
            namespace ns1 {
              class MyClass {};
            } // namespace ns1
            int main() {
              ns1::[[My^Class]]* Params;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MyClass";
            HI.Kind = index::SymbolKind::Class;
            HI.NamespaceScope = "ns1::";
            HI.Definition = "class MyClass {}";
          }},
      {
          R"cpp(// Union
            namespace ns1 {
              union MyUnion { int x; int y; };
            } // namespace ns1
            int main() {
              ns1::[[My^Union]] Params;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MyUnion";
            HI.Kind = index::SymbolKind::Union;
            HI.NamespaceScope = "ns1::";
            HI.Definition = "union MyUnion {}";
          }},
      {
          R"cpp(// Function definition via pointer
            void foo(int) {}
            int main() {
              auto *X = &^[[foo]];
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Function;
            HI.NamespaceScope = "";
            HI.Type = "void (int)";
            HI.Definition = "void foo(int)";
            HI.Documentation = "Function definition via pointer";
            HI.ReturnType = "void";
            HI.Parameters = {
                {{"int"}, std::nullopt, std::nullopt},
            };
          }},
      {
          R"cpp(// Function declaration via call
            int foo(int);
            int main() {
              return ^[[foo]](42);
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Function;
            HI.NamespaceScope = "";
            HI.Type = "int (int)";
            HI.Definition = "int foo(int)";
            HI.Documentation = "Function declaration via call";
            HI.ReturnType = "int";
            HI.Parameters = {
                {{"int"}, std::nullopt, std::nullopt},
            };
          }},
      {
          R"cpp(// Field
            struct Foo { int x; };
            int main() {
              Foo bar;
              (void)bar.^[[x]];
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::Field;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int";
            HI.Definition = "int x";
          }},
      {
          R"cpp(// Field with initialization
            struct Foo { int x = 5; };
            int main() {
              Foo bar;
              (void)bar.^[[x]];
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::Field;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int";
            HI.Definition = "int x = 5";
          }},
      {
          R"cpp(// Static field
            struct Foo { static int x; };
            int main() {
              (void)Foo::^[[x]];
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::StaticProperty;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int";
            HI.Definition = "static int x";
          }},
      {
          R"cpp(// Field, member initializer
            struct Foo {
              int x;
              Foo() : ^[[x]](0) {}
            };
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::Field;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int";
            HI.Definition = "int x";
          }},
      {
          R"cpp(// Field, GNU old-style field designator
            struct Foo { int x; };
            int main() {
              Foo bar = { ^[[x]] : 1 };
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::Field;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int";
            HI.Definition = "int x";
            // FIXME: Initializer for x is a DesignatedInitListExpr, hence it is
            // of struct type and omitted.
          }},
      {
          R"cpp(// Field, field designator
            struct Foo { int x; int y; };
            int main() {
              Foo bar = { .^[[x]] = 2, .y = 2 };
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::Field;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int";
            HI.Definition = "int x";
          }},
      {
          R"cpp(// Method call
            struct Foo { int x(); };
            int main() {
              Foo bar;
              bar.^[[x]]();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::InstanceMethod;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int ()";
            HI.Definition = "int x()";
            HI.ReturnType = "int";
            HI.Parameters = std::vector<HoverInfo::Param>{};
          }},
      {
          R"cpp(// Static method call
            struct Foo { static int x(); };
            int main() {
              Foo::^[[x]]();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::StaticMethod;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::";
            HI.Type = "int ()";
            HI.Definition = "static int x()";
            HI.ReturnType = "int";
            HI.Parameters = std::vector<HoverInfo::Param>{};
          }},
      {
          R"cpp(// Typedef
            typedef int Foo;
            int main() {
              ^[[Foo]] bar;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "Foo";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.NamespaceScope = "";
            HI.Definition = "typedef int Foo";
            HI.Type = "int";
            HI.Documentation = "Typedef";
          }},
      {
          R"cpp(// Typedef with embedded definition
            typedef struct Bar {} Foo;
            int main() {
              ^[[Foo]] bar;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "Foo";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.NamespaceScope = "";
            HI.Definition = "typedef struct Bar Foo";
            HI.Type = "struct Bar";
            HI.Documentation = "Typedef with embedded definition";
          }},
      {
          R"cpp(// Namespace
            namespace ns {
            struct Foo { static void bar(); };
            } // namespace ns
            int main() { ^[[ns]]::Foo::bar(); }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "ns";
            HI.Kind = index::SymbolKind::Namespace;
            HI.NamespaceScope = "";
            HI.Definition = "namespace ns {}";
          }},
      {
          R"cpp(// Anonymous namespace
            namespace ns {
              namespace {
                int foo;
              } // anonymous namespace
            } // namespace ns
            int main() { ns::[[f^oo]]++; }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "ns::";
            HI.Type = "int";
            HI.Definition = "int foo";
          }},
      {
          R"cpp(// Function definition via using declaration
            namespace ns {
              void foo();
            }
            int main() {
              using ns::foo;
              ^[[foo]]();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Function;
            HI.NamespaceScope = "ns::";
            HI.Type = "void ()";
            HI.Definition = "void foo()";
            HI.Documentation = "";
            HI.ReturnType = "void";
            HI.Parameters = std::vector<HoverInfo::Param>{};
          }},
      {
          R"cpp( // using declaration and two possible function declarations
            namespace ns { void foo(int); void foo(char); }
            using ns::foo;
            template <typename T> void bar() { [[f^oo]](T{}); }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Using;
            HI.NamespaceScope = "";
            HI.Definition = "using ns::foo";
          }},
      {
          R"cpp(// Macro
            #define MACRO 0
            int main() { return ^[[MACRO]]; }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MACRO";
            HI.Value = "0";
            HI.Type = "int";
            HI.Kind = index::SymbolKind::Macro;
            HI.Definition = "#define MACRO 0\n\n"
                            "// Expands to\n"
                            "0";
          }},
      {
          R"cpp(// Macro
            #define MACRO 0
            #define MACRO2 ^[[MACRO]]
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MACRO";
            HI.Kind = index::SymbolKind::Macro;
            HI.Definition = "#define MACRO 0";
            // NOTE MACRO doesn't have expansion since it technically isn't
            // expanded here
          }},
      {
          R"cpp(// Macro
            #define MACRO {\
              return 0;\
            }
            int main() ^[[MACRO]]
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MACRO";
            HI.Kind = index::SymbolKind::Macro;
            HI.Definition =
                R"cpp(#define MACRO                                                                  \
  {                                                                            \
    return 0;                                                                  \
  }

// Expands to
{
  return 0;
})cpp";
          }},
      {
          R"cpp(// Forward class declaration
            class Foo;
            class Foo {};
            [[F^oo]]* foo();
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "Foo";
            HI.Kind = index::SymbolKind::Class;
            HI.NamespaceScope = "";
            HI.Definition = "class Foo {}";
            HI.Documentation = "Forward class declaration";
          }},
      {
          R"cpp(// Function declaration
            void foo();
            void g() { [[f^oo]](); }
            void foo() {}
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Function;
            HI.NamespaceScope = "";
            HI.Type = "void ()";
            HI.Definition = "void foo()";
            HI.Documentation = "Function declaration";
            HI.ReturnType = "void";
            HI.Parameters = std::vector<HoverInfo::Param>{};
          }},
      {
          R"cpp(// Enum declaration
            enum Hello {
              ONE, TWO, THREE,
            };
            void foo() {
              [[Hel^lo]] hello = ONE;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "Hello";
            HI.Kind = index::SymbolKind::Enum;
            HI.NamespaceScope = "";
            HI.Definition = "enum Hello {}";
            HI.Documentation = "Enum declaration";
          }},
      {
          R"cpp(// Enumerator
            enum Hello {
              ONE, TWO, THREE,
            };
            void foo() {
              Hello hello = [[O^NE]];
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "ONE";
            HI.Kind = index::SymbolKind::EnumConstant;
            HI.NamespaceScope = "";
            HI.LocalScope = "Hello::";
            HI.Type = "enum Hello";
            HI.Definition = "ONE";
            HI.Value = "0";
          }},
      {
          R"cpp(// C++20's using enum
            enum class Hello {
              ONE, TWO, THREE,
            };
            void foo() {
              using enum Hello;
              Hello hello = [[O^NE]];
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "ONE";
            HI.Kind = index::SymbolKind::EnumConstant;
            HI.NamespaceScope = "";
            HI.LocalScope = "Hello::";
            HI.Type = "enum Hello";
            HI.Definition = "ONE";
            HI.Value = "0";
          }},
      {
          R"cpp(// Enumerator in anonymous enum
            enum {
              ONE, TWO, THREE,
            };
            void foo() {
              int hello = [[O^NE]];
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "ONE";
            HI.Kind = index::SymbolKind::EnumConstant;
            HI.NamespaceScope = "";
            // FIXME: This should be `(anon enum)::`
            HI.LocalScope = "";
            HI.Type = "enum (unnamed)";
            HI.Definition = "ONE";
            HI.Value = "0";
          }},
      {
          R"cpp(// Global variable
            static int hey = 10;
            void foo() {
              [[he^y]]++;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "hey";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Type = "int";
            HI.Definition = "static int hey = 10";
            HI.Documentation = "Global variable";
            // FIXME: Value shouldn't be set in this case
            HI.Value = "10 (0xa)";
          }},
      {
          R"cpp(// Global variable in namespace
            namespace ns1 {
              static long long hey = -36637162602497;
            }
            void foo() {
              ns1::[[he^y]]++;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "hey";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "ns1::";
            HI.Type = "long long";
            HI.Definition = "static long long hey = -36637162602497";
            HI.Value = "-36637162602497 (0xffffdeadbeefffff)"; // needs 64 bits
          }},
      {
          R"cpp(// Field in anonymous struct
            static struct {
              int hello;
            } s;
            void foo() {
              s.[[he^llo]]++;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "hello";
            HI.Kind = index::SymbolKind::Field;
            HI.NamespaceScope = "";
            HI.LocalScope = "(anonymous struct)::";
            HI.Type = "int";
            HI.Definition = "int hello";
          }},
      {
          R"cpp(// Templated function
            template <typename T>
            T foo() {
              return 17;
            }
            void g() { auto x = [[f^oo]]<int>(); }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Function;
            HI.NamespaceScope = "";
            HI.Type = "int ()";
            HI.Definition = "template <> int foo<int>()";
            HI.Documentation = "Templated function";
            HI.ReturnType = "int";
            HI.Parameters = std::vector<HoverInfo::Param>{};
            // FIXME: We should populate template parameters with arguments in
            // case of instantiations.
          }},
      {
          R"cpp(// Anonymous union
            struct outer {
              union {
                int abc, def;
              } v;
            };
            void g() { struct outer o; o.v.[[d^ef]]++; }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "def";
            HI.Kind = index::SymbolKind::Field;
            HI.NamespaceScope = "";
            HI.LocalScope = "outer::(anonymous union)::";
            HI.Type = "int";
            HI.Definition = "int def";
          }},
      {
          R"cpp(// documentation from index
            int nextSymbolIsAForwardDeclFromIndexWithNoLocalDocs;
            void indexSymbol();
            void g() { [[ind^exSymbol]](); }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "indexSymbol";
            HI.Kind = index::SymbolKind::Function;
            HI.NamespaceScope = "";
            HI.Type = "void ()";
            HI.Definition = "void indexSymbol()";
            HI.ReturnType = "void";
            HI.Parameters = std::vector<HoverInfo::Param>{};
            HI.Documentation = "comment from index";
          }},
      {
          R"cpp(// Simple initialization with auto
            void foo() {
              ^[[auto]] i = 1;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Simple initialization with const auto
            void foo() {
              const ^[[auto]] i = 1;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Simple initialization with const auto&
            void foo() {
              const ^[[auto]]& i = 1;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Simple initialization with auto&
            void foo() {
              int x;
              ^[[auto]]& i = x;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Simple initialization with auto*
            void foo() {
              int a = 1;
              ^[[auto]]* i = &a;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Simple initialization with auto from pointer
            void foo() {
              int a = 1;
              ^[[auto]] i = &a;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int *";
          }},
      {
          R"cpp(// Auto with initializer list.
            namespace std
            {
              template<class _E>
              class initializer_list { const _E *a, *b; };
            }
            void foo() {
              ^[[auto]] i = {1,2};
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "std::initializer_list<int>";
          }},
      {
          R"cpp(// User defined conversion to auto
            struct Bar {
              operator ^[[auto]]() const { return 10; }
            };
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Simple initialization with decltype(auto)
            void foo() {
              ^[[decltype]](auto) i = 1;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Simple initialization with const decltype(auto)
            void foo() {
              const int j = 0;
              ^[[decltype]](auto) i = j;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "const int";
          }},
      {
          R"cpp(// Simple initialization with const& decltype(auto)
            void foo() {
              int k = 0;
              const int& j = k;
              ^[[decltype]](auto) i = j;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "const int &";
          }},
      {
          R"cpp(// Simple initialization with & decltype(auto)
            void foo() {
              int k = 0;
              int& j = k;
              ^[[decltype]](auto) i = j;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int &";
          }},
      {
          R"cpp(// simple trailing return type
            ^[[auto]] main() -> int {
              return 0;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// auto function return with trailing type
            struct Bar {};
            ^[[auto]] test() -> decltype(Bar()) {
              return Bar();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation = "auto function return with trailing type";
          }},
      {
          R"cpp(// trailing return type
            struct Bar {};
            auto test() -> ^[[decltype]](Bar()) {
              return Bar();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation = "trailing return type";
          }},
      {
          R"cpp(// auto in function return
            struct Bar {};
            ^[[auto]] test() {
              return Bar();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation = "auto in function return";
          }},
      {
          R"cpp(// auto& in function return
            struct Bar {};
            ^[[auto]]& test() {
              static Bar x;
              return x;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation = "auto& in function return";
          }},
      {
          R"cpp(// auto* in function return
            struct Bar {};
            ^[[auto]]* test() {
              Bar* bar;
              return bar;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation = "auto* in function return";
          }},
      {
          R"cpp(// const auto& in function return
            struct Bar {};
            const ^[[auto]]& test() {
              static Bar x;
              return x;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation = "const auto& in function return";
          }},
      {
          R"cpp(// decltype(auto) in function return
            struct Bar {};
            ^[[decltype]](auto) test() {
              return Bar();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation = "decltype(auto) in function return";
          }},
      {
          R"cpp(// decltype(auto) reference in function return
            ^[[decltype]](auto) test() {
              static int a;
              return (a);
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int &";
          }},
      {
          R"cpp(// decltype lvalue reference
            void foo() {
              int I = 0;
              ^[[decltype]](I) J = I;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// decltype lvalue reference
            void foo() {
              int I= 0;
              int &K = I;
              ^[[decltype]](K) J = I;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int &";
          }},
      {
          R"cpp(// decltype lvalue reference parenthesis
            void foo() {
              int I = 0;
              ^[[decltype]]((I)) J = I;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int &";
          }},
      {
          R"cpp(// decltype rvalue reference
            void foo() {
              int I = 0;
              ^[[decltype]](static_cast<int&&>(I)) J = static_cast<int&&>(I);
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int &&";
          }},
      {
          R"cpp(// decltype rvalue reference function call
            int && bar();
            void foo() {
              int I = 0;
              ^[[decltype]](bar()) J = bar();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int &&";
          }},
      {
          R"cpp(// decltype of function with trailing return type.
            struct Bar {};
            auto test() -> decltype(Bar()) {
              return Bar();
            }
            void foo() {
              ^[[decltype]](test()) i = test();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "Bar";
            HI.Documentation =
                "decltype of function with trailing return type.";
          }},
      {
          R"cpp(// decltype of var with decltype.
            void foo() {
              int I = 0;
              decltype(I) J = I;
              ^[[decltype]](J) K = J;
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// decltype of dependent type
            template <typename T>
            struct X {
              using Y = ^[[decltype]](T::Z);
            };
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "<dependent type>";
          }},
      {
          R"cpp(// More complicated structured types.
            int bar();
            ^[[auto]] (*foo)() = bar;
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int";
          }},
      {
          R"cpp(// Should not crash when evaluating the initializer.
            struct Test {};
            void test() { Test && [[te^st]] = {}; }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "test";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.LocalScope = "test::";
            HI.Type = "Test &&";
            HI.Definition = "Test &&test = {}";
          }},
      {
          R"cpp(// Shouldn't crash when evaluating the initializer.
            struct Bar {}; // error-ok
            struct Foo { void foo(Bar x = y); }
            void Foo::foo(Bar [[^x]]) {})cpp",
          [](HoverInfo &HI) {
            HI.Name = "x";
            HI.Kind = index::SymbolKind::Parameter;
            HI.NamespaceScope = "";
            HI.LocalScope = "Foo::foo::";
            HI.Type = "Bar";
            HI.Definition = "Bar x = <recovery - expr>()";
          }},
      {
          R"cpp(// auto on alias
          typedef int int_type;
          ^[[auto]] x = int_type();
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "int_type // aka: int";
          }},
      {
          R"cpp(// auto on alias
          struct cls {};
          typedef cls cls_type;
          ^[[auto]] y = cls_type();
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "cls_type // aka: cls";
            HI.Documentation = "auto on alias";
          }},
      {
          R"cpp(// auto on alias
          template <class>
          struct templ {};
          ^[[auto]] z = templ<int>();
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "templ<int>";
            HI.Documentation = "auto on alias";
          }},
      {
          R"cpp(// Undeduced auto declaration
            template<typename T>
            void foo() {
              ^[[auto]] x = T();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "/* not deduced */";
          }},
      {
          R"cpp(// Undeduced auto return type
            template<typename T>
            ^[[auto]] foo() {
              return T();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "/* not deduced */";
          }},
      {
          R"cpp(// Template auto parameter
            template<[[a^uto]] T>
              void func() {
            }
          )cpp",
          [](HoverInfo &HI) {
            // FIXME: not sure this is what we want, but this
            // is what we currently get with getDeducedType
            HI.Name = "auto";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "/* not deduced */";
          }},
      {
          R"cpp(// Undeduced decltype(auto) return type
            template<typename T>
            ^[[decltype]](auto) foo() {
              return T();
            }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "decltype";
            HI.Kind = index::SymbolKind::TypeAlias;
            HI.Definition = "/* not deduced */";
          }},
      {
          R"cpp(// should not crash.
          template <class T> struct cls {
            int method();
          };

          auto test = cls<int>().[[m^ethod]]();
          )cpp",
          [](HoverInfo &HI) {
            HI.Definition = "int method()";
            HI.Kind = index::SymbolKind::InstanceMethod;
            HI.NamespaceScope = "";
            HI.LocalScope = "cls<int>::";
            HI.Name = "method";
            HI.Parameters.emplace();
            HI.ReturnType = "int";
            HI.Type = "int ()";
          }},
      {
          R"cpp(// type of nested templates.
          template <class T> struct cls {};
          cls<cls<cls<int>>> [[fo^o]];
          )cpp",
          [](HoverInfo &HI) {
            HI.Definition = "cls<cls<cls<int>>> foo";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Name = "foo";
            HI.Type = "cls<cls<cls<int>>>";
          }},
      {
          R"cpp(// type of nested templates.
          template <class T> struct cls {};
          [[cl^s]]<cls<cls<int>>> foo;
          )cpp",
          [](HoverInfo &HI) {
            HI.Definition = "template <> struct cls<cls<cls<int>>> {}";
            HI.Kind = index::SymbolKind::Struct;
            HI.NamespaceScope = "";
            HI.Name = "cls<cls<cls<int>>>";
            HI.Documentation = "type of nested templates.";
          }},
      {
          R"cpp(// type with decltype
          int a;
          decltype(a) [[b^]] = a;)cpp",
          [](HoverInfo &HI) {
            HI.Definition = "decltype(a) b = a";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Name = "b";
            HI.Type = "int";
          }},
      {
          R"cpp(// type with decltype
          int a;
          decltype(a) c;
          decltype(c) [[b^]] = a;)cpp",
          [](HoverInfo &HI) {
            HI.Definition = "decltype(c) b = a";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Name = "b";
            HI.Type = "int";
          }},
      {
          R"cpp(// type with decltype
          int a;
          const decltype(a) [[b^]] = a;)cpp",
          [](HoverInfo &HI) {
            HI.Definition = "const decltype(a) b = a";
            HI.Kind = index::SymbolKind::Variable;
            HI.NamespaceScope = "";
            HI.Name = "b";
            HI.Type = "int";
          }},
      {
          R"cpp(// type with decltype
          int a;
          auto [[f^oo]](decltype(a) x) -> decltype(a) { return 0; })cpp",
          [](HoverInfo &HI) {
            HI.Definition = "auto foo(decltype(a) x) -> decltype(a)";
            HI.Kind = index::SymbolKind::Function;
            HI.NamespaceScope = "";
            HI.Name = "foo";
            // FIXME: Handle composite types with decltype with a printing
            // policy.
            HI.Type = {"auto (decltype(a)) -> decltype(a)",
                       "auto (int) -> int"};
            HI.ReturnType = "int";
            HI.Parameters = {{{"int"}, std::string("x"), std::nullopt}};
          }},
      {
          R"cpp(// sizeof expr
          void foo() {
            (void)[[size^of]](char);
          })cpp",
          [](HoverInfo &HI) {
            HI.Name = "expression";
            HI.Type = {"__size_t", "unsigned long"};
            HI.Value = "1";
          }},
      {
          R"cpp(// alignof expr
          void foo() {
            (void)[[align^of]](char);
          })cpp",
          [](HoverInfo &HI) {
            HI.Name = "expression";
            HI.Type = {"__size_t", "unsigned long"};
            HI.Value = "1";
          }},
      {
          R"cpp(
          template <typename T = int>
          void foo(const T& = T()) {
            [[f^oo]]<>(3);
          })cpp",
          [](HoverInfo &HI) {
            HI.Name = "foo";
            HI.Kind = index::SymbolKind::Function;
            HI.Type = "void (const int &)";
            HI.ReturnType = "void";
            HI.Parameters = {
                {{"const int &"}, std::nullopt, std::string("T()")}};
            HI.Definition = "template <> void foo<int>(const int &)";
            HI.NamespaceScope = "";
          }},
      {
          R"cpp(// should not crash
           @interface ObjC {
             char [[da^ta]];
           }@end
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "data";
            HI.Type = "char";
            HI.Kind = index::SymbolKind::Field;
            HI.LocalScope = "ObjC::";
            HI.NamespaceScope = "";
            HI.Definition = "char data";
          }},
      {
          R"cpp(
          @interface MYObject
          @end
          @interface Interface
          @property(retain) [[MYOb^ject]] *x;
          @end
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MYObject";
            HI.Kind = index::SymbolKind::Class;
            HI.NamespaceScope = "";
            HI.Definition = "@interface MYObject\n@end";
          }},
      {
          R"cpp(
          @interface MYObject
          @end
          @interface Interface
          - (void)doWith:([[MYOb^ject]] *)object;
          @end
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MYObject";
            HI.Kind = index::SymbolKind::Class;
            HI.NamespaceScope = "";
            HI.Definition = "@interface MYObject\n@end";
          }},
      {
          R"cpp(// this expr
          // comment
          namespace ns {
            class Foo {
              Foo* bar() {
                return [[t^his]];
              }
            };
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "this";
            HI.Definition = "ns::Foo *";
          }},
      {
          R"cpp(// this expr for template class
          namespace ns {
            template <typename T>
            class Foo {
              Foo* bar() const {
                return [[t^his]];
              }
            };
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "this";
            HI.Definition = "const Foo<T> *";
          }},
      {
          R"cpp(// this expr for specialization class
          namespace ns {
            template <typename T> class Foo {};
            template <>
            struct Foo<int> {
              Foo* bar() {
                return [[thi^s]];
              }
            };
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "this";
            HI.Definition = "Foo<int> *";
          }},
      {
          R"cpp(// this expr for partial specialization struct
          namespace ns {
            template <typename T, typename F> struct Foo {};
            template <typename F>
            struct Foo<int, F> {
              Foo* bar() const {
                return [[thi^s]];
              }
            };
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "this";
            HI.Definition = "const Foo<int, F> *";
          }},
      {
          R"cpp(
          @interface MYObject
          @end
          @interface MYObject (Private)
          @property(nonatomic, assign) int privateField;
          @end

          int someFunction() {
            MYObject *obj = [MYObject sharedInstance];
            return obj.[[private^Field]];
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "privateField";
            HI.Kind = index::SymbolKind::InstanceProperty;
            HI.LocalScope = "MYObject(Private)::";
            HI.NamespaceScope = "";
            HI.Definition = "@property(nonatomic, assign, unsafe_unretained, "
                            "readwrite) int privateField;";
          }},
      {
          R"cpp(
          @protocol MYProtocol
          @property(nonatomic, assign) int prop1;
          @end

          int someFunction() {
            id<MYProtocol> obj = 0;
            return obj.[[pro^p1]];
          }
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "prop1";
            HI.Kind = index::SymbolKind::InstanceProperty;
            HI.LocalScope = "MYProtocol::";
            HI.NamespaceScope = "";
            HI.Definition = "@property(nonatomic, assign, unsafe_unretained, "
                            "readwrite) int prop1;";
          }},
      {
          R"cpp(
          @protocol MYProtocol
          @end
          @interface MYObject
          @end

          @interface MYObject (Ext) <[[MYProt^ocol]]>
          @end
          )cpp",
          [](HoverInfo &HI) {
            HI.Name = "MYProtocol";
            HI.Kind = index::SymbolKind::Protocol;
            HI.NamespaceScope = "";
            HI.Definition = "@protocol MYProtocol\n@end";
          }},
      {R"objc(
        @interface Foo
        @end

        @implementation Foo(Private)
        + (int)somePrivateMethod {
          int [[res^ult]] = 2;
          return result;
        }
        @end
        )objc",
       [](HoverInfo &HI) {
         HI.Name = "result";
         HI.Definition = "int result = 2";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = "int";
         HI.LocalScope = "+[Foo(Private) somePrivateMethod]::";
         HI.NamespaceScope = "";
         HI.Value = "2";
       }},
      {R"objc(
        @interface Foo
        @end

        @implementation Foo
        - (int)variadicArgMethod:(id)first, ... {
          int [[res^ult]] = 0;
          return result;
        }
        @end
        )objc",
       [](HoverInfo &HI) {
         HI.Name = "result";
         HI.Definition = "int result = 0";
         HI.Kind = index::SymbolKind::Variable;
         HI.Type = "int";
         HI.LocalScope = "-[Foo variadicArgMethod:, ...]::";
         HI.NamespaceScope = "";
         HI.Value = "0";
       }},
      // Should not crash.
      {R"objc(
        typedef struct MyRect {} MyRect;

        @interface IFace
        @property(nonatomic) MyRect frame;
        @end

        MyRect foobar() {
          MyRect mr;
          return mr;
        }
        void test() {
          IFace *v;
          v.frame = [[foo^bar]]();
        }
        )objc",
       [](HoverInfo &HI) {
         HI.Name = "foobar";
         HI.Kind = index::SymbolKind::Function;
         HI.NamespaceScope = "";
         HI.Definition = "MyRect foobar()";
         HI.Type = {"MyRect ()", "MyRect ()"};
         HI.ReturnType = {"MyRect", "MyRect"};
         HI.Parameters.emplace();
       }},
      {R"cpp(
         void foo(int * __attribute__(([[non^null]], noescape)) );
         )cpp",
       [](HoverInfo &HI) {
         HI.Name = "nonnull";
         HI.Kind = index::SymbolKind::Unknown; // FIXME: no suitable value
         HI.Definition = "__attribute__((nonnull))";
         HI.Documentation = Attr::getDocumentation(attr::NonNull).str();
       }},
      {
          R"cpp(
          namespace std {
          struct strong_ordering {
            int n;
            constexpr operator int() const { return n; }
            static const strong_ordering equal, greater, less;
          };
          constexpr strong_ordering strong_ordering::equal = {0};
          constexpr strong_ordering strong_ordering::greater = {1};
          constexpr strong_ordering strong_ordering::less = {-1};
          }

          struct Foo
          {
            int x;
            // Foo spaceship
            auto operator<=>(const Foo&) const = default;
          };

          bool x = Foo(1) [[!^=]] Foo(2);
         )cpp",
          [](HoverInfo &HI) {
            HI.Type = "bool (const Foo &) const noexcept";
            HI.Value = "true";
            HI.Name = "operator==";
            HI.Parameters = {{{"const Foo &"}, std::nullopt, std::nullopt}};
            HI.ReturnType = "bool";
            HI.Kind = index::SymbolKind::InstanceMethod;
            HI.LocalScope = "Foo::";
            HI.NamespaceScope = "";
            HI.Definition =
                "bool operator==(const Foo &) const noexcept = default";
            HI.Documentation = "";
          }},
  };

  // Create a tiny index, so tests above can verify documentation is fetched.
  Symbol IndexSym = func("indexSymbol");
  IndexSym.Documentation = "comment from index";
  SymbolSlab::Builder Symbols;
  Symbols.insert(IndexSym);
  auto Index =
      MemIndex::build(std::move(Symbols).build(), RefSlab(), RelationSlab());

  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Code);

    Annotations T(Case.Code);
    TestTU TU = TestTU::withCode(T.code());
    TU.ExtraArgs.push_back("-std=c++20");
    TU.ExtraArgs.push_back("-xobjective-c++");

    TU.ExtraArgs.push_back("-Wno-gnu-designator");
    // Types might be different depending on the target triplet, we chose a
    // fixed one to make sure tests passes on different platform.
    TU.ExtraArgs.push_back("--target=x86_64-pc-linux-gnu");
    auto AST = TU.build();
    Config Cfg;
    Cfg.Hover.ShowAKA = true;
    WithContextValue WithCfg(Config::Key, std::move(Cfg));
    auto H = getHover(AST, T.point(), format::getLLVMStyle(), Index.get());
    ASSERT_TRUE(H);
    HoverInfo Expected;
    Expected.SymRange = T.range();
    Case.ExpectedBuilder(Expected);

    SCOPED_TRACE(H->present().asPlainText());
    EXPECT_EQ(H->NamespaceScope, Expected.NamespaceScope);
    EXPECT_EQ(H->LocalScope, Expected.LocalScope);
    EXPECT_EQ(H->Name, Expected.Name);
    EXPECT_EQ(H->Kind, Expected.Kind);
    EXPECT_EQ(H->Documentation, Expected.Documentation);
    EXPECT_EQ(H->Definition, Expected.Definition);
    EXPECT_EQ(H->Type, Expected.Type);
    EXPECT_EQ(H->ReturnType, Expected.ReturnType);
    EXPECT_EQ(H->Parameters, Expected.Parameters);
    EXPECT_EQ(H->TemplateParameters, Expected.TemplateParameters);
    EXPECT_EQ(H->SymRange, Expected.SymRange);
    EXPECT_EQ(H->Value, Expected.Value);
  }
}

TEST(Hover, Providers) {
  struct {
    const char *Code;
    const std::function<void(HoverInfo &)> ExpectedBuilder;
  } Cases[] = {{R"cpp(
                  struct Foo {};
                  Foo F = Fo^o{};
                )cpp",
                [](HoverInfo &HI) { HI.Provider = ""; }},
               {R"cpp(
                  #include "foo.h"
                  Foo F = Fo^o{};
                )cpp",
                [](HoverInfo &HI) { HI.Provider = "\"foo.h\""; }},
               {R"cpp(
                  #include "all.h"
                  Foo F = Fo^o{};
                )cpp",
                [](HoverInfo &HI) { HI.Provider = "\"foo.h\""; }},
               {R"cpp(
                  #define FOO 5
                  int F = ^FOO;
                )cpp",
                [](HoverInfo &HI) { HI.Provider = ""; }},
               {R"cpp(
                  #include "foo.h"
                  int F = ^FOO;
                )cpp",
                [](HoverInfo &HI) { HI.Provider = "\"foo.h\""; }},
               {R"cpp(
                  #include "all.h"
                  int F = ^FOO;
                )cpp",
                [](HoverInfo &HI) { HI.Provider = "\"foo.h\""; }},
               {R"cpp(
                  #include "foo.h"
                  Foo A;
                  Foo B;
                  Foo C = A ^+ B;
                )cpp",
                [](HoverInfo &HI) { HI.Provider = "\"foo.h\""; }},
               // Hover selects the underlying decl of the using decl
               {R"cpp(
                  #include "foo.h"
                  namespace ns {
                    using ::Foo;
                  }
                  ns::F^oo d;
                )cpp",
                [](HoverInfo &HI) { HI.Provider = "\"foo.h\""; }},
                {R"cpp(
                  namespace foo {};
                  using namespace fo^o;
                )cpp",
                [](HoverInfo &HI) { HI.Provider = ""; }},
                };

  for (const auto &Case : Cases) {
    Annotations Code{Case.Code};
    SCOPED_TRACE(Code.code());

    TestTU TU;
    TU.Filename = "foo.cpp";
    TU.Code = Code.code();
    TU.AdditionalFiles["foo.h"] = guard(R"cpp(
                                          #define FOO 1
                                          class Foo {};
                                          Foo& operator+(const Foo, const Foo);
                                        )cpp");
    TU.AdditionalFiles["all.h"] = guard("#include \"foo.h\"");

    auto AST = TU.build();
    auto H = getHover(AST, Code.point(), format::getLLVMStyle(), nullptr);
    ASSERT_TRUE(H);
    HoverInfo Expected;
    Case.ExpectedBuilder(Expected);
    SCOPED_TRACE(H->present().asMarkdown());
    EXPECT_EQ(H->Provider, Expected.Provider);
  }
}

TEST(Hover, ParseProviderInfo) {
  HoverInfo HIFoo;
  HIFoo.Name = "foo";
  HIFoo.Provider = "\"foo.h\"";

  HoverInfo HIFooBar;
  HIFooBar.Name = "foo";
  HIFooBar.Provider = "<bar.h>";
  struct Case {
    HoverInfo HI;
    llvm::StringRef ExpectedMarkdown;
  } Cases[] = {{HIFoo, "### `foo`  \nprovided by `\"foo.h\"`"},
               {HIFooBar, "### `foo`  \nprovided by `<bar.h>`"}};

  for (const auto &Case : Cases)
    EXPECT_EQ(Case.HI.present().asMarkdown(), Case.ExpectedMarkdown);
}

TEST(Hover, UsedSymbols) {
  struct {
    const char *Code;
    const std::function<void(HoverInfo &)> ExpectedBuilder;
  } Cases[] = {{R"cpp(
                  #include ^"bar.h"
                  int fstBar = bar1();
                  int another= bar1(0);
                  int sndBar = bar2();
                  Bar bar;
                  int macroBar = BAR;
                )cpp",
                [](HoverInfo &HI) {
                  HI.UsedSymbolNames = {"BAR", "Bar", "bar1", "bar2"};
                }},
               {R"cpp(
                  #in^clude <vector>
                  std::vector<int> vec;
                )cpp",
                [](HoverInfo &HI) { HI.UsedSymbolNames = {"vector"}; }}};
  for (const auto &Case : Cases) {
    Annotations Code{Case.Code};
    SCOPED_TRACE(Code.code());

    TestTU TU;
    TU.Filename = "foo.cpp";
    TU.Code = Code.code();
    TU.AdditionalFiles["bar.h"] = guard(R"cpp(
                                          #define BAR 5
                                          int bar1();
                                          int bar2();
                                          int bar1(double);
                                          class Bar {};
                                        )cpp");
    TU.AdditionalFiles["system/vector"] = guard(R"cpp(
      namespace std {
        template<typename>
        class vector{};
      }
    )cpp");
    TU.ExtraArgs.push_back("-isystem" + testPath("system"));

    auto AST = TU.build();
    auto H = getHover(AST, Code.point(), format::getLLVMStyle(), nullptr);
    ASSERT_TRUE(H);
    HoverInfo Expected;
    Case.ExpectedBuilder(Expected);
    SCOPED_TRACE(H->present().asMarkdown());
    EXPECT_EQ(H->UsedSymbolNames, Expected.UsedSymbolNames);
  }
}

TEST(Hover, DocsFromIndex) {
  Annotations T(R"cpp(
  template <typename T> class X {};
  void foo() {
    auto t = X<int>();
    X^<int> w;
    (void)w;
  })cpp");

  TestTU TU = TestTU::withCode(T.code());
  auto AST = TU.build();
  Symbol IndexSym;
  IndexSym.ID = getSymbolID(&findDecl(AST, "X"));
  IndexSym.Documentation = "comment from index";
  SymbolSlab::Builder Symbols;
  Symbols.insert(IndexSym);
  auto Index =
      MemIndex::build(std::move(Symbols).build(), RefSlab(), RelationSlab());

  for (const auto &P : T.points()) {
    auto H = getHover(AST, P, format::getLLVMStyle(), Index.get());
    ASSERT_TRUE(H);
    EXPECT_EQ(H->Documentation, IndexSym.Documentation);
  }
}

TEST(Hover, DocsFromAST) {
  Annotations T(R"cpp(
  // doc
  template <typename T> class X {};
  // doc
  template <typename T> void bar() {}
  // doc
  template <typename T> T baz;
  void foo() {
    au^to t = X<int>();
    X^<int>();
    b^ar<int>();
    au^to T = ba^z<X<int>>;
    ba^z<int> = 0;
  })cpp");

  TestTU TU = TestTU::withCode(T.code());
  auto AST = TU.build();
  for (const auto &P : T.points()) {
    auto H = getHover(AST, P, format::getLLVMStyle(), nullptr);
    ASSERT_TRUE(H);
    EXPECT_EQ(H->Documentation, "doc");
  }
}

TEST(Hover, NoCrash) {
  Annotations T(R"cpp(
    /* error-ok */
    template<typename T> T foo(T);

    // Setter variable heuristic might fail if the callexpr is broken.
    struct X { int Y; void [[^setY]](float) { Y = foo(undefined); } };)cpp");

  TestTU TU = TestTU::withCode(T.code());
  auto AST = TU.build();
  for (const auto &P : T.points())
    getHover(AST, P, format::getLLVMStyle(), nullptr);
}

TEST(Hover, NoCrashAPInt64) {
  Annotations T(R"cpp(
    constexpr unsigned long value = -1; // wrap around
    void foo() { va^lue; }
  )cpp");
  auto AST = TestTU::withCode(T.code()).build();
  getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
}

TEST(Hover, NoCrashInt128) {
  Annotations T(R"cpp(
    constexpr __int128_t value = -4;
    void foo() { va^lue; }
  )cpp");
  auto TU = TestTU::withCode(T.code());
  // Need a triple that support __int128_t.
  TU.ExtraArgs.push_back("--target=x86_64-pc-linux-gnu");
  auto AST = TU.build();
  auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
  ASSERT_TRUE(H);
  EXPECT_EQ(H->Value, "-4 (0xfffffffc)");
}

TEST(Hover, DocsFromMostSpecial) {
  Annotations T(R"cpp(
  // doc1
  template <typename T> class $doc1^X {};
  // doc2
  template <> class $doc2^X<int> {};
  // doc3
  template <typename T> class $doc3^X<T*> {};
  void foo() {
    X$doc1^<char>();
    X$doc2^<int>();
    X$doc3^<int*>();
  })cpp");

  TestTU TU = TestTU::withCode(T.code());
  auto AST = TU.build();
  for (const auto *Comment : {"doc1", "doc2", "doc3"}) {
    for (const auto &P : T.points(Comment)) {
      auto H = getHover(AST, P, format::getLLVMStyle(), nullptr);
      ASSERT_TRUE(H);
      EXPECT_EQ(H->Documentation, Comment);
    }
  }
}

TEST(Hover, Present) {
  struct {
    const std::function<void(HoverInfo &)> Builder;
    llvm::StringRef ExpectedRender;
  } Cases[] = {
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Unknown;
            HI.Name = "X";
          },
          R"(X)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::NamespaceAlias;
            HI.Name = "foo";
          },
          R"(namespace-alias foo)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Class;
            HI.Size = 80;
            HI.TemplateParameters = {
                {{"typename"}, std::string("T"), std::nullopt},
                {{"typename"}, std::string("C"), std::string("bool")},
            };
            HI.Documentation = "documentation";
            HI.Definition =
                "template <typename T, typename C = bool> class Foo {}";
            HI.Name = "foo";
            HI.NamespaceScope.emplace();
          },
          R"(class foo

Size: 10 bytes
documentation

template <typename T, typename C = bool> class Foo {})",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Function;
            HI.Name = "foo";
            HI.Type = {"type", "c_type"};
            HI.ReturnType = {"ret_type", "can_ret_type"};
            HI.Parameters.emplace();
            HoverInfo::Param P;
            HI.Parameters->push_back(P);
            P.Type = {"type", "can_type"};
            HI.Parameters->push_back(P);
            P.Name = "foo";
            HI.Parameters->push_back(P);
            P.Default = "default";
            HI.Parameters->push_back(P);
            HI.NamespaceScope = "ns::";
            HI.Definition = "ret_type foo(params) {}";
          },
          "function foo\n"
          "\n"
          "→ ret_type (aka can_ret_type)\n"
          "Parameters:\n"
          "- \n"
          "- type (aka can_type)\n"
          "- type foo (aka can_type)\n"
          "- type foo = default (aka can_type)\n"
          "\n"
          "// In namespace ns\n"
          "ret_type foo(params) {}",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Field;
            HI.LocalScope = "test::Bar::";
            HI.Value = "value";
            HI.Name = "foo";
            HI.Type = {"type", "can_type"};
            HI.Definition = "def";
            HI.Size = 32;
            HI.Offset = 96;
            HI.Padding = 32;
            HI.Align = 32;
          },
          R"(field foo

Type: type (aka can_type)
Value = value
Offset: 12 bytes
Size: 4 bytes (+4 bytes padding), alignment 4 bytes

// In test::Bar
def)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Field;
            HI.LocalScope = "test::Bar::";
            HI.Value = "value";
            HI.Name = "foo";
            HI.Type = {"type", "can_type"};
            HI.Definition = "def";
            HI.Size = 25;
            HI.Offset = 35;
            HI.Padding = 4;
            HI.Align = 64;
          },
          R"(field foo

Type: type (aka can_type)
Value = value
Offset: 4 bytes and 3 bits
Size: 25 bits (+4 bits padding), alignment 8 bytes

// In test::Bar
def)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Field;
            HI.AccessSpecifier = "public";
            HI.Name = "foo";
            HI.LocalScope = "test::Bar::";
            HI.Definition = "def";
          },
          R"(field foo

// In test::Bar
public: def)",
      },
      {
          [](HoverInfo &HI) {
            HI.Definition = "size_t method()";
            HI.AccessSpecifier = "protected";
            HI.Kind = index::SymbolKind::InstanceMethod;
            HI.NamespaceScope = "";
            HI.LocalScope = "cls<int>::";
            HI.Name = "method";
            HI.Parameters.emplace();
            HI.ReturnType = {"size_t", "unsigned long"};
            HI.Type = {"size_t ()", "unsigned long ()"};
          },
          R"(instance-method method

→ size_t (aka unsigned long)

// In cls<int>
protected: size_t method())",
      },
      {
          [](HoverInfo &HI) {
            HI.Definition = "cls(int a, int b = 5)";
            HI.AccessSpecifier = "public";
            HI.Kind = index::SymbolKind::Constructor;
            HI.NamespaceScope = "";
            HI.LocalScope = "cls";
            HI.Name = "cls";
            HI.Parameters.emplace();
            HI.Parameters->emplace_back();
            HI.Parameters->back().Type = "int";
            HI.Parameters->back().Name = "a";
            HI.Parameters->emplace_back();
            HI.Parameters->back().Type = "int";
            HI.Parameters->back().Name = "b";
            HI.Parameters->back().Default = "5";
          },
          R"(constructor cls

Parameters:
- int a
- int b = 5

// In cls
public: cls(int a, int b = 5))",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Union;
            HI.AccessSpecifier = "private";
            HI.Name = "foo";
            HI.NamespaceScope = "ns1::";
            HI.Definition = "union foo {}";
          },
          R"(union foo

// In namespace ns1
private: union foo {})",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Variable;
            HI.Name = "foo";
            HI.Definition = "int foo = 3";
            HI.LocalScope = "test::Bar::";
            HI.Value = "3";
            HI.Type = "int";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "arg_a";
            HI.CalleeArgInfo->Type = "int";
            HI.CalleeArgInfo->Default = "7";
            HI.CallPassType = HoverInfo::PassType{PassMode::Value, false};
          },
          R"(variable foo

Type: int
Value = 3
Passed as arg_a

// In test::Bar
int foo = 3)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Variable;
            HI.Name = "foo";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Type = "int";
            HI.CallPassType = HoverInfo::PassType{PassMode::Value, false};
          },
          R"(variable foo

Passed by value)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Variable;
            HI.Name = "foo";
            HI.Definition = "int foo = 3";
            HI.LocalScope = "test::Bar::";
            HI.Value = "3";
            HI.Type = "int";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "arg_a";
            HI.CalleeArgInfo->Type = "int";
            HI.CalleeArgInfo->Default = "7";
            HI.CallPassType = HoverInfo::PassType{PassMode::Ref, false};
          },
          R"(variable foo

Type: int
Value = 3
Passed by reference as arg_a

// In test::Bar
int foo = 3)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Variable;
            HI.Name = "foo";
            HI.Definition = "int foo = 3";
            HI.LocalScope = "test::Bar::";
            HI.Value = "3";
            HI.Type = "int";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "arg_a";
            HI.CalleeArgInfo->Type = {"alias_int", "int"};
            HI.CalleeArgInfo->Default = "7";
            HI.CallPassType = HoverInfo::PassType{PassMode::Value, true};
          },
          R"(variable foo

Type: int
Value = 3
Passed as arg_a (converted to alias_int)

// In test::Bar
int foo = 3)",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Macro;
            HI.Name = "PLUS_ONE";
            HI.Definition = "#define PLUS_ONE(X) (X+1)\n\n"
                            "// Expands to\n"
                            "(1 + 1)";
          },
          R"(macro PLUS_ONE

#define PLUS_ONE(X) (X+1)

// Expands to
(1 + 1))",
      },
      {
          [](HoverInfo &HI) {
            HI.Kind = index::SymbolKind::Variable;
            HI.Name = "foo";
            HI.Definition = "int foo = 3";
            HI.LocalScope = "test::Bar::";
            HI.Value = "3";
            HI.Type = "int";
            HI.CalleeArgInfo.emplace();
            HI.CalleeArgInfo->Name = "arg_a";
            HI.CalleeArgInfo->Type = "int";
            HI.CalleeArgInfo->Default = "7";
            HI.CallPassType = HoverInfo::PassType{PassMode::ConstRef, true};
          },
          R"(variable foo

Type: int
Value = 3
Passed by const reference as arg_a (converted to int)

// In test::Bar
int foo = 3)",
      },
      {
          [](HoverInfo &HI) {
            HI.Name = "stdio.h";
            HI.Definition = "/usr/include/stdio.h";
          },
          R"(stdio.h

/usr/include/stdio.h)",
      },
      {[](HoverInfo &HI) {
         HI.Name = "foo.h";
         HI.UsedSymbolNames = {"Foo", "Bar", "Bar"};
       },
       R"(foo.h

provides Foo, Bar, Bar)"},
      {[](HoverInfo &HI) {
         HI.Name = "foo.h";
         HI.UsedSymbolNames = {"Foo", "Bar", "Baz", "Foobar", "Qux", "Quux"};
       },
       R"(foo.h

provides Foo, Bar, Baz, Foobar, Qux and 1 more)"}};

  for (const auto &C : Cases) {
    HoverInfo HI;
    C.Builder(HI);
    Config Cfg;
    Cfg.Hover.ShowAKA = true;
    WithContextValue WithCfg(Config::Key, std::move(Cfg));
    EXPECT_EQ(HI.present().asPlainText(), C.ExpectedRender);
  }
}

TEST(Hover, ParseDocumentation) {
  struct Case {
    llvm::StringRef Documentation;
    llvm::StringRef ExpectedRenderMarkdown;
    llvm::StringRef ExpectedRenderPlainText;
  } Cases[] = {{
                   " \n foo\nbar",
                   "foo bar",
                   "foo bar",
               },
               {
                   "foo\nbar \n  ",
                   "foo bar",
                   "foo bar",
               },
               {
                   "foo  \nbar",
                   "foo bar",
                   "foo bar",
               },
               {
                   "foo    \nbar",
                   "foo bar",
                   "foo bar",
               },
               {
                   "foo\n\n\nbar",
                   "foo  \nbar",
                   "foo\nbar",
               },
               {
                   "foo\n\n\n\tbar",
                   "foo  \nbar",
                   "foo\nbar",
               },
               {
                   "foo\n\n\n bar",
                   "foo  \nbar",
                   "foo\nbar",
               },
               {
                   "foo.\nbar",
                   "foo.  \nbar",
                   "foo.\nbar",
               },
               {
                   "foo. \nbar",
                   "foo.  \nbar",
                   "foo.\nbar",
               },
               {
                   "foo\n*bar",
                   "foo  \n\\*bar",
                   "foo\n*bar",
               },
               {
                   "foo\nbar",
                   "foo bar",
                   "foo bar",
               },
               {
                   "Tests primality of `p`.",
                   "Tests primality of `p`.",
                   "Tests primality of `p`.",
               },
               {
                   "'`' should not occur in `Code`",
                   "'\\`' should not occur in `Code`",
                   "'`' should not occur in `Code`",
               },
               {
                   "`not\nparsed`",
                   "\\`not parsed\\`",
                   "`not parsed`",
               }};

  for (const auto &C : Cases) {
    markup::Document Output;
    parseDocumentation(C.Documentation, Output);

    EXPECT_EQ(Output.asMarkdown(), C.ExpectedRenderMarkdown);
    EXPECT_EQ(Output.asPlainText(), C.ExpectedRenderPlainText);
  }
}

// This is a separate test as headings don't create any differences in
// plaintext mode.
TEST(Hover, PresentHeadings) {
  HoverInfo HI;
  HI.Kind = index::SymbolKind::Variable;
  HI.Name = "foo";

  EXPECT_EQ(HI.present().asMarkdown(), "### variable `foo`");
}

// This is a separate test as rulers behave differently in markdown vs
// plaintext.
TEST(Hover, PresentRulers) {
  HoverInfo HI;
  HI.Kind = index::SymbolKind::Variable;
  HI.Name = "foo";
  HI.Value = "val";
  HI.Definition = "def";

  llvm::StringRef ExpectedMarkdown = //
      "### variable `foo`  \n"
      "\n"
      "---\n"
      "Value = `val`  \n"
      "\n"
      "---\n"
      "```cpp\n"
      "def\n"
      "```";
  EXPECT_EQ(HI.present().asMarkdown(), ExpectedMarkdown);

  llvm::StringRef ExpectedPlaintext = R"pt(variable foo

Value = val

def)pt";
  EXPECT_EQ(HI.present().asPlainText(), ExpectedPlaintext);
}

TEST(Hover, SpaceshipTemplateNoCrash) {
  Annotations T(R"cpp(
  namespace std {
  struct strong_ordering {
    int n;
    constexpr operator int() const { return n; }
    static const strong_ordering equal, greater, less;
  };
  constexpr strong_ordering strong_ordering::equal = {0};
  constexpr strong_ordering strong_ordering::greater = {1};
  constexpr strong_ordering strong_ordering::less = {-1};
  }

  template <typename T>
  struct S {
    // Foo bar baz
    friend auto operator<=>(S, S) = default;
  };
  static_assert(S<void>() =^= S<void>());
    )cpp");

  TestTU TU = TestTU::withCode(T.code());
  TU.ExtraArgs.push_back("-std=c++20");
  auto AST = TU.build();
  auto HI = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
  EXPECT_EQ(HI->Documentation, "");
}

TEST(Hover, ForwardStructNoCrash) {
  Annotations T(R"cpp(
  struct Foo;
  int bar;
  auto baz = (Fo^o*)&bar;
    )cpp");

  TestTU TU = TestTU::withCode(T.code());
  auto AST = TU.build();
  auto HI = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
  ASSERT_TRUE(HI);
  EXPECT_EQ(*HI->Value, "&bar");
}

TEST(Hover, FunctionParameterDefaulValueNotEvaluatedOnInvalidDecls) {
  struct {
    const char *const Code;
    const std::optional<std::string> HoverValue;
  } Cases[] = {
      {R"cpp(
        // error-ok testing behavior on invalid decl
        class Foo {};
        void foo(Foo p^aram = nullptr);
        )cpp",
       std::nullopt},
      {R"cpp(
        class Foo {};
        void foo(Foo *p^aram = nullptr);
        )cpp",
       "nullptr"},
  };

  for (const auto &C : Cases) {
    Annotations T(C.Code);
    TestTU TU = TestTU::withCode(T.code());
    auto AST = TU.build();
    auto HI = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
    ASSERT_TRUE(HI);
    ASSERT_EQ(HI->Value, C.HoverValue);
  }
}

TEST(Hover, DisableShowAKA) {
  Annotations T(R"cpp(
    using m_int = int;
    m_int ^[[a]];
  )cpp");

  Config Cfg;
  Cfg.Hover.ShowAKA = false;
  WithContextValue WithCfg(Config::Key, std::move(Cfg));

  TestTU TU = TestTU::withCode(T.code());
  TU.ExtraArgs.push_back("-std=c++17");
  auto AST = TU.build();
  auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);

  ASSERT_TRUE(H);
  EXPECT_EQ(H->Type, HoverInfo::PrintedType("m_int"));
}

TEST(Hover, HideBigInitializers) {
  Annotations T(R"cpp(
  #define A(x) x, x, x, x
  #define B(x) A(A(A(A(x))))
  int a^rr[] = {B(0)};
  )cpp");

  TestTU TU = TestTU::withCode(T.code());
  auto AST = TU.build();
  auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);

  ASSERT_TRUE(H);
  EXPECT_EQ(H->Definition, "int arr[]");
}

#if defined(__aarch64__)
// FIXME: AARCH64 sanitizer buildbots are broken after 72142fbac4.
#define PREDEFINEMACROS_TEST(x) DISABLED_##x
#else
#define PREDEFINEMACROS_TEST(x) x
#endif

TEST(Hover, PREDEFINEMACROS_TEST(GlobalVarEnumeralCastNoCrash)) {
  Annotations T(R"cpp(
    using uintptr_t = __UINTPTR_TYPE__;
    enum Test : uintptr_t {};
    unsigned global_var;
    void foo() {
      Test v^al = static_cast<Test>(reinterpret_cast<uintptr_t>(&global_var));
    }
  )cpp");

  TestTU TU = TestTU::withCode(T.code());
  TU.PredefineMacros = true;
  auto AST = TU.build();
  auto HI = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
  ASSERT_TRUE(HI);
  EXPECT_EQ(*HI->Value, "&global_var");
}

TEST(Hover, PREDEFINEMACROS_TEST(GlobalVarIntCastNoCrash)) {
  Annotations T(R"cpp(
    using uintptr_t = __UINTPTR_TYPE__;
    unsigned global_var;
    void foo() {
      uintptr_t a^ddress = reinterpret_cast<uintptr_t>(&global_var);
    }
  )cpp");

  TestTU TU = TestTU::withCode(T.code());
  TU.PredefineMacros = true;
  auto AST = TU.build();
  auto HI = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);
  ASSERT_TRUE(HI);
  EXPECT_EQ(*HI->Value, "&global_var");
}

TEST(Hover, Typedefs) {
  Annotations T(R"cpp(
  template <bool X, typename T, typename F>
  struct cond { using type = T; };
  template <typename T, typename F>
  struct cond<false, T, F> { using type = F; };

  template <bool X, typename T, typename F>
  using type = typename cond<X, T, F>::type;

  void foo() {
    using f^oo = type<true, int, double>;
  }
  )cpp");

  TestTU TU = TestTU::withCode(T.code());
  auto AST = TU.build();
  auto H = getHover(AST, T.point(), format::getLLVMStyle(), nullptr);

  ASSERT_TRUE(H && H->Type);
  EXPECT_EQ(H->Type->Type, "int");
  EXPECT_EQ(H->Definition, "using foo = type<true, int, double>");
}

TEST(Hover, EvaluateMacros) {
  llvm::StringRef PredefinedCXX = R"cpp(
#define X 42
#define SizeOf sizeof
#define AlignOf alignof
#define PLUS_TWO +2
#define TWO 2

using u64 = unsigned long long;
// calculate (a ** b) % p
constexpr u64 pow_with_mod(u64 a, u64 b, u64 p) {
  u64 ret = 1;
  while (b) {
    if (b & 1)
      ret = (ret * a) % p;
    a = (a * a) % p;
    b >>= 1;
  }
  return ret;
}
#define last_n_digit(x, y, n)                                                  \
  pow_with_mod(x, y, pow_with_mod(10, n, 2147483647))
#define declare_struct(X, name, value)                                         \
  struct X {                                                                   \
    constexpr auto name() { return value; }                                    \
  }
#define gnu_statement_expression(value)                                        \
  ({                                                                           \
    declare_struct(Widget, getter, value);                                     \
    Widget().getter();                                                         \
  })
#define define_lambda_begin(lambda, ...)                                       \
  [&](__VA_ARGS__) {
#define define_lambda_end() }

#define left_bracket [
#define right_bracket ]
#define dg_left_bracket <:
#define dg_right_bracket :>
#define array_decl(type, name, size) type name left_bracket size right_bracket
  )cpp";

  struct {
    llvm::StringRef Code;
    const std::function<void(std::optional<HoverInfo>, size_t /*Id*/)>
        Validator;
  } Cases[] = {
      {
          /*Code=*/R"cpp(
            X^;
          )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_EQ(HI->Value, "42 (0x2a)");
            EXPECT_EQ(HI->Type, HoverInfo::PrintedType("int"));
          },
      },
      {
          /*Code=*/R"cpp(
            Size^Of(int);
          )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_TRUE(HI->Value);
            EXPECT_TRUE(HI->Type);
            // Don't validate type or value of `sizeof` and `alignof` as we're
            // getting different values or desugared types on different
            // platforms. Same as below.
          },
      },
      {
          /*Code=*/R"cpp(
          struct Y {
            int y;
            double z;
          };
          Alig^nOf(Y);
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_TRUE(HI->Value);
            EXPECT_TRUE(HI->Type);
          },
      },
      {
          /*Code=*/R"cpp(
          // 2**32 == 4294967296
          last_n_di^git(2, 32, 6);
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_EQ(HI->Value, "967296 (0xec280)");
            EXPECT_EQ(HI->Type, "u64");
          },
      },
      {
          /*Code=*/R"cpp(
          gnu_statement_exp^ression(42);
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_EQ(HI->Value, "42 (0x2a)");
            EXPECT_EQ(HI->Type, "int");
          },
      },
      {
          /*Code=*/R"cpp(
          40 + PLU^S_TWO;
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_EQ(HI->Value, "2");
            EXPECT_EQ(HI->Type, "int");
          },
      },
      {
          /*Code=*/R"cpp(
          40 PLU^S_TWO;
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_FALSE(HI->Value) << HI->Value;
            EXPECT_FALSE(HI->Type) << HI->Type;
          },
      },
      {
          /*Code=*/R"cpp(
          40 + TW^O;
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t) {
            EXPECT_EQ(HI->Value, "2");
            EXPECT_EQ(HI->Type, "int");
          },
      },
      {
          /*Code=*/R"cpp(
          arra^y_decl(int, vector, 10);
          vector left_b^racket 3 right_b^racket;
          vector dg_le^ft_bracket 3 dg_righ^t_bracket;
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t Id) {
            switch (Id) {
            case 0:
              EXPECT_EQ(HI->Type, HoverInfo::PrintedType("int[10]"));
              break;
            case 1:
            case 2:
            case 3:
            case 4:
              EXPECT_FALSE(HI->Type) << HI->Type;
              EXPECT_FALSE(HI->Value) << HI->Value;
              break;
            default:
              ASSERT_TRUE(false) << "Unhandled id: " << Id;
            }
          },
      },
      {
          /*Code=*/R"cpp(
          constexpr auto value = define_lamb^da_begin(lambda, int, char)
            // Check if the expansion range is right.
            return ^last_n_digit(10, 3, 3)^;
          define_lam^bda_end();
        )cpp",
          /*Validator=*/
          [](std::optional<HoverInfo> HI, size_t Id) {
            switch (Id) {
            case 0:
              EXPECT_FALSE(HI->Value);
              EXPECT_EQ(HI->Type, HoverInfo::PrintedType("const (lambda)"));
              break;
            case 1:
              EXPECT_EQ(HI->Value, "0");
              EXPECT_EQ(HI->Type, HoverInfo::PrintedType("u64"));
              break;
            case 2:
              EXPECT_FALSE(HI);
              break;
            case 3:
              EXPECT_FALSE(HI->Type) << HI->Type;
              EXPECT_FALSE(HI->Value) << HI->Value;
              break;
            default:
              ASSERT_TRUE(false) << "Unhandled id: " << Id;
            }
          },
      },
  };

  Config Cfg;
  Cfg.Hover.ShowAKA = false;
  WithContextValue WithCfg(Config::Key, std::move(Cfg));
  for (const auto &C : Cases) {
    Annotations Code(
        (PredefinedCXX + "void function() {\n" + C.Code + "}\n").str());
    auto TU = TestTU::withCode(Code.code());
    TU.ExtraArgs.push_back("-std=c++17");
    auto AST = TU.build();
    for (auto [Index, Position] : llvm::enumerate(Code.points())) {
      C.Validator(getHover(AST, Position, format::getLLVMStyle(), nullptr),
                  Index);
    }
  }

  Annotations C(R"c(
    #define alignof _Alignof
    void foo() {
      al^ignof(struct { int x; char y[10]; });
    }
  )c");

  auto TU = TestTU::withCode(C.code());
  TU.Filename = "TestTU.c";
  TU.ExtraArgs = {
      "-std=c17",
  };
  auto AST = TU.build();
  auto H = getHover(AST, C.point(), format::getLLVMStyle(), nullptr);

  ASSERT_TRUE(H);
  EXPECT_TRUE(H->Value);
  EXPECT_TRUE(H->Type);
}
} // namespace
} // namespace clangd
} // namespace clang
