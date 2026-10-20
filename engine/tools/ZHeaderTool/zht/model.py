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
    properties: List[PropertyInfo] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "class_key": self.class_key,
            "base": self.base,
            "line": self.line,
            "generated_body_line": self.generated_body_line,
            "abstract": self.abstract,
            "custom_transfer": self.custom_transfer,
            "properties": [p.to_dict() for p in self.properties],
        }


@dataclass
class HeaderInfo:
    path: str  # absolute, as given on the command line
    include_path: str  # path relative to the include root, '/'-separated
    classes: List[ClassInfo] = field(default_factory=list)

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
        }
