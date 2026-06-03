#pragma once

#include "AutoReadWriteLock.h"
#include "Runtime/Utility/NonCopyable.h"

class ReadWriteLock : NonCopyable
{
public:
    using AutoReadLock = AutoReadLockT<ReadWriteLock>;
    using AutoWriteLock = AutoWriteLockT<ReadWriteLock>;
};