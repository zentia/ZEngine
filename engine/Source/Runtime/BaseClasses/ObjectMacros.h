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
// ZENUM (file scope, or exactly one named namespace; NOT inside a class)
//   Publishes a named enum to scripts:
//       ZENUM()
//       enum class CameraMode : unsigned char { third_person, first_person, free, invalid };
//   It crosses the boundary as a plain int32 (ZScript::Converter's
//   std::is_enum specialisation), and ZHeaderTool additionally emits the
//   (name, value) list so the runtime can publish a `CameraMode` global object
//   and the .d.ts can mirror it with `declare enum CameraMode { ThirdPerson = 0, ... }`.
//   That is what makes `camera.mode = CameraMode.Free` both type-check and run.
//   Enumerator spellings are PascalCased for scripts (third_person ->
//   ThirdPerson); an initializer must be a literal integer, because the .d.ts
//   has to print a concrete number -- `1u << 0` is a generate-time error.
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
// floats / eastl::string / Vector3 / Quaternion) or is a ZENUM-annotated enum
// are exposed as script properties automatically, named by their serialized
// name. ReadOnly fields are exposed read-only. Everything else (a plain
// enum class, PPtr, a Res struct, std::vector) is skipped -- ZPROPERTY marks a
// field serialized, not script-facing, so an unmarshallable type must not be
// an error.
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
#define ZENUM(...)

#define ZGENERATED_BODY(ClassName) ZGEN_BODY_##ClassName
