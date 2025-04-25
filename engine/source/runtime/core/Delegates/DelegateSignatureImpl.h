#pragma once
#include "DelegateBase.h"

template <typename DelegateSignature, typename UserPolicy = FDefaultDelegateUserPolicy>
class TDelegateRegistration;

template <typename InRetValType, typename... ParamTypes, typename UserPolicy>
class TDelegateRegistration < InRetValType(ParamTypes...), UserPolicy> : public UserPolicy::FDelegateExtras
{
public:
    
};