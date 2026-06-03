#pragma once

class Type;

class ConstVariantRef
{
public:
    const Type* GetType() const { return m_Type; }

private:
    const Type* m_Type;
};