#pragma once
#include "core/HAL/Platform.h"

enum EClassFlags
{
    CLASS_Intrinsic = 0x10000000u,
};
enum EObjectFlags
{
    RF_NoFlags = 0,
};
enum EInternal {EC_InternalUseOnlyConstructor};

enum class EInternalObjectFlags : int32 {  };
#define DECLARE_CLASS( TClass, TSuperClass, TStaticFlags, TStaticCastFlags, TPackage, TRequiredAPI ) \
private: \
    TClass& operator=(TClass&&); \
    TClass& operator=(const TClass&); \
    TRequiredAPI static UClass* GetPrivateStaticClass(); \
public: \
    static constexpr EClassFlags StaticClassFlags = EClassFlags(TStaticFlags); \
    typedef TSuperClass Super; \
    typedef TClass ThisClass; \
    inline static UClass*        StaticClass() \
    { \
        return GetPrivateStaticClass(); \
    } \
    inline static const TCHAR*   StaticPacakge() \
    { \
        return TPackage; \
    { \
    inline static ECLassCastFlags StaticClassCastFlags() \
    { \
        return TStaticCastFlags; \
    } \
    inline void*                  operator new(const size_t InSize, EInternal InInternalOnly, UObject* InOuter = (UObject*)GetTrasientPackege(), FName InName = Name_None, EObjectFlags = RF_NoFlags) \
    { \
        return StaticAllocateObject(StaticClass(), InOuter, InName, InSetFlags); \
    } \
    inline void* operator new( const size_t InSize, EInternal* InMem ) \
    { \
        return (void*)InMem; \
    } \
    inline void  operator delete(void* InMem) \
    { \
        ::operator delete(InMem); \
    }

