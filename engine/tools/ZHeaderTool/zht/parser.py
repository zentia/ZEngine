"""Parser for ZCLASS / ZPROPERTY / ZGENERATED_BODY annotated headers.

This is NOT a C++ parser. Like UE's UnrealHeaderTool it only understands
the small, regular subset of C++ that annotated declarations live in:

    ZCLASS(<specifiers>)
    class <Name> [final] : public <Base> [, ...]
    {
        ZGENERATED_BODY(<Name>)
    <access>:
        ZPROPERTY(<specifiers>)
        <Type> <member> [{init} | = init];

        ZFUNCTION(<specifiers>)
        <Ret> <Name>([<Params>]) [const];
        ...
    };

Everything else in the header is skipped by brace matching. Anything the
tool cannot represent faithfully is a hard error with file/line, never a
silent guess.
"""

from __future__ import annotations

import re
from typing import Dict, List, Optional, Sequence, Tuple

from .lexer import CHAR, IDENT, NUMBER, PUNCT, STRING, LexError, Token, tokenize
from .model import (ClassInfo, HeaderInfo, MethodInfo, ParamInfo, PropertyInfo,
                    ScriptPropertyInfo, SpecValue)


class ParseError(Exception):
    def __init__(self, line: int, message: str):
        super().__init__(message)
        self.line = line
        self.message = message


# ---------------------------------------------------------------------------
# Specifier vocabulary
# ---------------------------------------------------------------------------

CLASS_FLAGS = {
    # Emits REGISTER_CLASS_TRAITS(kTypeIsAbstract): the type gets no factory.
    "Abstract",
    # Registration is generated, but Transfer() stays hand-written in the
    # class's own .cpp (IMPLEMENT_OBJECT_SERIALIZE + INSTANTIATE included).
    "CustomTransfer",
    # The generated Transfer() does not chain to Super::Transfer(). Only for
    # classes whose hand-written Transfer() deliberately skipped the base.
    "NoSuperTransfer",
}

# ZPROPERTY flag -> TransferMetaFlags enumerator.
PROPERTY_TRANSFER_FLAGS = {
    "HideInEditor": "HideInEditorMask",
    "ReadOnly": "DisallowSerializedPropertyModification",
    "StrongPPtr": "StrongPPtrMask",
    "Deprecated": "Deprecated",
    "TreatIntegerAsBool": "kTreatIntegerValueAsBoolean",
    "Hex64": "kTransferHex64",
}

PROPERTY_PLAIN_FLAGS = {
    # Reflected (phase 2+) but not serialized.
    "Transient",
}

PROPERTY_VALUE_KEYS = {
    # Overrides the serialized field name (default: snake_case of the member
    # name without its m_ prefix).
    "Name": "string",
}

# ZFUNCTION specifiers (phase 3: script bindings).
FUNCTION_VALUE_KEYS = {
    # Overrides the name exposed to TypeScript (default: the C++ name).
    "Name": "string",
}

# C++ -> TypeScript type mapping for ZFUNCTION signatures. Kept here (not in
# codegen) because an unmappable type must be a generate-time error with a line
# number, not a silent `any` in the emitted .d.ts.
_TS_TYPES = {
    "void": "void",
    "bool": "boolean",
    "char": "number",
    "signed char": "number",
    "unsigned char": "number",
    "short": "number",
    "unsigned short": "number",
    "int": "number",
    "unsigned": "number",
    "unsigned int": "number",
    "long": "number",
    "long long": "number",
    "float": "number",
    "double": "number",
    "int8_t": "number",
    "int16_t": "number",
    "int32_t": "number",
    "int64_t": "number",
    "uint8_t": "number",
    "uint16_t": "number",
    "uint32_t": "number",
    "uint64_t": "number",
    "size_t": "number",
    "std::size_t": "number",
    # Engine scalar typedefs. A bare identifier that is not listed here is
    # emitted verbatim, which is only correct for class / struct names (a
    # `Vector3` or a `Transform*`); a typedef of an integer leaks the C++ alias
    # into the .d.ts as an undeclared type name, so it has to be listed.
    "GObjectID": "number",
    "AudioVoiceId": "number",
    "std::string": "string",
    "eastl::string": "string",
}

# ZPROPERTY field types that may cross into scripts (phase 7). Deliberately a
# separate, narrower table than _TS_TYPES: a ZPROPERTY field is annotated to be
# *serialized*, not to be script-facing, so a type we cannot marshal is skipped
# silently instead of being a generate-time error. That is the only way to keep
# `enum class` / PPtr / std::vector fields (115 of the 162 reflected fields are
# marshallable, the rest are not) from breaking every class that has one.
#
# Values must have a ZScript::Converter. Note the absence of `T*`: a pointer
# field would need the pointee's header at generate time and an Object-derived
# type check the parser cannot make, so pointer properties are not supported.
_SCRIPT_PROPERTY_TYPES = {
    "bool": "boolean",
    "char": "number",
    "signed char": "number",
    "unsigned char": "number",
    "short": "number",
    "unsigned short": "number",
    "int": "number",
    "unsigned": "number",
    "unsigned int": "number",
    "long": "number",
    "long long": "number",
    "float": "number",
    "double": "number",
    "int8_t": "number",
    "int16_t": "number",
    "int32_t": "number",
    "int64_t": "number",
    "uint8_t": "number",
    "uint16_t": "number",
    "uint32_t": "number",
    "uint64_t": "number",
    "size_t": "number",
    "std::size_t": "number",
    "GObjectID": "number",
    "AudioVoiceId": "number",
    "std::string": "string",
    "eastl::string": "string",
    "Vector3": "Vector3",
    "Quaternion": "Quaternion",
}


def script_property_ts_type(cpp_type: str) -> Optional[str]:
    """TypeScript type of a ZPROPERTY field, or None when it cannot be
    marshalled (that field then simply has no script property)."""
    text = " ".join(cpp_type.split())
    return _SCRIPT_PROPERTY_TYPES.get(text)


def script_property_name(serialized_name: str) -> str:
    """Serialized field name -> script property name.

    Unity's own split: YAML and the Inspector keep the C++-flavoured
    `m_script_guid` / `clip_path`, while script code sees `scriptGuid` /
    `clipPath`. Dropping a leading `m_` first is what stops
    `ZPROPERTY(Name="m_script_guid")` from surfacing as `mScriptGuid`.
    """
    text = serialized_name[2:] if serialized_name.startswith("m_") else serialized_name
    parts = [p for p in text.split("_") if p]
    if not parts:
        return serialized_name
    return parts[0] + "".join(p.capitalize() for p in parts[1:])


# ZSCRIPTPROPERTY specifiers.
SCRIPT_PROPERTY_KEYS = {
    "Name": "string",
    "Getter": "string",
    "Setter": "string",
}

# Declaration specifiers that may precede a ZFUNCTION return type.
_FUNCTION_LEADING = {"static", "virtual", "inline", "constexpr", "explicit", "FORCEINLINE"}

# Trailing tokens after the parameter list. Only `const` changes the binding;
# the rest are accepted and ignored.
_FUNCTION_TRAILING = {"const", "noexcept", "override", "final"}

# meta=(...) keys, validated now, consumed from phase 2 on.
META_KEYS = {
    "Tooltip": "string",
    "DisplayName": "string",
    "Category": "string",
    "EditCondition": "string",
    "ClampMin": "number",
    "ClampMax": "number",
    "UIMin": "number",
    "UIMax": "number",
}

# C++ scalar types ClampMin/ClampMax/UIMin/UIMax may annotate.
_NUMERIC_TYPES = {
    "float", "double", "int", "unsigned", "unsigned int", "short", "long", "long long",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "unsigned char", "signed char", "char", "size_t",
}

_CPP_KEYWORDS_NOT_NAMES = {
    "const", "volatile", "unsigned", "signed", "int", "char", "float", "double",
    "bool", "long", "short", "void", "auto", "struct", "class", "enum", "typename",
}

_DECL_FORBIDDEN_LEADING = {
    "static": "static members are not serialized per-instance",
    "constexpr": "constexpr members cannot be deserialized",
    "virtual": "ZPROPERTY must annotate a data member, not a function",
    "friend": "ZPROPERTY must annotate a data member",
    "using": "ZPROPERTY must annotate a data member",
    "typedef": "ZPROPERTY must annotate a data member",
    "template": "ZPROPERTY must annotate a data member",
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def default_serialized_name(member_name: str) -> str:
    """m_SourceRelPath -> source_rel_path, m_UVScale -> uv_scale, radius -> radius."""
    name = member_name[2:] if member_name.startswith("m_") and len(member_name) > 2 else member_name
    name = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    name = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    return name.lower()


def format_type(tokens: Sequence[Token]) -> str:
    """Canonical spelling for diagnostics / IR: `std::map<int, float>`, `const char* const`."""
    out: List[str] = []
    prev: Optional[Token] = None
    for tok in tokens:
        is_word = tok.kind in (IDENT, NUMBER, STRING, CHAR)
        if prev is not None:
            prev_word = prev.kind in (IDENT, NUMBER, STRING, CHAR)
            if prev.is_punct(","):
                out.append(" ")
            elif is_word and (prev_word or prev.text in ("*", "&", ">")):
                out.append(" ")
        out.append(tok.text)
        prev = tok
    return "".join(out)


class _Cursor:
    def __init__(self, tokens: List[Token]):
        self.tokens = tokens
        self.pos = 0

    def peek(self, offset: int = 0) -> Optional[Token]:
        idx = self.pos + offset
        return self.tokens[idx] if 0 <= idx < len(self.tokens) else None

    def next(self) -> Token:
        tok = self.peek()
        if tok is None:
            last_line = self.tokens[-1].line if self.tokens else 1
            raise ParseError(last_line, "unexpected end of file")
        self.pos += 1
        return tok

    def expect_punct(self, text: str, context: str) -> Token:
        tok = self.next()
        if not tok.is_punct(text):
            raise ParseError(tok.line, "expected '%s' %s, got '%s'" % (text, context, tok.text))
        return tok

    def at_end(self) -> bool:
        return self.pos >= len(self.tokens)


def _collect_balanced(cur: _Cursor, open_tok: Token) -> List[Token]:
    """Cursor sits just past `open_tok` ('(' / '{' / '['); returns inner tokens."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    closers = {")": "(", "}": "{", "]": "["}
    stack = [open_tok.text]
    inner: List[Token] = []
    while True:
        if cur.at_end():
            raise ParseError(open_tok.line, "unbalanced '%s'" % open_tok.text)
        tok = cur.next()
        if tok.kind == PUNCT and tok.text in pairs:
            stack.append(tok.text)
        elif tok.kind == PUNCT and tok.text in closers:
            if stack[-1] != closers[tok.text]:
                raise ParseError(tok.line, "mismatched '%s'" % tok.text)
            stack.pop()
            if not stack:
                return inner
        inner.append(tok)


# ---------------------------------------------------------------------------
# Specifier lists:  Flag, Key=Value, meta=(Key=Value, ...)
# ---------------------------------------------------------------------------

Spec = Tuple[str, Optional[SpecValue], int]


def _parse_spec_value(tokens: List[Token], i: int, line: int) -> Tuple[SpecValue, int]:
    if i >= len(tokens):
        raise ParseError(line, "missing value after '='")
    tok = tokens[i]
    if tok.kind == STRING:
        return tok.text, i + 1
    negative = False
    if tok.is_punct("-"):
        negative = True
        i += 1
        if i >= len(tokens) or tokens[i].kind != NUMBER:
            raise ParseError(tok.line, "expected a number after '-'")
        tok = tokens[i]
    if tok.kind == NUMBER:
        text = tok.text.replace("'", "").rstrip("fFlLuU")
        try:
            value: SpecValue = float(text) if any(ch in text for ch in ".eE") and not text.lower().startswith("0x") else int(text, 0)
        except ValueError:
            raise ParseError(tok.line, "invalid number '%s'" % tok.text)
        return (-value if negative else value), i + 1
    if tok.kind == IDENT:
        if tok.text in ("true", "false"):
            return tok.text == "true", i + 1
        return tok.text, i + 1
    raise ParseError(tok.line, "unsupported specifier value '%s'" % tok.text)


def parse_specifiers(tokens: List[Token], line: int) -> Tuple[List[Spec], Dict[str, Tuple[SpecValue, int]]]:
    specs: List[Spec] = []
    meta: Dict[str, Tuple[SpecValue, int]] = {}
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if not tok.is_ident():
            raise ParseError(tok.line, "expected specifier name, got '%s'" % tok.text)
        key = tok.text
        i += 1
        if i < len(tokens) and tokens[i].is_punct("="):
            i += 1
            if key == "meta":
                if i >= len(tokens) or not tokens[i].is_punct("("):
                    raise ParseError(tok.line, "meta must be written as meta=(Key=Value, ...)")
                depth = 0
                start = i + 1
                j = i
                while j < len(tokens):
                    if tokens[j].is_punct("("):
                        depth += 1
                    elif tokens[j].is_punct(")"):
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                if j >= len(tokens):
                    raise ParseError(tok.line, "unbalanced meta=(...)")
                inner_specs, inner_meta = parse_specifiers(tokens[start:j], tok.line)
                if inner_meta:
                    raise ParseError(tok.line, "nested meta=(...) is not allowed")
                for mkey, mvalue, mline in inner_specs:
                    if mkey in meta:
                        raise ParseError(mline, "duplicate meta key '%s'" % mkey)
                    meta[mkey] = (True if mvalue is None else mvalue, mline)
                i = j + 1
            else:
                value, i = _parse_spec_value(tokens, i, tok.line)
                specs.append((key, value, tok.line))
        else:
            if key == "meta":
                raise ParseError(tok.line, "meta must be written as meta=(Key=Value, ...)")
            specs.append((key, None, tok.line))
        if i < len(tokens):
            if not tokens[i].is_punct(","):
                raise ParseError(tokens[i].line, "expected ',' between specifiers, got '%s'" % tokens[i].text)
            i += 1
            if i >= len(tokens):
                raise ParseError(tokens[i - 1].line, "trailing ',' in specifier list")
    seen = set()
    for key, _value, sline in specs:
        if key in seen:
            raise ParseError(sline, "duplicate specifier '%s'" % key)
        seen.add(key)
    return specs, meta


def _check_value_kind(kind: str, key: str, value: Optional[SpecValue], line: int) -> None:
    if value is None:
        raise ParseError(line, "'%s' requires a value (%s=...)" % (key, key))
    if kind == "string" and not isinstance(value, str):
        raise ParseError(line, "'%s' expects a string literal" % key)
    if kind == "number" and (isinstance(value, bool) or not isinstance(value, (int, float))):
        raise ParseError(line, "'%s' expects a number" % key)


def _unknown(key: str, line: int, what: str, known: Sequence[str]) -> ParseError:
    return ParseError(line, "unknown %s specifier '%s' (known: %s)" % (what, key, ", ".join(sorted(known))))


# ---------------------------------------------------------------------------
# Header parsing
# ---------------------------------------------------------------------------


def has_annotations(text: str) -> bool:
    """Cheap-but-correct detection used by `list`: a real ZCLASS( token outside
    comments / strings / preprocessor lines."""
    if "ZCLASS" not in text:
        return False
    try:
        tokens = tokenize(text).tokens
    except LexError:
        # Let `generate` report the lexing problem with a proper location.
        return True
    for idx, tok in enumerate(tokens):
        if tok.is_ident("ZCLASS") and idx + 1 < len(tokens) and tokens[idx + 1].is_punct("("):
            return True
    return False


def parse_header(path: str, include_path: str, text: str) -> HeaderInfo:
    try:
        lexed = tokenize(text)
    except LexError as exc:
        raise ParseError(exc.line, exc.message)

    header = HeaderInfo(path=path, include_path=include_path)
    cur = _Cursor(lexed.tokens)
    # ("namespace", "<name>") | ("block", "")
    scope: List[Tuple[str, str]] = []

    while not cur.at_end():
        tok = cur.next()
        if tok.is_ident("ZCLASS"):
            namespace = ""
            if scope:
                if any(kind != "namespace" for kind, _ in scope):
                    raise ParseError(tok.line, "ZCLASS is not supported on nested or local classes")
                if len(scope) > 1 or not scope[0][1] or "::" in scope[0][1]:
                    raise ParseError(tok.line, "ZCLASS supports global scope or exactly one named namespace (IMPLEMENT_REGISTER_CLASS_3 token-pastes the namespace)")
                namespace = scope[0][1]
            info = _parse_class(cur, tok)
            info.namespace = namespace
            header.classes.append(info)
            continue
        if tok.is_ident("ZPROPERTY") or tok.is_ident("ZGENERATED_BODY") or tok.is_ident("ZFUNCTION"):
            raise ParseError(tok.line, "%s used outside of a ZCLASS-annotated class" % tok.text)
        if tok.is_ident("namespace"):
            # namespace A {  /  namespace A::B {  /  namespace {  /  namespace X = Y;
            name_parts: List[str] = []
            while not cur.at_end():
                nxt = cur.peek()
                if nxt.is_punct("{"):
                    cur.next()
                    scope.append(("namespace", "".join(name_parts)))
                    break
                if nxt.is_punct(";") or nxt.is_punct("="):
                    break
                if nxt.kind == IDENT and nxt.text != "inline" or nxt.is_punct("::"):
                    name_parts.append(nxt.text)
                cur.next()
            continue
        if tok.is_punct("{"):
            scope.append(("block", ""))
        elif tok.is_punct("}"):
            if not scope:
                raise ParseError(tok.line, "unbalanced '}'")
            scope.pop()

    if header.classes:
        _validate_generated_include(header, lexed.includes)
    return header


def _validate_generated_include(header: HeaderInfo, includes) -> None:
    expected = header.generated_header_name
    matches = [inc for inc in includes if inc.path == expected or inc.path.endswith("/" + expected)]
    first_line = header.classes[0].line
    if not matches:
        raise ParseError(first_line, '#include "%s" is required in headers that use ZCLASS (it must be the last #include)' % expected)
    if includes[-1] is not matches[-1]:
        raise ParseError(matches[-1].line, '#include "%s" must be the last #include in the file' % expected)
    if matches[-1].line > first_line:
        raise ParseError(matches[-1].line, '#include "%s" must come before the first ZCLASS' % expected)


def _validate_script_properties(info: ClassInfo) -> None:
    """Resolve ZSCRIPTPROPERTY accessor names against the class's ZFUNCTIONs.

    Runs after the whole body is parsed, so the accessor may be declared
    anywhere in the class. A ZSCRIPTPROPERTY name must also not collide with a
    script-exposed ZPROPERTY field: both end up as a pesapi property of the same
    class, and the later registration would silently shadow the earlier one.
    """
    by_name = {m.name: m for m in info.methods}
    for prop in info.script_properties:
        getter = by_name.get(prop.getter)
        if getter is None:
            raise ParseError(prop.line, "ZSCRIPTPROPERTY '%s': '%s' is not a ZFUNCTION-annotated member of '%s'"
                             % (prop.name, prop.getter, info.name))
        if getter.is_static:
            raise ParseError(prop.line, "ZSCRIPTPROPERTY '%s': getter '%s' must not be static" % (prop.name, prop.getter))
        if getter.params:
            raise ParseError(prop.line, "ZSCRIPTPROPERTY '%s': getter '%s' must take no parameters" % (prop.name, prop.getter))
        if getter.return_type.strip() in ("void", ""):
            raise ParseError(prop.line, "ZSCRIPTPROPERTY '%s': getter '%s' must return a value" % (prop.name, prop.getter))
        prop.ts_type = ts_type_for(getter.return_type, prop.line)

        if prop.setter:
            setter = by_name.get(prop.setter)
            if setter is None:
                raise ParseError(prop.line, "ZSCRIPTPROPERTY '%s': '%s' is not a ZFUNCTION-annotated member of '%s'"
                                 % (prop.name, prop.setter, info.name))
            if len(setter.params) != 1:
                raise ParseError(prop.line, "ZSCRIPTPROPERTY '%s': setter '%s' must take exactly one parameter"
                                 % (prop.name, prop.setter))

    # Field names are keyed by the *script* spelling: two distinct serialized
    # names can collapse onto one (`m_Foo` and `m_foo` both become `foo`), and
    # the collision only exists after the rename.
    field_lines: Dict[str, int] = {}
    for prop in info.properties:
        if script_property_ts_type(prop.cpp_type) is None:
            continue
        script_name = script_property_name(prop.serialized_name)
        other = field_lines.get(script_name)
        if other is not None:
            raise ParseError(prop.line, "ZPROPERTY '%s' and the field of line %d both become the script property '%s'"
                             % (prop.serialized_name, other, script_name))
        field_lines[script_name] = prop.line

    taken: Dict[str, int] = {}
    for prop in info.script_properties:
        if prop.name in field_lines:
            raise ParseError(prop.line, "ZSCRIPTPROPERTY name '%s' collides with the script-exposed ZPROPERTY field of line %d"
                             % (prop.name, field_lines[prop.name]))
        other = taken.get(prop.name)
        if other is not None:
            raise ParseError(prop.line, "duplicate ZSCRIPTPROPERTY name '%s' (first declared at line %d)" % (prop.name, other))
        taken[prop.name] = prop.line


def _parse_class(cur: _Cursor, zclass_tok: Token) -> ClassInfo:
    open_paren = cur.expect_punct("(", "after ZCLASS")
    spec_tokens = _collect_balanced(cur, open_paren)
    specs, meta = parse_specifiers(spec_tokens, zclass_tok.line)
    if meta:
        raise ParseError(zclass_tok.line, "ZCLASS meta=(...) is not supported yet")

    key_tok = cur.next()
    if key_tok.is_ident("template"):
        raise ParseError(key_tok.line, "ZCLASS on class templates is not supported")
    if not (key_tok.is_ident("class") or key_tok.is_ident("struct")):
        raise ParseError(key_tok.line, "ZCLASS must be followed by a class or struct definition, got '%s'" % key_tok.text)

    head: List[Token] = []
    while True:
        tok = cur.next()
        if tok.is_punct("{"):
            open_brace = tok
            break
        if tok.is_punct(";"):
            raise ParseError(tok.line, "ZCLASS must annotate a class definition, not a forward declaration")
        head.append(tok)

    colon = next((idx for idx, t in enumerate(head) if t.is_punct(":")), len(head))
    name_part = [t for t in head[:colon] if t.kind == IDENT and t.text != "final"]
    if not name_part:
        raise ParseError(key_tok.line, "could not determine the class name")
    name_tok = name_part[-1]

    base: Optional[str] = None
    base_tokens = head[colon + 1 :]
    if base_tokens:
        first: List[Token] = []
        depth = 0
        for t in base_tokens:
            if t.is_punct("<"):
                depth += 1
            elif t.is_punct(">"):
                depth -= 1
            elif t.is_punct(",") and depth == 0:
                break
            first.append(t)
        first = [t for t in first if not (t.kind == IDENT and t.text in ("public", "protected", "private", "virtual"))]
        if first:
            base = format_type(first)
    if base is None:
        raise ParseError(name_tok.line, "ZCLASS '%s' must derive from an Object-based class (e.g. 'public Component')" % name_tok.text)

    info = ClassInfo(name=name_tok.text, class_key=key_tok.text, base=base, line=zclass_tok.line)
    for key, value, line in specs:
        if key not in CLASS_FLAGS:
            raise _unknown(key, line, "ZCLASS", CLASS_FLAGS)
        if value is not None:
            raise ParseError(line, "ZCLASS specifier '%s' does not take a value" % key)
        if key == "Abstract":
            info.abstract = True
        elif key == "CustomTransfer":
            info.custom_transfer = True
        elif key == "NoSuperTransfer":
            info.no_super_transfer = True
    if info.custom_transfer and info.no_super_transfer:
        raise ParseError(zclass_tok.line, "NoSuperTransfer has no effect with CustomTransfer (Transfer() is hand-written)")

    _parse_class_body(cur, open_brace, info)

    end_tok = cur.peek()
    if end_tok is None or not end_tok.is_punct(";"):
        raise ParseError(end_tok.line if end_tok else open_brace.line, "expected ';' after the definition of class '%s'" % info.name)
    cur.next()

    if info.generated_body_line == 0:
        raise ParseError(info.line, "class '%s' is missing ZGENERATED_BODY(%s)" % (info.name, info.name))

    seen: Dict[str, PropertyInfo] = {}
    for prop in info.properties:
        if prop.transient:
            continue
        other = seen.get(prop.serialized_name)
        if other is not None:
            raise ParseError(prop.line, "serialized name '%s' of '%s' collides with '%s' (line %d)" % (prop.serialized_name, prop.member_name, other.member_name, other.line))
        seen[prop.serialized_name] = prop

    # After the serialized-name check: two fields that already serialize to the
    # same name are reported there (a clearer message), and this pass adds the
    # collisions that only appear once the names are camel-cased for scripts.
    _validate_script_properties(info)

    # EditCondition names another ZPROPERTY bool of the same class by its C++
    # member name, optionally negated with '!'. Resolve it to the serialized
    # name the runtime / Inspector look up.
    by_member = {p.member_name: p for p in info.properties}
    for prop in info.properties:
        cond = prop.meta.get("EditCondition")
        if cond is None:
            continue
        text = str(cond).strip()
        negate = text.startswith("!")
        target_name = text[1:].strip() if negate else text
        target = by_member.get(target_name)
        if target is None or target.transient:
            raise ParseError(prop.line, "EditCondition '%s' must name a serialized ZPROPERTY of class '%s'" % (text, info.name))
        if target.cpp_type != "bool":
            raise ParseError(prop.line, "EditCondition '%s' must name a bool property ('%s' is %s)" % (text, target_name, target.cpp_type))
        if target is prop:
            raise ParseError(prop.line, "EditCondition of '%s' refers to itself" % prop.member_name)
        prop.edit_condition = ("!" if negate else "") + target.serialized_name

    # Two ZFUNCTIONs cannot expose the same TypeScript name (the .d.ts would
    # declare a duplicate member and the binding table would overwrite a slot).
    seen_methods: Dict[str, MethodInfo] = {}
    for method in info.methods:
        other = seen_methods.get(method.ts_name)
        if other is not None:
            raise ParseError(method.line, "script name '%s' of '%s' collides with '%s' (line %d)" % (
                method.ts_name, method.name, other.name, other.line))
        seen_methods[method.ts_name] = method
    return info


def _parse_class_body(cur: _Cursor, open_brace: Token, info: ClassInfo) -> None:
    access = "private" if info.class_key == "class" else "public"
    stack = ["{"]
    pairs = {"(": ")", "{": "}", "[": "]"}
    closers = {")": "(", "}": "{", "]": "["}

    while True:
        if cur.at_end():
            raise ParseError(open_brace.line, "unterminated class '%s'" % info.name)
        tok = cur.next()
        depth = len(stack)

        if tok.kind == PUNCT and tok.text in pairs:
            stack.append(tok.text)
            continue
        if tok.kind == PUNCT and tok.text in closers:
            if stack[-1] != closers[tok.text]:
                raise ParseError(tok.line, "mismatched '%s'" % tok.text)
            stack.pop()
            if not stack:
                return
            continue

        if tok.kind != IDENT:
            continue

        if tok.text in ("ZPROPERTY", "ZGENERATED_BODY", "ZFUNCTION", "ZSCRIPTPROPERTY", "ZCLASS") and depth != 1:
            raise ParseError(tok.line, "%s is only allowed directly inside a ZCLASS body" % tok.text)

        if depth == 1 and tok.text in ("public", "protected", "private"):
            nxt = cur.peek()
            if nxt is not None and nxt.is_punct(":"):
                cur.next()
                access = tok.text
            continue

        if tok.text == "ZCLASS":
            raise ParseError(tok.line, "nested ZCLASS is not supported")
        if tok.text == "ZFUNCTION":
            open_paren = cur.expect_punct("(", "after ZFUNCTION")
            func_specs, _ = parse_specifiers(_collect_balanced(cur, open_paren), tok.line)
            _validate_function_specifiers(func_specs, tok.line)
            info.methods.append(_parse_method(cur, tok, func_specs, access))
            continue

        if tok.text == "ZGENERATED_BODY":
            open_paren = cur.expect_punct("(", "after ZGENERATED_BODY")
            args = _collect_balanced(cur, open_paren)
            if len(args) != 1 or not args[0].is_ident():
                raise ParseError(tok.line, "ZGENERATED_BODY expects exactly the class name: ZGENERATED_BODY(%s)" % info.name)
            if args[0].text != info.name:
                raise ParseError(tok.line, "ZGENERATED_BODY(%s) does not match class name '%s'" % (args[0].text, info.name))
            if info.generated_body_line:
                raise ParseError(tok.line, "duplicate ZGENERATED_BODY in class '%s'" % info.name)
            info.generated_body_line = tok.line
            continue

        if tok.text == "ZPROPERTY":
            open_paren = cur.expect_punct("(", "after ZPROPERTY")
            spec_tokens = _collect_balanced(cur, open_paren)
            specs, meta = parse_specifiers(spec_tokens, tok.line)
            info.properties.append(_parse_property(cur, tok, specs, meta, access))
            continue

        if tok.text == "ZSCRIPTPROPERTY":
            open_paren = cur.expect_punct("(", "after ZSCRIPTPROPERTY")
            sp_specs, _ = parse_specifiers(_collect_balanced(cur, open_paren), tok.line)
            values: Dict[str, str] = {}
            for key, value, sline in sp_specs:
                if key in SCRIPT_PROPERTY_KEYS:
                    _check_value_kind(SCRIPT_PROPERTY_KEYS[key], key, value, sline)
                    values[key] = str(value)
                    continue
                raise _unknown(key, sline, "ZSCRIPTPROPERTY specifier", sorted(SCRIPT_PROPERTY_KEYS))
            missing = [k for k in ("Name", "Getter") if k not in values]
            if missing:
                raise ParseError(tok.line, "ZSCRIPTPROPERTY requires %s=..." % "=... and ".join(missing))
            # The accessor has to be a ZFUNCTION so codegen knows its signature
            # (the IR only records annotated methods). Checked after the class
            # body is fully parsed -- the accessor may be declared below.
            info.script_properties.append(
                ScriptPropertyInfo(name=values["Name"], getter=values["Getter"],
                                   setter=values.get("Setter", ""), line=tok.line))
            continue


def _parse_property(cur: _Cursor, zprop_tok: Token, specs: List[Spec], meta, access: str) -> PropertyInfo:
    decl: List[Token] = []
    depth = 0
    while True:
        if cur.at_end():
            raise ParseError(zprop_tok.line, "unterminated declaration after ZPROPERTY")
        tok = cur.next()
        if tok.kind == PUNCT and tok.text in "({[":
            depth += 1
        elif tok.kind == PUNCT and tok.text in ")}]":
            depth -= 1
            if depth < 0:
                raise ParseError(tok.line, "ZPROPERTY must be followed by a member declaration")
        elif tok.is_punct(";") and depth == 0:
            break
        decl.append(tok)

    if not decl:
        raise ParseError(zprop_tok.line, "ZPROPERTY must be followed by a member declaration")
    line = decl[0].line
    if decl[0].kind == IDENT and decl[0].text in _DECL_FORBIDDEN_LEADING:
        raise ParseError(line, "ZPROPERTY: %s" % _DECL_FORBIDDEN_LEADING[decl[0].text])
    if decl[0].is_ident("ZPROPERTY") or any(t.is_ident("ZPROPERTY") for t in decl):
        raise ParseError(line, "ZPROPERTY must be followed by a member declaration")

    angle = paren = 0
    stop = len(decl)
    for idx, t in enumerate(decl):
        if t.kind != PUNCT:
            continue
        if t.text == "<":
            angle += 1
        elif t.text == ">":
            angle -= 1
        elif angle == 0 and t.text in ("{", "=", "[", ":", "(", ","):
            stop = idx
            break
    stop_tok = decl[stop] if stop < len(decl) else None
    if stop_tok is not None:
        if stop_tok.text == "(":
            raise ParseError(stop_tok.line, "ZPROPERTY must annotate a data member; functions will use ZFUNCTION (phase 3)")
        if stop_tok.text == "[":
            raise ParseError(stop_tok.line, "ZPROPERTY on C arrays is not supported; use std::array / std::vector")
        if stop_tok.text == ":":
            raise ParseError(stop_tok.line, "ZPROPERTY on bit-fields is not supported")
        if stop_tok.text == ",":
            raise ParseError(stop_tok.line, "ZPROPERTY must annotate exactly one member per declaration")

    if stop < 2:
        raise ParseError(line, "could not parse the member declaration after ZPROPERTY")
    name_tok = decl[stop - 1]
    if name_tok.kind != IDENT or name_tok.text in _CPP_KEYWORDS_NOT_NAMES:
        raise ParseError(name_tok.line, "could not determine the member name after ZPROPERTY")
    type_tokens = decl[: stop - 1]
    if type_tokens[-1].is_punct("&"):
        raise ParseError(name_tok.line, "ZPROPERTY on reference members is not supported")
    if stop_tok is not None and stop_tok.text in ("{", "="):
        # Make sure the initializer is a single expression: a ',' at depth 0
        # after '=' means multiple declarators (int a = 1, b = 2;).
        if stop_tok.text == "=":
            d = 0
            for t in decl[stop + 1 :]:
                if t.kind == PUNCT and t.text in "({[<":
                    d += 1
                elif t.kind == PUNCT and t.text in ")}]>":
                    d -= 1
                elif t.is_punct(",") and d == 0:
                    raise ParseError(t.line, "ZPROPERTY must annotate exactly one member per declaration")

    prop = PropertyInfo(
        member_name=name_tok.text,
        cpp_type=format_type(type_tokens),
        serialized_name=default_serialized_name(name_tok.text),
        line=name_tok.line,
        access=access,
    )

    for key, value, sline in specs:
        if key in PROPERTY_TRANSFER_FLAGS:
            if value is not None:
                raise ParseError(sline, "ZPROPERTY flag '%s' does not take a value" % key)
            prop.transfer_flags.append(PROPERTY_TRANSFER_FLAGS[key])
        elif key in PROPERTY_PLAIN_FLAGS:
            if value is not None:
                raise ParseError(sline, "ZPROPERTY flag '%s' does not take a value" % key)
            if key == "Transient":
                prop.transient = True
        elif key in PROPERTY_VALUE_KEYS:
            _check_value_kind(PROPERTY_VALUE_KEYS[key], key, value, sline)
            if key == "Name":
                if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", str(value)):
                    raise ParseError(sline, "Name=\"%s\" is not a valid serialized field name" % value)
                prop.serialized_name = str(value)
        else:
            raise _unknown(key, sline, "ZPROPERTY", list(PROPERTY_TRANSFER_FLAGS) + list(PROPERTY_PLAIN_FLAGS) + list(PROPERTY_VALUE_KEYS) + ["meta"])

    if prop.transient and (prop.transfer_flags or any(k == "Name" for k, _v, _l in specs)):
        raise ParseError(zprop_tok.line, "Transient properties are not serialized; Name / transfer flags make no sense on them")

    for mkey, (mvalue, mline) in meta.items():
        if mkey not in META_KEYS:
            raise _unknown(mkey, mline, "meta", META_KEYS)
        _check_value_kind(META_KEYS[mkey], mkey, None if mvalue is True else mvalue, mline)
        prop.meta[mkey] = mvalue
    if prop.meta:
        if prop.transient:
            raise ParseError(zprop_tok.line, "meta=(...) on a Transient property has no effect (the Inspector shows serialized fields only)")
        ranged = [k for k in ("ClampMin", "ClampMax", "UIMin", "UIMax") if k in prop.meta]
        if ranged and prop.cpp_type not in _NUMERIC_TYPES:
            raise ParseError(zprop_tok.line, "%s only applies to numeric scalars, not '%s'" % ("/".join(ranged), prop.cpp_type))
        for lo, hi in (("ClampMin", "ClampMax"), ("UIMin", "UIMax")):
            if lo in prop.meta and hi in prop.meta and prop.meta[lo] > prop.meta[hi]:
                raise ParseError(zprop_tok.line, "%s (%s) is greater than %s (%s)" % (lo, prop.meta[lo], hi, prop.meta[hi]))
    return prop


def ts_type_for(cpp_type: str, line: int) -> str:
    """Map one C++ type of a ZFUNCTION signature to its TypeScript spelling.

    Any other identifier is passed through as a bound class name (the .d.ts
    declares it); anything more complex than that is a hard error.
    """
    text = " ".join(cpp_type.split())
    while text.startswith("const "):
        text = text[len("const ") :].strip()
    while text.endswith("&&") or text.endswith("&"):
        text = text[:-1].strip() if text.endswith("&&") is False else text[:-2].strip()
    if text.endswith("*"):
        inner = text[:-1].strip()
        while inner.startswith("const "):
            inner = inner[len("const ") :].strip()
        if inner == "char":
            return "string"
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", inner):
            return inner
        raise ParseError(line, "ZFUNCTION: '%s' cannot be mapped to a TypeScript type" % cpp_type)
    if text in _TS_TYPES:
        return _TS_TYPES[text]
    if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", text):
        return text
    raise ParseError(line, "ZFUNCTION: '%s' cannot be mapped to a TypeScript type" % cpp_type)


def _build_ts_signature(method: MethodInfo) -> None:
    params = ", ".join(
        "%s: %s" % (p.name or "arg%d" % (i + 1), ts_type_for(p.cpp_type, method.line))
        for i, p in enumerate(method.params)
    )
    method.ts_signature = "%s(%s): %s" % (method.ts_name, params, ts_type_for(method.return_type, method.line))


def _validate_function_specifiers(specs: List[Spec], line: int) -> None:
    seen: Dict[str, int] = {}
    for key, value, sline in specs:
        if key in seen:
            raise ParseError(sline, "duplicate ZFUNCTION specifier '%s'" % key)
        seen[key] = sline
        if key in FUNCTION_VALUE_KEYS:
            _check_value_kind(FUNCTION_VALUE_KEYS[key], key, value, sline)
            continue
        raise _unknown(key, sline, "ZFUNCTION", list(FUNCTION_VALUE_KEYS))


def _split_top_level(tokens: List[Token], separator: str) -> List[List[Token]]:
    """Split on `separator` at bracket depth 0."""
    out: List[List[Token]] = []
    current: List[Token] = []
    depth = 0
    for tok in tokens:
        if tok.kind == PUNCT:
            if tok.text in "({[<":
                depth += 1
            elif tok.text in ")}]>":
                depth -= 1
            elif tok.text == separator and depth == 0:
                out.append(current)
                current = []
                continue
        current.append(tok)
    out.append(current)
    return [chunk for chunk in out if chunk]


def _parse_method(cur: _Cursor, zfunc_tok: Token, specs: List[Spec], access: str) -> MethodInfo:
    """Parse the member function declaration that follows ZFUNCTION(...).

    Only the shape needed to emit a script binding is extracted: return type,
    name, parameter types, const-ness and static-ness. Default arguments,
    bodies, `override` / `noexcept` and ref-qualifiers are accepted and ignored.
    """
    decl: List[Token] = []
    depth = 0
    terminated = False
    while True:
        if cur.at_end():
            raise ParseError(zfunc_tok.line, "unterminated declaration after ZFUNCTION")
        tok = cur.next()
        if tok.kind == PUNCT:
            if tok.text == "{" and depth == 0:
                # Inline body: skip it (balanced) and treat the declaration as
                # done. The '{' itself stays out of `decl`, otherwise the
                # trailing-qualifier check below would trip on it.
                depth = 1
                while depth > 0 and not cur.at_end():
                    inner = cur.next()
                    if inner.kind == PUNCT:
                        if inner.text in "({[":
                            depth += 1
                        elif inner.text in ")}]":
                            depth -= 1
                terminated = True
                break
            if tok.text in "({[":
                depth += 1
            elif tok.text in ")}]":
                depth -= 1
                if depth < 0:
                    raise ParseError(tok.line, "ZFUNCTION must be followed by a member function declaration")
            elif tok.text == ";" and depth == 0:
                terminated = True
                break
        decl.append(tok)

    del terminated  # the loop above only exits via `break` or raises
    if not decl:
        raise ParseError(zfunc_tok.line, "ZFUNCTION must be followed by a member function declaration")

    # 1) Leading declaration specifiers.
    is_static = False
    while decl and decl[0].is_ident() and decl[0].text in _FUNCTION_LEADING:
        if decl[0].text == "static":
            is_static = True
        decl = decl[1:]
    if not decl:
        raise ParseError(zfunc_tok.line, "ZFUNCTION: could not find a function declaration")

    # 2) Parameter list: the first '(' at depth 0. Tokens before it are
    #    [return type] [name]; tokens after the matching ')' are qualifiers.
    open_idx = -1
    depth = 0
    for idx, tok in enumerate(decl):
        if tok.kind != PUNCT:
            continue
        if tok.text in "({[":
            depth += 1
            if tok.text == "(" and depth == 1:
                open_idx = idx
                break
        elif tok.text in ")}]":
            depth -= 1
    if open_idx < 0:
        raise ParseError(zfunc_tok.line, "ZFUNCTION must annotate a function declaration (no parameter list found)")

    close_idx = -1
    depth = 0
    for idx in range(open_idx, len(decl)):
        tok = decl[idx]
        if tok.kind != PUNCT:
            continue
        if tok.text in "({[":
            depth += 1
        elif tok.text in ")}]":
            depth -= 1
            if depth == 0:
                close_idx = idx
                break
    if close_idx < 0:
        raise ParseError(zfunc_tok.line, "unbalanced parameter list after ZFUNCTION")

    head = decl[:open_idx]
    param_tokens = decl[open_idx + 1 : close_idx]
    tail = decl[close_idx + 1 :]

    # 3) Trailing qualifiers.
    is_const = False
    for tok in tail:
        if tok.kind == PUNCT:
            if tok.text == "=":
                raise ParseError(tok.line, "ZFUNCTION on a pure-virtual / deleted function is not supported")
            if tok.text in ("&", "&&"):
                continue  # ref-qualifier
            raise ParseError(tok.line, "ZFUNCTION: unsupported trailing token '%s'" % tok.text)
        if tok.is_ident() and tok.text in _FUNCTION_TRAILING:
            if tok.text == "const":
                is_const = True
            continue
        raise ParseError(tok.line, "ZFUNCTION: unsupported trailing token '%s'" % tok.text)

    # 4) Name + return type.
    if not head:
        raise ParseError(zfunc_tok.line, "ZFUNCTION: missing return type")
    name_tok = head[-1]
    if name_tok.kind != IDENT:
        raise ParseError(zfunc_tok.line, "ZFUNCTION: could not determine the function name")
    name = name_tok.text
    if name.startswith("~") or name.startswith("operator"):
        raise ParseError(zfunc_tok.line, "ZFUNCTION on '%s' is not supported" % name)
    return_tokens = head[:-1]
    if not return_tokens:
        raise ParseError(zfunc_tok.line, "ZFUNCTION on '%s': a constructor cannot be bound (no return type)" % name)
    if any(t.is_ident("operator") for t in return_tokens):
        raise ParseError(zfunc_tok.line, "ZFUNCTION on operator overloads is not supported")

    # 5) Parameters.
    params: List[ParamInfo] = []
    for chunk in _split_top_level(param_tokens, ","):
        # Drop a default argument ("... = expr").
        stripped: List[Token] = []
        d = 0
        for tok in chunk:
            if tok.kind == PUNCT:
                if tok.text in "({[<":
                    d += 1
                elif tok.text in ")}]>":
                    d -= 1
                elif tok.text == "=" and d == 0:
                    break
            stripped.append(tok)
        if not stripped:
            raise ParseError(zfunc_tok.line, "ZFUNCTION on '%s': empty parameter" % name)
        pname = ""
        if len(stripped) >= 2 and stripped[-1].kind == IDENT and stripped[-1].text not in _CPP_KEYWORDS_NOT_NAMES \
                and not (stripped[-2].kind == PUNCT and stripped[-2].text == "::"):
            pname = stripped[-1].text
            stripped = stripped[:-1]
        if not stripped:
            raise ParseError(zfunc_tok.line, "ZFUNCTION on '%s': parameter '%s' has no type" % (name, pname))
        params.append(ParamInfo(cpp_type=format_type(stripped), name=pname))

    method = MethodInfo(
        name=name,
        return_type=format_type(return_tokens),
        line=name_tok.line,
        access=access,
        params=params,
        is_const=is_const,
        is_static=is_static,
    )
    for key, value, sline in specs:
        if key == "Name":
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", str(value)):
                raise ParseError(sline, "Name=\"%s\" is not a valid script method name" % value)
            method.ts_name = str(value)
    if not method.ts_name:
        method.ts_name = method.name
    _build_ts_signature(method)
    return method
