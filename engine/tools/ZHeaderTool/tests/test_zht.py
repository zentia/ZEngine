"""Unit tests for ZHeaderTool. Stdlib only:

    python3 engine/tools/ZHeaderTool/tests/test_zht.py
"""

import io
import os
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from zht import cli  # noqa: E402
from zht.codegen import generate_header, generate_source  # noqa: E402
from zht.lexer import IDENT, STRING, tokenize  # noqa: E402
from zht.parser import ParseError, default_serialized_name, has_annotations, parse_header  # noqa: E402

LIGHT_H = """#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "LightComponent.generated.h"

ZCLASS()
class LightComponent : public Component
{
    ZGENERATED_BODY(LightComponent)

public:
    void SetFlux(const Vector3& flux) { m_Flux = flux; }
    float GetRadius() const { return m_Radius; }

private:
    ZPROPERTY()
    Vector3 m_Flux {Vector3(100.0f, 100.0f, 100.0f)};
    ZPROPERTY()
    float m_Radius {8.0f};
};
"""


def parse(text, name="Foo.h"):
    return parse_header("/src/" + name, "Runtime/" + name, text)


def wrap(body, cls="Foo", extra_head="", spec="", key="class", base=" : public Component"):
    return (
        '#include "%s.generated.h"\n' % cls
        + extra_head
        + "ZCLASS(%s)\n%s %s%s\n{\n    ZGENERATED_BODY(%s)\n%s\n};\n" % (spec, key, cls, base, cls, body)
    )


class LexerTests(unittest.TestCase):
    def test_comments_strings_and_preprocessor_are_skipped(self):
        src = (
            "// ZCLASS() in a comment\n"
            "/* ZCLASS()\n   still comment */\n"
            "#define ZCLASS(...) \\\n   nothing\n"
            'const char* s = "ZCLASS()";\n'
            'auto r = R"x(ZCLASS() )" )x";\n'
            "char c = '\\'';\n"
            "int after;\n"
        )
        lexed = tokenize(src)
        idents = [t.text for t in lexed.tokens if t.kind == IDENT]
        self.assertNotIn("ZCLASS", idents)
        self.assertIn("after", idents)
        strings = [t.text for t in lexed.tokens if t.kind == STRING]
        self.assertEqual(strings, ["ZCLASS()", 'ZCLASS() )" '])
        after = next(t for t in lexed.tokens if t.text == "after")
        self.assertEqual(after.line, 9)

    def test_includes_are_recorded_with_lines(self):
        lexed = tokenize('#include <vector>\n  #  include "a/b.h" // c\n')
        self.assertEqual([(i.path, i.line) for i in lexed.includes], [("vector", 1), ("a/b.h", 2)])

    def test_scope_operator(self):
        toks = [t.text for t in tokenize("std::vector<int>").tokens]
        self.assertEqual(toks, ["std", "::", "vector", "<", "int", ">"])


class NamingTests(unittest.TestCase):
    def test_default_serialized_name(self):
        self.assertEqual(default_serialized_name("m_Flux"), "flux")
        self.assertEqual(default_serialized_name("m_SourceRelPath"), "source_rel_path")
        self.assertEqual(default_serialized_name("m_IsMainCamera"), "is_main_camera")
        self.assertEqual(default_serialized_name("m_UVScale"), "uv_scale")
        self.assertEqual(default_serialized_name("m_Texture2D"), "texture2_d")
        self.assertEqual(default_serialized_name("radius"), "radius")


class ParserTests(unittest.TestCase):
    def test_light_component(self):
        info = parse(LIGHT_H, "LightComponent.h")
        self.assertEqual(len(info.classes), 1)
        cls = info.classes[0]
        self.assertEqual((cls.name, cls.base, cls.class_key), ("LightComponent", "Component", "class"))
        self.assertEqual([(p.member_name, p.cpp_type, p.serialized_name, p.access) for p in cls.properties],
                         [("m_Flux", "Vector3", "flux", "private"), ("m_Radius", "float", "radius", "private")])

    def test_specifiers_flags_and_meta(self):
        info = parse(wrap(
            '    ZPROPERTY(Name="main", HideInEditor, ReadOnly, meta=(Tooltip="t", ClampMin=-1.5, ClampMax=10))\n'
            "    float m_IsMainCamera {0.0f};\n"
            "    ZPROPERTY(Transient)\n"
            "    int m_Cache = 0;\n"
            "    ZPROPERTY()\n"
            "    std::map<int, std::vector<float>> m_Table;\n"
        ))
        p0, p1, p2 = info.classes[0].properties
        self.assertEqual(p0.serialized_name, "main")
        self.assertEqual(p0.transfer_flags, ["HideInEditorMask", "DisallowSerializedPropertyModification"])
        self.assertEqual(p0.meta, {"Tooltip": "t", "ClampMin": -1.5, "ClampMax": 10})
        self.assertTrue(p1.transient)
        self.assertEqual(p2.cpp_type, "std::map<int, std::vector<float>>")

    def test_struct_defaults_to_public_and_abstract_flag(self):
        info = parse(wrap("    ZPROPERTY()\n    int m_A;\n", key="struct", spec="Abstract"))
        cls = info.classes[0]
        self.assertTrue(cls.abstract)
        self.assertEqual(cls.properties[0].access, "public")

    def test_nested_braces_and_functions_are_skipped(self):
        info = parse(wrap(
            "    struct Inner { int x; void f() { if (1) { } } };\n"
            "    void g() { int y = 0; }\n"
            "    ZPROPERTY()\n"
            "    float m_V = 1.0f;\n"
        ))
        self.assertEqual([p.member_name for p in info.classes[0].properties], ["m_V"])

    def test_non_annotated_code_around_is_ignored(self):
        text = (
            '#include "Foo.generated.h"\n'
            "enum class Mode : unsigned char { A, B };\n"
            "template<> class SerializeTraits<Mode> { public: static void T() {} };\n"
            "ZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) };\n"
            "class Other { int z; };\n"
        )
        self.assertEqual([c.name for c in parse(text).classes], ["Foo"])

    def _assert_error(self, text, fragment):
        with self.assertRaises(ParseError) as ctx:
            parse(text)
        self.assertIn(fragment, ctx.exception.message)
        return ctx.exception

    def test_errors(self):
        cases = [
            (wrap("    ZPROPERTY(Bogus)\n    int m_A;\n"), "unknown ZPROPERTY specifier 'Bogus'"),
            (wrap("    ZPROPERTY(meta=(Nope=1))\n    int m_A;\n"), "unknown meta specifier 'Nope'"),
            (wrap("    ZPROPERTY(Name=3)\n    int m_A;\n"), "'Name' expects a string literal"),
            (wrap("    ZPROPERTY(meta=(ClampMin=\"x\"))\n    int m_A;\n"), "'ClampMin' expects a number"),
            (wrap("    ZPROPERTY()\n    int m_A, m_B;\n"), "exactly one member"),
            (wrap("    ZPROPERTY()\n    int m_A = 1, m_B = 2;\n"), "exactly one member"),
            (wrap("    ZPROPERTY()\n    static int s_A;\n"), "static members"),
            (wrap("    ZPROPERTY()\n    int m_A[4];\n"), "C arrays"),
            (wrap("    ZPROPERTY()\n    int m_A : 3;\n"), "bit-fields"),
            (wrap("    ZPROPERTY()\n    int& m_A;\n"), "reference members"),
            (wrap("    ZPROPERTY()\n    void Foo();\n"), "ZFUNCTION"),
            (wrap("    ZPROPERTY()\n    int m_A;\n    ZPROPERTY(Name=\"a\")\n    int m_B;\n"), "collides"),
            (wrap("    ZPROPERTY(Transient, Name=\"x\")\n    int m_A;\n"), "Transient"),
            (wrap("    struct In { ZPROPERTY() int x; };\n"), "only allowed directly inside"),
            (wrap("", spec="Whatever"), "unknown ZCLASS specifier 'Whatever'"),
            (wrap("", base=""), "must derive from"),
            ('#include "Foo.generated.h"\nZCLASS()\nclass Foo : public Component { };\n', "missing ZGENERATED_BODY"),
            ('#include "Foo.generated.h"\nZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Bar) };\n', "does not match"),
            ("ZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) };\n", "is required"),
            ('#include "Foo.generated.h"\n#include <vector>\nZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) };\n', "must be the last"),
            ('#include "Foo.generated.h"\nnamespace A { namespace B {\nZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) };\n} }\n', "exactly one named namespace"),
            ('#include "Foo.generated.h"\nnamespace A::B {\nZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) };\n}\n', "exactly one named namespace"),
            ('#include "Foo.generated.h"\nnamespace {\nZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) };\n}\n', "exactly one named namespace"),
            ('#include "Foo.generated.h"\nstruct Outer {\nZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) };\n};\n', "nested or local"),
            (wrap("", spec="CustomTransfer, NoSuperTransfer"), "no effect"),
            (wrap("    ZPROPERTY(meta=(ClampMin=0))\n    eastl::string m_S;\n"), "only applies to numeric"),
            (wrap("    ZPROPERTY(meta=(ClampMin=5, ClampMax=1))\n    float m_V;\n"), "greater than"),
            (wrap("    ZPROPERTY(meta=(UIMin=2, UIMax=1))\n    float m_V;\n"), "greater than"),
            (wrap("    ZPROPERTY(Transient, meta=(Tooltip=\"x\"))\n    float m_V;\n"), "Transient property has no effect"),
            (wrap("    ZPROPERTY(meta=(EditCondition=\"m_Missing\"))\n    float m_V;\n"), "must name a serialized ZPROPERTY"),
            (wrap("    ZPROPERTY()\n    int m_Mode;\n    ZPROPERTY(meta=(EditCondition=\"m_Mode\"))\n    float m_V;\n"), "must name a bool"),
            (wrap("    ZPROPERTY(meta=(EditCondition=\"m_V\"))\n    bool m_V;\n"), "refers to itself"),
            ('#include "Foo.generated.h"\nZCLASS()\nclass Foo;\n', "forward declaration"),
            ('#include "Foo.generated.h"\nZCLASS()\nclass Foo : public Component { ZGENERATED_BODY(Foo) }\n', "expected ';'"),
            ('ZPROPERTY()\nint g;\n', "outside of a ZCLASS"),
        ]
        for text, fragment in cases:
            with self.subTest(fragment=fragment):
                self._assert_error(text, fragment)

    def test_error_line_numbers(self):
        exc = self._assert_error(wrap("    int ok;\n    ZPROPERTY(Bogus)\n    int m_A;\n"), "Bogus")
        self.assertEqual(exc.line, 7)

    def test_has_annotations(self):
        self.assertTrue(has_annotations(LIGHT_H))
        self.assertFalse(has_annotations("// ZCLASS()\n#define ZCLASS(...)\nconst char* s = \"ZCLASS(\";\n"))
        self.assertFalse(has_annotations("class A {};\n"))


class CodegenTests(unittest.TestCase):
    def test_light_component_output(self):
        info = parse_header("/src/LightComponent.h", "Runtime/Function/Framework/Component/Light/LightComponent.h", LIGHT_H)
        header = generate_header(info)
        self.assertIn("#define ZGEN_BODY_LightComponent \\\n"
                      "    REGISTER_CLASS(LightComponent); \\\n"
                      "    DECLARE_OBJECT_SERIALIZE(); \\\n"
                      "private:\n", header)
        source = generate_source(info)
        self.assertIn('#include "Runtime/Function/Framework/Component/Light/LightComponent.h"\n', source)
        self.assertIn('#include "Runtime/Core/Serialize/TransferFunctions/SerializeTransfer.h"\n', source)
        expected = (
            "// ---- LightComponent ----\n"
            "IMPLEMENT_REGISTER_CLASS(LightComponent)\n"
            "IMPLEMENT_OBJECT_SERIALIZE(LightComponent)\n"
            "\n"
            "template<typename TransferFunction>\n"
            "void LightComponent::Transfer(TransferFunction& transfer)\n"
            "{\n"
            "    Super::Transfer(transfer);\n"
            '    transfer.Transfer(m_Flux, "flux");\n'
            '    transfer.Transfer(m_Radius, "radius");\n'
            "}\n"
            "\n"
            "INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(LightComponent)\n"
        )
        self.assertTrue(source.endswith(expected), source)

    def test_flags_transient_abstract_custom_transfer(self):
        info = parse(wrap(
            "    ZPROPERTY(HideInEditor, StrongPPtr)\n    int m_A;\n"
            "    ZPROPERTY(Transient)\n    int m_B;\n",
            spec="Abstract",
        ))
        header, source = generate_header(info), generate_source(info)
        self.assertIn("REGISTER_CLASS_TRAITS(kTypeIsAbstract); \\\n    REGISTER_CLASS(Foo);", header)
        self.assertIn('transfer.Transfer(m_A, "a", TransferMetaFlags::HideInEditorMask | TransferMetaFlags::StrongPPtrMask);', source)
        self.assertNotIn("m_B", source)

        custom = generate_source(parse(wrap("", spec="CustomTransfer")))
        self.assertIn("IMPLEMENT_REGISTER_CLASS(Foo)", custom)
        self.assertNotIn("IMPLEMENT_OBJECT_SERIALIZE", custom.split("// ZCLASS(CustomTransfer)")[0])
        self.assertNotIn("void Foo::Transfer", custom)
        self.assertNotIn("INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Foo)", custom)

    def test_namespaced_class(self):
        text = (
            '#include "Foo.generated.h"\n'
            "namespace Runtime\n{\n"
            "    ZCLASS()\n    class Foo : public Component\n    {\n        ZGENERATED_BODY(Foo)\n"
            "        ZPROPERTY()\n        float m_Volume {1.0f};\n    };\n"
            "}  // namespace Runtime\n"
        )
        info = parse(text)
        cls = info.classes[0]
        self.assertEqual((cls.namespace, cls.qualified_name), ("Runtime", "Runtime::Foo"))
        source = generate_source(info)
        self.assertIn("IMPLEMENT_REGISTER_CLASS_3(Runtime, Foo, typeid(Runtime::Foo).hash_code())\n", source)
        self.assertIn("IMPLEMENT_OBJECT_SERIALIZE(Runtime::Foo)\n", source)
        self.assertIn("namespace Runtime\n{\n    template<typename TransferFunction>\n    void Foo::Transfer(", source)
        self.assertIn("    INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Foo)\n}  // namespace Runtime\n", source)

    def test_no_super_transfer_and_empty_body(self):
        source = generate_source(parse(wrap("    ZPROPERTY()\n    int m_A;\n", spec="NoSuperTransfer")))
        self.assertNotIn("Super::Transfer", source)
        self.assertIn('    transfer.Transfer(m_A, "a");\n}', source)
        empty = generate_source(parse(wrap("", spec="NoSuperTransfer")))
        self.assertIn("{\n    (void)transfer;\n}", empty)

    def test_property_meta_table(self):
        text = wrap(
            "    ZPROPERTY()\n    bool m_UseRange {true};\n"
            '    ZPROPERTY(meta=(DisplayName="Range (m)", Tooltip="Light \\"reach\\"", Category="Shape", '
            'ClampMin=0, ClampMax=1000.5, UIMax=100, EditCondition="!m_UseRange"))\n'
            "    float m_Radius {8.0f};\n"
            "    ZPROPERTY()\n    float m_Plain;\n"
        )
        info = parse(text)
        radius = info.classes[0].properties[1]
        self.assertEqual(radius.edit_condition, "!use_range")
        source = generate_source(info)
        self.assertIn('#include "Runtime/BaseClasses/PropertyMeta.h"', source)
        self.assertIn(
            '{.serializedName = "radius", .displayName = "Range (m)", .tooltip = "Light \\"reach\\"", .category = "Shape", '
            '.editCondition = "!use_range", .hasClampMin = true, .clampMin = 0.0, .hasClampMax = true, .clampMax = 1000.5, '
            '.hasUIMax = true, .uiMax = 100.0},', source)
        self.assertIn("ZClassPropertyMeta kZhtClassMeta_Foo {TypeOf<Foo>(), kZhtPropertyMeta_Foo, 1};", source)
        self.assertIn("const ZPropertyMetaRegistrar kZhtMetaRegistrar_Foo(kZhtClassMeta_Foo);", source)
        self.assertNotIn('.serializedName = "plain"', source)

    def test_script_binding_table(self):
        text = wrap(
            "    ZFUNCTION()\n    const eastl::string& GetName() const;\n"
            "    ZFUNCTION()\n    void SetName(eastl::string name);\n"
            '    ZFUNCTION(Name="id")\n    GObjectID GetID() const;\n'
            "    ZFUNCTION()\n    Foo* GetChild(int index, bool recursive = false) const;\n"
            "    ZFUNCTION()\n    static Foo* GetStatic(int a, float b);\n"
        )
        info = parse(text)
        cls = info.classes[0]
        self.assertEqual([m.ts_name for m in cls.methods], ["GetName", "SetName", "id", "GetChild", "GetStatic"])
        self.assertEqual(cls.methods[0].ts_signature, "GetName(): string")
        self.assertEqual(cls.methods[1].ts_signature, "SetName(name: string): void")
        self.assertEqual(cls.methods[2].ts_signature, "id(): number")
        self.assertEqual(cls.methods[3].ts_signature, "GetChild(index: number, recursive: boolean): Foo")
        self.assertEqual(cls.methods[4].ts_signature, "GetStatic(a: number, b: number): Foo")
        self.assertTrue(cls.methods[0].is_const)
        self.assertFalse(cls.methods[1].is_const)
        self.assertTrue(cls.methods[4].is_static)

        source = generate_source(info)
        self.assertIn('#include "Runtime/Scripting/Binding/ScriptBinding.h"', source)
        # Overloads are disambiguated by casting to the exact function-pointer type.
        self.assertIn(
            '&ZScript::MethodCallback<static_cast<const eastl::string& (Foo::*)() const>(&Foo::GetName)>::Invoke', source)
        self.assertIn('&ZScript::MethodCallback<static_cast<void (Foo::*)(eastl::string)>(&Foo::SetName)>::Invoke', source)
        self.assertIn('&ZScript::MethodCallback<static_cast<Foo* (*)(int, float)>(&Foo::GetStatic)>::Invoke', source)
        self.assertIn('{.name = "id", .tsSignature = "id(): number", .isStatic = false', source)
        self.assertIn(
            'ZClassScriptBinding kZhtScriptClass_Foo {ZScriptTypeId<Foo>::Get(), ZScriptTypeId<Component>::Get(), '
            '"Foo", "Component", kZhtScriptMethods_Foo, 5};', source)
        self.assertIn("const ZClassScriptBindingRegistrar kZhtScriptRegistrar_Foo(kZhtScriptClass_Foo);", source)

    def test_no_zfunction_no_binding_table(self):
        source = generate_source(parse(wrap("    ZPROPERTY()\n    float m_V;\n")))
        self.assertNotIn("ScriptBinding", source)

    def test_method_with_inline_body_is_bound(self):
        # A definition instead of a declaration is fine; the body is skipped.
        info = parse(wrap("    ZFUNCTION()\n    float GetV() const { return m_V; }\n"
                          "    ZPROPERTY()\n    float m_V;\n"))
        self.assertEqual(info.classes[0].methods[0].ts_signature, "GetV(): number")

    def test_scalar_typedef_maps_to_number_not_the_alias(self):
        # A bare identifier is emitted verbatim, which is right for class names
        # (Vector3) but wrong for a typedef of an integer: it would leak an
        # undeclared name into the .d.ts. Such aliases belong in _TS_TYPES.
        info = parse(wrap("    ZFUNCTION()\n    AudioVoiceId Play();\n"
                          "    ZFUNCTION()\n    GObjectID GetID() const;\n"))
        self.assertEqual(info.classes[0].methods[0].ts_signature, "Play(): number")
        self.assertEqual(info.classes[0].methods[1].ts_signature, "GetID(): number")

    def test_no_meta_no_table(self):
        source = generate_source(parse(wrap("    ZPROPERTY()\n    float m_V;\n")))
        self.assertNotIn("PropertyMeta", source)

    def test_namespaced_meta_uses_qualified_type(self):
        text = ('#include "Foo.generated.h"\nnamespace Runtime\n{\n    ZCLASS()\n    class Foo : public Component\n    {\n'
                '        ZGENERATED_BODY(Foo)\n        ZPROPERTY(meta=(ClampMin=0))\n        float m_V;\n    };\n}\n')
        source = generate_source(parse(text))
        self.assertIn("kZhtClassMeta_Runtime_Foo {TypeOf<Runtime::Foo>()", source)
        self.assertGreater(source.index("kZhtClassMeta_Runtime_Foo"), source.index("}  // namespace Runtime"))

    def test_struct_restores_public(self):
        self.assertTrue(generate_header(parse(wrap("", key="struct"))).rstrip().endswith("public:"))

    def test_bound_signature_pulls_in_the_types_header(self):
        # ZScript::Converter<T*> static_asserts on std::is_base_of<Object, T>,
        # which is a hard error on an incomplete type. A reflected header that
        # only forward-declares the return type (Component.h / GameObject.h /
        # Transform.h form such a cycle) must still compile.
        info = parse(wrap("    ZFUNCTION()\n    Bar* GetBar() const;\n"
                          "    ZFUNCTION()\n    void Take(Bar* bar, const Baz& baz);\n"))
        source = generate_source(info, {"Bar": "Runtime/Bar.h", "Baz": "Runtime/Baz.h"}, set(), {})
        self.assertIn('#include "Runtime/Bar.h"', source)
        self.assertIn('#include "Runtime/Baz.h"', source)
        # The class's own header is already included at the top and must not be
        # emitted again; unknown names are left alone (std:: / eastl:: types
        # ship their own includes through ScriptBinding.h).
        self.assertEqual(source.count('#include "Runtime/Foo.h"'), 1)
        self.assertNotIn('#include "Runtime/std::string.h"', source)

        plain = generate_source(info)
        self.assertNotIn("Runtime/Bar.h", plain)

    def test_super_skips_methodless_bases(self):
        # Foo -> Middle -> Base, where only Base and Foo own ZFUNCTIONs.
        # Emitting "extends Middle" would make pesapi declare Middle as a root
        # placeholder and drop every Base method from the prototype chain.
        info = parse(wrap("    ZFUNCTION()\n    float GetV() const;\n", base=" : public Middle"))
        source = generate_source(info, {}, {"Base": "Object", "Foo": "Middle"},
                                 {"Middle": "Base", "Base": "Object", "Foo": "Middle"})
        self.assertIn('ZScriptTypeId<Base>::Get(), "Foo", "Base"', source)

        # An ancestor this module knows nothing about is kept as-is, so
        # InstallAll still declares the placeholder.
        source = generate_source(info, {}, {"Foo": "Middle"}, {"Middle": "Base", "Foo": "Middle"})
        self.assertIn('ZScriptTypeId<Base>::Get(), "Foo", "Base"', source)

        # A methodless root (Object) is likewise kept.
        info2 = parse(wrap("    ZFUNCTION()\n    float GetV() const;\n", base=" : public Object"))
        source = generate_source(info2, {}, {"Foo": "Object"}, {"Foo": "Object"})
        self.assertIn('ZScriptTypeId<Object>::Get(), "Foo", "Object"', source)


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        self.src = os.path.join(root, "Source")
        self.gen = os.path.join(root, "gen")
        os.makedirs(os.path.join(self.src, "Runtime", "Light"))
        self.light = os.path.join(self.src, "Runtime", "Light", "LightComponent.h")
        self.plain = os.path.join(self.src, "Runtime", "Plain.h")
        with open(self.light, "w") as f:
            f.write(LIGHT_H)
        with open(self.plain, "w") as f:
            f.write("#pragma once\n// mentions ZCLASS( only in a comment\nclass Plain {};\n")
        self.headers = os.path.join(root, "headers.txt")
        with open(self.headers, "w") as f:
            f.write(self.light + "\n" + self.plain + "\n")
        self.manifest = os.path.join(self.gen, "annotated.txt")
        self.stamp = os.path.join(self.gen, "zht.stamp")

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *argv):
        err = io.StringIO()
        with redirect_stderr(err):
            rc = cli.main(list(argv))
        return rc, err.getvalue()

    def generate(self):
        return self.run_cli(
            "generate", "--headers-file", self.headers, "--include-root", self.src,
            "--out-inc", os.path.join(self.gen, "Inc"), "--out-src", os.path.join(self.gen, "Src"),
            "--manifest", self.manifest, "--stamp", self.stamp, "--module", "Test",
            "--dump-ir", os.path.join(self.gen, "ir.json"),
        )

    def test_list_then_generate_is_incremental_and_prunes(self):
        rc, _ = self.run_cli("list", "--headers-file", self.headers, "--output", self.manifest)
        self.assertEqual(rc, 0)
        with open(self.manifest) as f:
            self.assertEqual(f.read().strip(), self.light.replace("\\", "/"))

        rc, err = self.generate()
        self.assertEqual(rc, 0, err)
        gen_h = os.path.join(self.gen, "Inc", "LightComponent.generated.h")
        gen_cpp = os.path.join(self.gen, "Src", "LightComponent.gen.cpp")
        with open(gen_cpp) as f:
            self.assertIn('#include "Runtime/Light/LightComponent.h"', f.read())
        self.assertTrue(os.path.exists(self.stamp))

        mtime = os.stat(gen_h).st_mtime_ns
        time.sleep(0.02)
        rc, err = self.generate()
        self.assertEqual(rc, 0, err)
        self.assertEqual(os.stat(gen_h).st_mtime_ns, mtime, "unchanged output must not be rewritten (restat)")

        stale = os.path.join(self.gen, "Src", "Gone.gen.cpp")
        with open(stale, "w") as f:
            f.write("//")
        rc, err = self.generate()
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(stale))

    def test_annotation_set_change_rewrites_manifest_and_fails(self):
        self.run_cli("list", "--headers-file", self.headers, "--output", self.manifest)
        with open(self.plain, "w") as f:
            f.write('#include "Plain.generated.h"\nZCLASS()\nclass Plain : public Object { ZGENERATED_BODY(Plain) };\n')
        rc, err = self.generate()
        self.assertEqual(rc, 2)
        self.assertIn("re-configure", err)
        with open(self.manifest) as f:
            self.assertEqual(len(f.read().split()), 2)
        rc, err = self.generate()
        self.assertEqual(rc, 0, err)

    def test_parse_error_is_reported_with_location_and_no_stamp(self):
        self.run_cli("list", "--headers-file", self.headers, "--output", self.manifest)
        with open(self.light, "w") as f:
            f.write(LIGHT_H.replace("ZPROPERTY()\n    float", "ZPROPERTY(Oops)\n    float"))
        rc, err = self.generate()
        self.assertEqual(rc, 1)
        self.assertIn("LightComponent.h", err)
        self.assertIn("unknown ZPROPERTY specifier 'Oops'", err)
        self.assertRegex(err, r"LightComponent\.h[:(]19")
        self.assertFalse(os.path.exists(self.stamp))

    def test_base_name_collision(self):
        other_dir = os.path.join(self.src, "Runtime", "Other")
        os.makedirs(other_dir)
        dup = os.path.join(other_dir, "LightComponent.h")
        with open(dup, "w") as f:
            f.write(LIGHT_H.replace("class LightComponent", "class LightComponent2").replace("ZGENERATED_BODY(LightComponent)", "ZGENERATED_BODY(LightComponent2)"))
        with open(self.headers, "a") as f:
            f.write(dup + "\n")
        self.run_cli("list", "--headers-file", self.headers, "--output", self.manifest)
        rc, err = self.generate()
        self.assertEqual(rc, 1)
        self.assertIn("collides", err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
