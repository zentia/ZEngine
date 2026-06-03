#pragma once

class RetriableOperation
{
public:
    template<typename T>
    static bool Perform(T& operation)
    {
        while (true)
        {
            if (operation.Execute())
            {
                return true;
            }
            return false;
        }
        return false;
    }
};