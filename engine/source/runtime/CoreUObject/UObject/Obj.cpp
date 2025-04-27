#include "Package.h"
static UPackage* GObjTransientPkg = nullptr;

UPackage* GetTransientPackage()
{ return GObjTransientPkg; }