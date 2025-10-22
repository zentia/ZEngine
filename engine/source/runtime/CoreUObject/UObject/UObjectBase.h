#pragma once
#include "ObjectMacros.h"

class FGCReconstructionGuard;
enum EObjectFlags;
class FName;
class UClass;
class UObject;
class UPackage; // forward declaration to avoid circular include

class UObjectBase
{
    friend UObject* StaticAllocateObject(
        const UClass*,
        UObject*,
        FName,
        EObjectFlags,
        EInternalObjectFlags,
        bool,
        bool*,
        UPackage*,
        int32,
        FGCReconstructionGuard*);

public:
    
};