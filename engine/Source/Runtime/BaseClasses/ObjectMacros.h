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
// ZGENERATED_BODY(Name) expands to ZGEN_BODY_Name, defined in the
// generated header (REGISTER_CLASS + DECLARE_OBJECT_SERIALIZE). A
// "ZGEN_BODY_Name was not declared" compile error therefore means the
// .generated.h include is missing or the build did not run ZHeaderTool.
// =====================================================================

#include "Runtime/BaseClasses/ObjectDefines.h"

#define ZCLASS(...)
#define ZPROPERTY(...)
#define ZFUNCTION(...)

#define ZGENERATED_BODY(ClassName) ZGEN_BODY_##ClassName
