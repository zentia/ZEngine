#include "Field.h"

FFieldClass* FField::StaticClass()
{
    static FFieldClass StaticFieldClass;
    return &StaticFieldClass;
}
