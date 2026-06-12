"""Utility functions: renderdoc module loading, serialization helpers, enum mappings."""

from __future__ import annotations

import json
import os
import sys
from typing import Any


def load_renderdoc():
    """Load the renderdoc Python module, configuring paths from environment."""
    if "renderdoc" in sys.modules:
        return sys.modules["renderdoc"]

    module_path = os.environ.get("RENDERDOC_MODULE_PATH", "")
    dll_path = os.environ.get("RENDERDOC_DLL_PATH", "")
    if module_path:
        if module_path not in sys.path:
            sys.path.insert(0, module_path)
        if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
            os.add_dll_directory(module_path)
            if dll_path and dll_path not in (module_path, ""):
                os.add_dll_directory(dll_path)

    import renderdoc  # noqa: E402

    return renderdoc


rd = load_renderdoc()

# ── Shader stage mapping ──

SHADER_STAGE_MAP: dict[str, Any] = {
    "vertex": rd.ShaderStage.Vertex,
    "hull": rd.ShaderStage.Hull,
    "domain": rd.ShaderStage.Domain,
    "geometry": rd.ShaderStage.Geometry,
    "pixel": rd.ShaderStage.Pixel,
    "compute": rd.ShaderStage.Compute,
}

# ── FileType mapping ──

FILE_TYPE_MAP: dict[str, Any] = {
    "png": rd.FileType.PNG,
    "jpg": rd.FileType.JPG,
    "bmp": rd.FileType.BMP,
    "tga": rd.FileType.TGA,
    "hdr": rd.FileType.HDR,
    "exr": rd.FileType.EXR,
    "dds": rd.FileType.DDS,
}

# ── MeshDataStage mapping ──

MESH_DATA_STAGE_MAP: dict[str, Any] = {
    "vsin": rd.MeshDataStage.VSIn,
    "vsout": rd.MeshDataStage.VSOut,
    "gsout": rd.MeshDataStage.GSOut,
}

# ── Enum readable-name mappings ──

BLEND_FACTOR_MAP: dict[int, str] = {
    0: "Zero", 1: "One", 2: "SrcColor", 3: "InvSrcColor",
    4: "DstColor", 5: "InvDstColor", 6: "SrcAlpha", 7: "InvSrcAlpha",
    8: "DstAlpha", 9: "InvDstAlpha", 10: "SrcAlphaSat",
    11: "BlendFactor", 12: "InvBlendFactor",
    13: "Src1Color", 14: "InvSrc1Color", 15: "Src1Alpha", 16: "InvSrc1Alpha",
}

BLEND_OP_MAP: dict[int, str] = {
    0: "Add", 1: "Subtract", 2: "RevSubtract", 3: "Min", 4: "Max",
}

COMPARE_FUNC_MAP: dict[int, str] = {
    0: "AlwaysFalse", 1: "Never", 2: "Less", 3: "LessEqual",
    4: "Greater", 5: "GreaterEqual", 6: "Equal", 7: "NotEqual", 8: "Always",
}

STENCIL_OP_MAP: dict[int, str] = {
    0: "Keep", 1: "Zero", 2: "Replace", 3: "IncrSat",
    4: "DecrSat", 5: "Invert", 6: "IncrWrap", 7: "DecrWrap",
}

CULL_MODE_MAP: dict[int, str] = {0: "None", 1: "Front", 2: "Back", 3: "FrontAndBack"}

FILL_MODE_MAP: dict[int, str] = {0: "Solid", 1: "Wireframe", 2: "Point"}

TOPOLOGY_MAP: dict[int, str] = {
    0: "Unknown",
    1: "PointList", 2: "LineList", 3: "LineStrip",
    4: "TriangleList", 5: "TriangleStrip", 6: "TriangleFan",
    7: "LineList_Adj", 8: "LineStrip_Adj",
    9: "TriangleList_Adj", 10: "TriangleStrip_Adj",
    11: "PatchList",
}

VAR_TYPE_MAP: dict[int, str] = {
    0: "Float", 1: "Double", 2: "Half",
    3: "SInt", 4: "UInt", 5: "SShort", 6: "UShort",
    7: "SLong", 8: "ULong", 9: "SByte", 10: "UByte",
    11: "Bool", 12: "Enum", 13: "GPUPointer",
    14: "ConstantBlock", 15: "Struct", 16: "Unknown",
}

SYSTEM_VALUE_MAP: dict[int, str] = {
    0: "None", 1: "Position", 2: "ClipDistance", 3: "CullDistance",
    4: "RTIndex", 5: "ViewportIndex", 6: "VertexIndex", 7: "PrimitiveIndex",
    8: "InstanceIndex", 9: "DispatchThreadIndex", 10: "GroupIndex",
    11: "GroupFlatIndex", 12: "GroupThreadIndex", 13: "GSInstanceIndex",
    14: "OutputControlPointIndex", 15: "DomainLocation",
    16: "IsFrontFace", 17: "MSAACoverage", 18: "MSAASamplePosition",
    19: "MSAASampleIndex", 20: "PatchNumVertices",
    21: "OuterTessFactor", 22: "InsideTessFactor",
    23: "ColourOutput", 24: "DepthOutput", 25: "DepthOutputGreaterEqual",
    26: "DepthOutputLessEqual",
}

TEXTURE_DIM_MAP: dict[int, str] = {
    0: "Unknown", 1: "Buffer", 2: "Texture1D", 3: "Texture1DArray",
    4: "Texture2D", 5: "Texture2DArray", 6: "Texture2DMS", 7: "Texture2DMSArray",
    8: "Texture3D", 9: "TextureCube", 10: "TextureCubeArray",
}

RESOURCE_USAGE_MAP: dict[int, str] = {
    0: "None", 1: "VertexBuffer", 2: "IndexBuffer",
    3: "VS_Constants", 4: "HS_Constants", 5: "DS_Constants",
    6: "GS_Constants", 7: "PS_Constants", 8: "CS_Constants",
    9: "All_Constants",
    10: "StreamOut", 11: "IndirectArg",
    16: "VS_Resource", 17: "HS_Resource", 18: "DS_Resource",
    19: "GS_Resource", 20: "PS_Resource", 21: "CS_Resource",
    22: "All_Resource",
    32: "VS_RWResource", 33: "HS_RWResource", 34: "DS_RWResource",
    35: "GS_RWResource", 36: "PS_RWResource", 37: "CS_RWResource",
    38: "All_RWResource",
    48: "InputTarget", 49: "ColorTarget", 50: "DepthStencilTarget",
    64: "Clear", 65: "GenMips", 66: "Resolve", 67: "ResolveSrc",
    68: "ResolveDst", 69: "Copy", 70: "CopySrc", 71: "CopyDst",
    72: "Barrier",
}


def enum_str(value, mapping: dict, fallback_prefix: str = "") -> str:
    """Convert an enum value to a readable string, falling back to str()."""
    try:
        int_val = int(value)
    except (TypeError, ValueError):
        return str(value)
    return mapping.get(int_val, f"{fallback_prefix}{value}")


def blend_formula(color_src: str, color_dst: str, color_op: str,
                  alpha_src: str, alpha_dst: str, alpha_op: str) -> str:
    """Generate a human-readable blend formula string."""
    def _op_str(op: str, a: str, b: str) -> str:
        if op == "Add":
            return f"{a} + {b}"
        elif op == "Subtract":
            return f"{a} - {b}"
        elif op == "RevSubtract":
            return f"{b} - {a}"
        elif op == "Min":
            return f"min({a}, {b})"
        elif op == "Max":
            return f"max({a}, {b})"
        return f"{a} {op} {b}"

    def _factor(f: str, channel: str) -> str:
        src, dst = f"src.{channel}", f"dst.{channel}"
        factor_map = {
            "Zero": "0", "One": "1",
            "SrcColor": "src.rgb", "InvSrcColor": "(1-src.rgb)",
            "DstColor": "dst.rgb", "InvDstColor": "(1-dst.rgb)",
            "SrcAlpha": "src.a", "InvSrcAlpha": "(1-src.a)",
            "DstAlpha": "dst.a", "InvDstAlpha": "(1-dst.a)",
            "SrcAlphaSat": "sat(src.a)",
            "BlendFactor": "factor", "InvBlendFactor": "(1-factor)",
            "Src1Color": "src1.rgb", "InvSrc1Color": "(1-src1.rgb)",
            "Src1Alpha": "src1.a", "InvSrc1Alpha": "(1-src1.a)",
        }
        return factor_map.get(f, f)

    c_src = _factor(color_src, "rgb")
    c_dst = _factor(color_dst, "rgb")
    color_expr = _op_str(color_op, f"{c_src}*src.rgb", f"{c_dst}*dst.rgb")

    a_src = _factor(alpha_src, "a")
    a_dst = _factor(alpha_dst, "a")
    alpha_expr = _op_str(alpha_op, f"{a_src}*src.a", f"{a_dst}*dst.a")

    return f"color: {color_expr} | alpha: {alpha_expr}"


# ── Error helpers ──

def make_error(message: str, code: str = "API_ERROR") -> dict:
    """Return a standardized error dict."""
    return {"error": message, "code": code}


# ── ActionFlags helpers ──

_ACTION_FLAG_NAMES: dict[int, str] | None = None


def _build_flag_names() -> dict[int, str]:
    """Build a mapping of single-bit flag values to their names."""
    names: dict[int, str] = {}
    for attr in dir(rd.ActionFlags):
        if attr.startswith("_"):
            continue
        val = getattr(rd.ActionFlags, attr)
        if isinstance(val, int) and val != 0 and (val & (val - 1)) == 0:
            names[val] = attr
    return names


def flags_to_list(flags: int) -> list[str]:
    """Convert an ActionFlags bitmask to a list of flag name strings."""
    global _ACTION_FLAG_NAMES
    if _ACTION_FLAG_NAMES is None:
        _ACTION_FLAG_NAMES = _build_flag_names()
    result = []
    for bit, name in _ACTION_FLAG_NAMES.items():
        if flags & bit:
            result.append(name)
    return result


# ── Serialization helpers ──

def serialize_action(action, structured_file, depth: int = 0, max_depth: int = 2) -> dict:
    """Serialize an ActionDescription to a dict."""
    result = {
        "event_id": action.eventId,
        "name": action.GetName(structured_file),
        "flags": flags_to_list(action.flags),
        "num_indices": action.numIndices,
        "num_instances": action.numInstances,
    }
    outputs = []
    for o in action.outputs:
        rid = int(o)
        if rid != 0:
            outputs.append(str(o))
    if outputs:
        result["outputs"] = outputs
    depth_id = int(action.depthOut)
    if depth_id != 0:
        result["depth_output"] = str(action.depthOut)

    if depth < max_depth and len(action.children) > 0:
        result["children"] = [
            serialize_action(c, structured_file, depth + 1, max_depth)
            for c in action.children
        ]
    elif len(action.children) > 0:
        result["children_count"] = len(action.children)

    return result


def serialize_action_detail(action, structured_file) -> dict:
    """Serialize a single ActionDescription with full detail (no depth limit on self, but no children expansion)."""
    result = {
        "event_id": action.eventId,
        "name": action.GetName(structured_file),
        "flags": flags_to_list(action.flags),
        "num_indices": action.numIndices,
        "num_instances": action.numInstances,
        "index_offset": action.indexOffset,
        "base_vertex": action.baseVertex,
        "vertex_offset": action.vertexOffset,
        "instance_offset": action.instanceOffset,
        "drawIndex": action.drawIndex,
    }
    outputs = []
    for o in action.outputs:
        rid = int(o)
        if rid != 0:
            outputs.append(str(o))
    result["outputs"] = outputs

    depth_id = int(action.depthOut)
    result["depth_output"] = str(action.depthOut) if depth_id != 0 else None

    if action.parent:
        result["parent_event_id"] = action.parent.eventId
    if action.previous:
        result["previous_event_id"] = action.previous.eventId
    if action.next:
        result["next_event_id"] = action.next.eventId
    result["children_count"] = len(action.children)

    return result


def serialize_texture_desc(tex) -> dict:
    """Serialize a TextureDescription to a dict."""
    return {
        "resource_id": str(tex.resourceId),
        "name": tex.name if hasattr(tex, "name") else "",
        "width": tex.width,
        "height": tex.height,
        "depth": tex.depth,
        "array_size": tex.arraysize,
        "mips": tex.mips,
        "format": str(tex.format.Name()),
        "dimension": enum_str(tex.dimension, TEXTURE_DIM_MAP, "Dim."),
        "msqual": tex.msQual,
        "mssamp": tex.msSamp,
        "creation_flags": tex.creationFlags,
    }


def serialize_buffer_desc(buf) -> dict:
    """Serialize a BufferDescription to a dict."""
    return {
        "resource_id": str(buf.resourceId),
        "name": buf.name if hasattr(buf, "name") else "",
        "length": buf.length,
        "creation_flags": buf.creationFlags,
    }


def serialize_resource_desc(res) -> dict:
    """Serialize a ResourceDescription to a dict."""
    try:
        name = res.name if hasattr(res, "name") else str(res.resourceId)
    except Exception:
        name = str(res.resourceId)
    return {
        "resource_id": str(res.resourceId),
        "name": name,
        "type": str(res.type),
    }


def _get_var_type_accessor(var) -> tuple[str, str]:
    """Determine the correct value accessor and type name for a ShaderVariable.

    Returns (accessor_attr, type_name) based on var.type.
    """
    try:
        var_type = int(var.type)
    except Exception:
        return "f32v", "float"

    # RenderDoc VarType enum values:
    # Float=0, Double=1, Half=2, SInt=3, UInt=4, SShort=5, UShort=6,
    # SLong=7, ULong=8, SByte=9, UByte=10, Bool=11, Enum=12,
    # GPUPointer=13, ConstantBlock=14, Struct=15, Unknown=16
    if var_type == 1:  # Double
        return "f64v", "double"
    elif var_type in (3, 5, 7, 9):  # SInt, SShort, SLong, SByte
        return "s32v", "int"
    elif var_type in (4, 6, 8, 10):  # UInt, UShort, ULong, UByte
        return "u32v", "uint"
    elif var_type == 11:  # Bool
        return "u32v", "bool"
    else:  # Float, Half, and all others default to float
        return "f32v", "float"


def serialize_shader_variable(var, max_depth: int = 10, depth: int = 0) -> dict:
    """Recursively serialize a ShaderVariable to a dict."""
    result: dict[str, Any] = {"name": var.name}
    if len(var.members) == 0:
        # Leaf variable - extract values using correct type accessor
        accessor, type_name = _get_var_type_accessor(var)
        result["type"] = type_name
        values = []
        for r in range(var.rows):
            row_vals = []
            for c in range(var.columns):
                row_vals.append(getattr(var.value, accessor)[r * var.columns + c])
            values.append(row_vals)
        # Flatten single-row results
        if len(values) == 1:
            result["value"] = values[0]
        else:
            result["value"] = values
        result["rows"] = var.rows
        result["columns"] = var.columns
    elif depth < max_depth:
        result["members"] = [
            serialize_shader_variable(m, max_depth, depth + 1) for m in var.members
        ]
    return result


def serialize_usage_entry(usage) -> dict:
    """Serialize a single EventUsage entry."""
    return {
        "event_id": usage.eventId,
        "usage": enum_str(usage.usage, RESOURCE_USAGE_MAP, "Usage."),
    }


def serialize_sig_element(sig) -> dict:
    """Serialize a SigParameter (shader signature element)."""
    return {
        "var_name": sig.varName,
        "semantic_name": sig.semanticName,
        "semantic_index": sig.semanticIndex,
        "semantic_idx_name": sig.semanticIdxName,
        "var_type": enum_str(sig.varType, VAR_TYPE_MAP, "VarType."),
        "comp_count": sig.compCount,
        "system_value": enum_str(sig.systemValue, SYSTEM_VALUE_MAP, "SysValue."),
        "reg_index": sig.regIndex,
    }


def to_json(obj: Any) -> str:
    """Serialize to compact JSON string."""
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)
