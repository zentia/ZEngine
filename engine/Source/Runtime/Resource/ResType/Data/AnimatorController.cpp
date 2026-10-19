#include "AnimatorController.h"

IMPLEMENT_REGISTER_CLASS(AnimatorController)
IMPLEMENT_OBJECT_SERAILIZE(AnimatorController);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AnimatorController)

template<typename TransferFunction>
void AnimatorParameter::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(type, "type");
    transfer.Transfer(default_float, "default_float");
    transfer.Transfer(default_int, "default_int");
    transfer.Transfer(default_bool, "default_bool");
}

template<typename TransferFunction>
void AnimatorCondition::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(parameter, "parameter");
    transfer.Transfer(mode, "mode");
    transfer.Transfer(threshold, "threshold");
}

template<typename TransferFunction>
void AnimatorEvent::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(normalized_time, "normalized_time");
    transfer.Transfer(function_name, "function_name");
}

template<typename TransferFunction>
void AnimatorState::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(clip_file_path, "clip_file_path");
    transfer.Transfer(anim_skel_map_path, "anim_skel_map_path");
    transfer.Transfer(wrap_mode, "wrap_mode");
    transfer.Transfer(speed, "speed");
    transfer.Transfer(clip_length, "clip_length");
    transfer.Transfer(events, "events");
    // Editor-only graph layout. Old files have no "pos_x" node, so
    // SafeBinaryRead leaves it at 0 and the panel auto-arranges.
    transfer.Transfer(pos_x, "pos_x");
    transfer.Transfer(pos_y, "pos_y");
}

template<typename TransferFunction>
void AnimatorTransition::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(from_state, "from_state");
    transfer.Transfer(to_state, "to_state");
    transfer.Transfer(conditions, "conditions");
    transfer.Transfer(duration, "duration");
    transfer.Transfer(has_exit_time, "has_exit_time");
    transfer.Transfer(exit_time, "exit_time");
    transfer.Transfer(priority, "priority");
}

template<typename TransferFunction>
void AnimatorController::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(parameters, "parameters");
    transfer.Transfer(states, "states");
    transfer.Transfer(transitions, "transitions");
    transfer.Transfer(default_state, "default_state");
}

int AnimatorController::FindState(const char* state_name) const
{
    if (state_name == nullptr)
    {
        return -1;
    }
    for (size_t state_index = 0; state_index < states.size(); ++state_index)
    {
        if (states[state_index].name == state_name)
        {
            return static_cast<int>(state_index);
        }
    }
    return -1;
}

int AnimatorController::FindParameter(const char* parameter_name) const
{
    if (parameter_name == nullptr)
    {
        return -1;
    }
    for (size_t parameter_index = 0; parameter_index < parameters.size(); ++parameter_index)
    {
        if (parameters[parameter_index].name == parameter_name)
        {
            return static_cast<int>(parameter_index);
        }
    }
    return -1;
}
