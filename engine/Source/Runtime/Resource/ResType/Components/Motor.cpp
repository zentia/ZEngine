#include "Runtime/Resource/ResType/Components/Motor.h"

#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/Core/Base/Macro.h"

// PhysicsControllerConfig is referenced through PPtr<ControllerConfig>, so it
// needs a runtime factory to be producible by name.
IMPLEMENT_REGISTER_CLASS(PhysicsControllerConfig)

MotorComponentRes::~MotorComponentRes() {}