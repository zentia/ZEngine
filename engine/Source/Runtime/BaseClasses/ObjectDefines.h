#pragma once
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/CoreMacros.h"
#include "Type.h"

#include <functional>
#include <vector>

class Object;

#define EXPORTDLL

#define REGISTER_CLASS_TRAITS(...)              \
                                                \
public:                                         \
    struct kTypeFlags                           \
    {                                           \
        enum                                    \
        {                                       \
            value = UNSIGNED_FLAGS(__VA_ARGS__) \
        };                                      \
    };                                          \
    class MISSING_SEMICOLON_AFTER_REGISTER_CLASS_TRAITS_MACRO

#define REGISTER_CLASS(TYPE_NAME_)                              \
                                                                \
public:                                                         \
    typedef ThisType Super;                                     \
    typedef TYPE_NAME_ ThisType;                                \
                                                                \
    static const char* GetPPtrTypeString()                      \
    {                                                           \
        return "PPtr<" #TYPE_NAME_ ">";                         \
    }                                                           \
                                                                \
private:                                                        \
    virtual const Type* GetTypeVirtualInternal() const override \
    {                                                           \
        return TypeOf<TYPE_NAME_>();                            \
    }

template<typename T>
void RegisterKlass();

// 自动注册机制：全局注册表
namespace AutoTypeRegistration
{
    using RegisterFunction = void (*)();

    // 获取全局注册函数列表（线程安全）
    std::vector<RegisterFunction>& GetRegisterFunctions();

    // 添加注册函数到列表
    void AddRegisterFunction(RegisterFunction func);

    // 执行所有注册函数
    void ExecuteAllRegistrations();
}  // namespace AutoTypeRegistration

#define DECLARE_OBJECT_SERIALIZE(...)                                             \
                                                                                  \
public:                                                                           \
    static const char* GetTypeString()                                            \
    {                                                                             \
        return TypeOf<ThisType>()->GetName();                                     \
    }                                                                             \
    static bool AllowTransferOptimization()                                       \
    {                                                                             \
        return false;                                                             \
    }                                                                             \
    template<typename TransferFunction>                                           \
    void Transfer(TransferFunction& transfer);                                    \
    virtual void VirtualRedirectTransfer(JSONRead& transfer) override;            \
    virtual void VirtualRedirectTransfer(JSONWrite& transfer) override;           \
    virtual void VirtualRedirectTransfer(YAMLRead& transfer) override;            \
    virtual void VirtualRedirectTransfer(YAMLWrite& transfer) override;           \
    virtual void VirtualRedirectTransfer(StreamedBinaryRead& transfer) override;  \
    virtual void VirtualRedirectTransfer(StreamedBinaryWrite& transfer) override; \
    virtual void VirtualRedirectTransfer(SafeBinaryRead& transfer) override;      \
    virtual void VirtualRedirectTransfer(GenerateTypeTreeTransfer& transfer) override;

enum class ObjectFlags
{
    OF_NoFlags = 0x0,
    OF_BeginDestroyed = 0x1,
    OF_FinishDestroyed = 0x2,
};

#define IMPLEMENT_REGISTER_CLASS_1(TYPE_NAME_) IMPLEMENT_REGISTER_CLASS_3(, TYPE_NAME_, typeid(TYPE_NAME_).hash_code())

#define IMPLEMENT_REGISTER_CLASS_3(NAMESPACE_, TYPE_NAME_, PERSISTENT_TYPE_ID)                                      \
    enum                                                                                                            \
    {                                                                                                               \
        k##NAMESPACE_##TYPE_NAME_##TypeFlags = SelectOnTypeEquality < NAMESPACE_::TYPE_NAME_::ThisType::kTypeFlags, \
        NAMESPACE_::TYPE_NAME_::Super::kTypeFlags,                                                                  \
        kTypeNoFlags,                                                                                               \
        NAMESPACE_::TYPE_NAME_::ThisType::kTypeFlags::value > ::result                                              \
    };                                                                                                              \
                                                                                                                    \
    static TypeRegistrationDesc NAMESPACE_##TYPE_NAME_##_TypeRegistrationDesc = {                                   \
        {&TypeContainer<NAMESPACE_::TYPE_NAME_::Super>::m_Type,                                                     \
         nullptr,                                                                                                   \
         #TYPE_NAME_,                                                                                               \
         #NAMESPACE_,                                                                                               \
         PERSISTENT_TYPE_ID,                                                                                        \
         sizeof(NAMESPACE_::TYPE_NAME_),                                                                            \
         {                                                                                                          \
             static_cast<uint32_t>(Type::DefaultTypeIndex),                                                         \
             0,                                                                                                     \
         },                                                                                                         \
         (static_cast<TypeFlags>(k##NAMESPACE_##TYPE_NAME_##TypeFlags) & kTypeIsAbstract) != 0,                     \
         nullptr,                                                                                                   \
         0},                                                                                                        \
        &TypeContainer<NAMESPACE_::TYPE_NAME_>::m_Type,                                                             \
        nullptr,                                                                                                    \
        nullptr,                                                                                                    \
        nullptr,                                                                                                    \
    };                                                                                                              \
    template<>                                                                                                      \
    void RegisterKlass<NAMESPACE_::TYPE_NAME_>()                                                                    \
    {                                                                                                               \
        NAMESPACE_##TYPE_NAME_##_TypeRegistrationDesc.init.factory =                                                \
            ProduceHelper < NAMESPACE_::TYPE_NAME_,                                                                 \
        (static_cast<TypeFlags>(k##NAMESPACE_##TYPE_NAME_##TypeFlags) & kTypeIsAbstract) != 0 > ::Produce;          \
        TypeRegistrationDesc& desc = NAMESPACE_##TYPE_NAME_##_TypeRegistrationDesc;                                 \
        void GlobalRegisterType(const TypeRegistrationDesc& desc);                                                  \
        GlobalRegisterType(desc);                                                                                   \
    }                                                                                                               \
    namespace                                                                                                       \
    {                                                                                                               \
        struct NAMESPACE_##TYPE_NAME_##_AutoRegistrar                                                               \
        {                                                                                                           \
            NAMESPACE_##TYPE_NAME_##_AutoRegistrar()                                                                \
            {                                                                                                       \
                AutoTypeRegistration::AddRegisterFunction([]() { RegisterKlass<NAMESPACE_::TYPE_NAME_>(); });       \
            }                                                                                                       \
        };                                                                                                          \
        static NAMESPACE_##TYPE_NAME_##_AutoRegistrar NAMESPACE_##TYPE_NAME_##_auto_registrar_instance;             \
    }

#define IMPLEMENT_REGISTER_CLASS(...) PP_VARG_SELECT_OVERLOAD(IMPLEMENT_REGISTER_CLASS_, (__VA_ARGS__))

template<typename T, bool isAbstract>
struct ProduceHelper
{
    static Object* Produce()
    {
        LOG_FATAL(ZEngine, "Can't produce abstract class {}", TypeOf<T>()->GetName());
        return nullptr;
    }
};
template<typename T>
struct ProduceHelper<T, false>
{
    static Object* Produce() { return new T(); }
};

#define IMPLEMENT_OBJECT_SERAILIZE(TYPE_) IMPLEMENT_OBJECT_SERIALIZE_WITH_DECL(TYPE_::, )

#define IMPLEMENT_OBJECT_SERIALIZE_WITH_DECL(PREFIX_, DECL_)                       \
    DECL_ void PREFIX_ VirtualRedirectTransfer(JSONRead& transfer)                 \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }                                                                              \
    DECL_ void PREFIX_ VirtualRedirectTransfer(JSONWrite& transfer)                \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }                                                                              \
    DECL_ void PREFIX_ VirtualRedirectTransfer(YAMLRead& transfer)                 \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }                                                                              \
    DECL_ void PREFIX_ VirtualRedirectTransfer(YAMLWrite& transfer)                \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }                                                                              \
    DECL_ void PREFIX_ VirtualRedirectTransfer(StreamedBinaryRead& transfer)       \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }                                                                              \
    DECL_ void PREFIX_ VirtualRedirectTransfer(StreamedBinaryWrite& transfer)      \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }                                                                              \
    DECL_ void PREFIX_ VirtualRedirectTransfer(SafeBinaryRead& transfer)           \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }                                                                              \
    DECL_ void PREFIX_ VirtualRedirectTransfer(GenerateTypeTreeTransfer& transfer) \
    {                                                                              \
        transfer.TransferBase(*this);                                              \
    }

#define IMPLEMENT_OBJECT_SERIALIZE(TYPE_) IMPLEMENT_OBJECT_SERIALIZE_WITH_DECL(TYPE_::, )

// Undefine the macro first to avoid redefinition warnings
#ifdef INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL
    #undef INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL
#endif

#define INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(FULL_TYPENAME_, DECL_, FUNCTION_NAME_, FUNCTION_RETURN_TYPE_) \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(JSONRead& transfer);                  \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(JSONWrite& transfer);                 \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(YAMLRead& transfer);                  \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(YAMLWrite& transfer);                 \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(StreamedBinaryRead& transfer);        \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(StreamedBinaryWrite& transfer);       \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(SafeBinaryRead& transfer);            \
    template DECL_ FUNCTION_RETURN_TYPE_ FULL_TYPENAME_::FUNCTION_NAME_(GenerateTypeTreeTransfer& transfer);

#define INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(FULL_TYPENAME_) \
    INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(FULL_TYPENAME_, EXPORTDLL, Transfer, void)