// Type registration for the primitive geometry classes.
//
// Box / Sphere / Capsule are header-only value-ish types, but they derive from
// Object and are referenced through PPtr<Geometry>, so they need a runtime
// factory. ObjectManager::Produce returns nullptr for an unregistered Type,
// which is why a migrator that instantiates a shape from its JSON "$typeName"
// has to have these registered first.

#include "BasicShape.h"

#include "Runtime/BaseClasses/ObjectDefines.h"

IMPLEMENT_REGISTER_CLASS(Box)
IMPLEMENT_REGISTER_CLASS(Sphere)
IMPLEMENT_REGISTER_CLASS(Capsule)
