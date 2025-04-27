#pragma once
#include "ObjectMacros.h"
#include "Package.h"

class FGCReconstructionGuard;
enum EObjectFlags : int;
class FName;
class UClass;
class UObject;

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