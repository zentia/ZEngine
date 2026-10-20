#pragma once

// =====================================================================
// Reflection markers consumed by ZHeaderTool (engine/tools/ZHeaderTool).
// ---------------------------------------------------------------------
// To the C++ compiler these are no-ops (except ZGENERATED_BODY); the
// header tool scans for them at build time and writes
// <Name>.generated.h / <Name>.gen.cpp. Usage:
//
//     #include "Runtime/Function/Framework/Component/Component.h"
//     #include "LightComponent.generated.h"      // must be the LAST include
//
//     ZCLASS()
//     class LightComponent : public Component
//     {
//         ZGENERATED_BODY(LightComponent)
//     private:
//         ZPROPERTY()                              // serialized as "flux"
//         Vector3 m_Flux;
//         ZPROPERTY(Name="main", HideInEditor)     // explicit field name + flag
//         bool m_IsMainCamera {false};
//     };
//
// ZCLASS specifiers:     Abstract, CustomTransfer, NoSuperTransfer
//                        (global scope or exactly one named namespace)
// ZPROPERTY specifiers:  Name="..", HideInEditor, ReadOnly, StrongPPtr,
//                        Deprecated, TreatIntegerAsBool, Hex64, Transient,
//                        meta=(Tooltip="..", DisplayName="..", Category="..",
//                              EditCondition="..", ClampMin=, ClampMax=,
//                              UIMin=, UIMax=)
//
// ZFUNCTION specifiers:   Name=".."
//
// ZSCRIPTPROPERTY(Name="..", Getter=".."[, Setter=".."])
//   Publishes an existing ZFUNCTION getter (and optionally a setter) as a
//   pesapi accessor property, so a script writes `transform.position = v`.
//   It carries no data member of its own and may appear anywhere in the class
//   body. The named accessors must themselves be ZFUNCTION-annotated: that is
//   where ZHeaderTool reads the TypeScript type from, and generate time
//   rejects a getter with parameters or a void return. Omitting Setter yields
//   a read-only property.
//
// ZPROPERTY fields whose type has a ZScript::Converter (bool / integers /
// floats / eastl::string / Vector3 / Quaternion) are exposed as script
// properties automatically, named by their serialized name. ReadOnly fields
// are exposed read-only. Everything else (enum class, PPtr, a Res struct,
// std::vector) is skipped -- ZPROPERTY marks a field serialized, not
// script-facing, so an unmarshallable type must not be an error.
//
// ZGENERATED_BODY(Name) expands to ZGEN_BODY_Name, defined in the
// generated header (REGISTER_CLASS + DECLARE_OBJECT_SERIALIZE). A
// "ZGEN_BODY_Name was not declared" compile error therefore means the
// .generated.h include is missing or the build did not run ZHeaderTool.
// =====================================================================

#include "Runtime/BaseClasses/ObjectDefines.h"

#define ZCLASS(...)
#define ZPROPERTY(...)
#define ZFUNCTION(...)
#define ZSCRIPTPROPERTY(...)

#define ZGENERATED_BODY(ClassName) ZGEN_BODY_##ClassName
