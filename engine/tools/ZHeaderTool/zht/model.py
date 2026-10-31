"""Intermediate representation produced by the parser and consumed by codegen.

The IR is deliberately plain data (and JSON-serialisable via `to_dict`) so a
future phase can dump it for the script-binding / .d.ts generators without
touching the parser.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Union

SpecValue = Union[str, int, float, bool]


@dataclass
class PropertyInfo:
    member_name: str
    cpp_type: str
    serialized_name: str
    line: int
    access: str
    # TransferMetaFlags enumerator names, in declaration order.
    transfer_flags: List[str] = field(default_factory=list)
    # True when the property is reflected but not serialized (ZPROPERTY(Transient)).
    transient: bool = False
    # Editor / UI metadata (Tooltip, Category, ClampMin...). Parsed and
    # validated in phase 1, emitted from phase 2 on.
    meta: Dict[str, SpecValue] = field(default_factory=dict)
    # Resolved EditCondition: serialized name of the gating bool, '!'-prefixed
    # when negated. Empty when the property has no EditCondition.
    edit_condition: str = ""

    def to_dict(self) -> dict:
        return {
            "member_name": self.member_name,
            "cpp_type": self.cpp_type,
            "serialized_name": self.serialized_name,
            "line": self.line,
            "access": self.access,
            "transfer_flags": list(self.transfer_flags),
            "transient": self.transient,
            "meta": dict(self.meta),
            "edit_condition": self.edit_condition,
        }


@dataclass
class ParamInfo:
    """One parameter of a ZFUNCTION-annotated method."""
    cpp_type: str
    name: str = ""  # may be empty for unnamed parameters

    def to_dict(self) -> dict:
        return {"cpp_type": self.cpp_type, "name": self.name}


@dataclass
class MethodInfo:
    """A member function annotated with ZFUNCTION (script binding, phase 3).

    `ts_name` is the name exposed to TypeScript (Name=".." overrides it) and
    `ts_signature` is the ready-to-emit d.ts member line, both filled in by
    codegen, which owns the C++ -> TypeScript type mapping.
    """
    name: str
    return_type: str
    line: int
    access: str
    params: List[ParamInfo] = field(default_factory=list)
    is_const: bool = False
    is_static: bool = False
    ts_name: str = ""
    ts_signature: str = ""

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "return_type": self.return_type,
            "line": self.line,
            "access": self.access,
            "params": [p.to_dict() for p in self.params],
            "is_const": self.is_const,
            "is_static": self.is_static,
            "ts_name": self.ts_name,
            "ts_signature": self.ts_signature,
        }


@dataclass
class EnumValueInfo:
    """One enumerator of a ZENUM-annotated enum."""
    name: str
    value: int
    line: int
    # Script-facing spelling (PascalCase, the TypeScript convention).
    ts_name: str = ""

    def to_dict(self) -> dict:
        return {"name": self.name, "value": self.value, "line": self.line, "ts_name": self.ts_name}


@dataclass
class EnumInfo:
    """A ZENUM-annotated enum (phase 8).

    Enums cross the boundary as numbers -- that is what `std::is_enum` gives us
    generically -- but scripts need the names, so the tool also emits a
    (name, value) table. The runtime publishes it as a frozen global object and
    the .d.ts declares a `declare enum`, which together make
    `camera.mode = CameraMode.Free` type-check *and* run.
    """
    name: str
    namespace: str
    line: int
    is_scoped: bool = False
    values: List[EnumValueInfo] = field(default_factory=list)

    @property
    def qualified_name(self) -> str:
        return self.namespace + "::" + self.name if self.namespace else self.name

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "namespace": self.namespace,
            "line": self.line,
            "is_scoped": self.is_scoped,
            "values": [v.to_dict() for v in self.values],
        }


@dataclass
class ScriptPropertyInfo:
    """A script-facing property backed by ZFUNCTION-annotated accessors
    (ZSCRIPTPROPERTY, phase 7).

    Unlike a ZPROPERTY field this has no data member of its own: it publishes an
    existing getter (and optionally a setter) as a pesapi accessor property, so
    scripts can write `transform.position = v` instead of `SetPosition(v)`.
    `ts_type` is filled in from the getter's return type by codegen.

    `is_static` mirrors the getter: when the accessor is a static ZFUNCTION the
    property is a *class-level* one (`Color.red`), not an instance one
    (`transform.position`). pesapi keeps those in a separate `Variables` table
    and installs them on the constructor, so the two never share an index.
    """
    name: str
    getter: str
    setter: str = ""  # empty -> read-only
    line: int = 0
    ts_type: str = ""
    is_static: bool = False

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "getter": self.getter,
            "setter": self.setter,
            "line": self.line,
            "ts_type": self.ts_type,
            "is_static": self.is_static,
        }


@dataclass
class ClassInfo:
    name: str
    class_key: str  # "class" | "struct"
    base: Optional[str]
    line: int
    generated_body_line: int = 0
    abstract: bool = False
    custom_transfer: bool = False
    no_super_transfer: bool = False
    namespace: str = ""
    properties: List[PropertyInfo] = field(default_factory=list)
    methods: List[MethodInfo] = field(default_factory=list)
    script_properties: List[ScriptPropertyInfo] = field(default_factory=list)

    @property
    def qualified_name(self) -> str:
        return self.namespace + "::" + self.name if self.namespace else self.name

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "class_key": self.class_key,
            "base": self.base,
            "line": self.line,
            "generated_body_line": self.generated_body_line,
            "abstract": self.abstract,
            "custom_transfer": self.custom_transfer,
            "no_super_transfer": self.no_super_transfer,
            "namespace": self.namespace,
            "properties": [p.to_dict() for p in self.properties],
            "methods": [m.to_dict() for m in self.methods],
            "script_properties": [p.to_dict() for p in self.script_properties],
        }


@dataclass
class HeaderInfo:
    path: str  # absolute, as given on the command line
    include_path: str  # path relative to the include root, '/'-separated
    classes: List[ClassInfo] = field(default_factory=list)
    enums: List[EnumInfo] = field(default_factory=list)

    @property
    def base_name(self) -> str:
        name = self.include_path.rsplit("/", 1)[-1]
        return name[: name.rfind(".")] if "." in name else name

    @property
    def generated_header_name(self) -> str:
        return self.base_name + ".generated.h"

    @property
    def generated_source_name(self) -> str:
        return self.base_name + ".gen.cpp"

    def to_dict(self) -> dict:
        return {
            "path": self.path,
            "include_path": self.include_path,
            "classes": [c.to_dict() for c in self.classes],
            "enums": [e.to_dict() for e in self.enums],
        }
