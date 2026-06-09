#include "Runtime/Function/Input/InputAction.h"

namespace GameActions
{
    InputAction Move("Move", EInputActionValueType::Axis2D);
    InputAction Look("Look", EInputActionValueType::Axis2D);
    InputAction Jump("Jump", EInputActionValueType::Boolean);
    InputAction Sprint("Sprint", EInputActionValueType::Boolean);
    InputAction Crouch("Crouch", EInputActionValueType::Boolean);
    InputAction Fire("Fire", EInputActionValueType::Boolean);
    InputAction ToggleFreeCamera("ToggleFreeCamera", EInputActionValueType::Boolean);

    void Init()
    {
        // Actions are already constructed with correct names/value types.
        // This function exists for any future one-time setup.
    }

    std::vector<InputAction*> GetAll()
    {
        return {
            &Move,
            &Look,
            &Jump,
            &Sprint,
            &Crouch,
            &Fire,
            &ToggleFreeCamera,
        };
    }
} // namespace GameActions
