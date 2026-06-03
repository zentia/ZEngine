#pragma once

template<typename T>
class SerializeTraitsBase
{
public:
    typedef T value_type;

    static int GetByteSize() { return sizeof(value_type); }
    static size_t GetAlignOf() { return alignof(value_type); }
    static bool AllowTypeConversion() { return true; }
};

template<typename T>
class SerializeTraitsBaseForBasicType : public SerializeTraitsBase<T>
{
public:
    typedef T value_type;

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        transfer.TransferBasicData(data);
    }
};

template<typename T>
class SerializeTraits : public SerializeTraitsBase<T>
{
public:
    using value_type = T;

    inline static const char* GetTypeString(void*) { return value_type::GetTypeString(); }

    static bool AllowTransferOptimization() { return T::AllowTransferOptimization(); }

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        data.Transfer(transfer);
    }
};
