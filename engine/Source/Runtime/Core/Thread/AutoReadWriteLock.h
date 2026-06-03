#pragma once

#include "Runtime/Utility/NonCopyable.h"

template<typename TLock>
class AutoReadLockT : NonCopyable
{
public:
    AutoReadLockT(TLock& rwslock)
        : m_Rwslock(rwslock) { m_Rwslock.ReadLock(); }
    ~AutoReadLockT() { m_Rwslock.ReadUnlock(); }

private:
    TLock& m_Rwslock;
};

template<typename TLock>
class AutoWriteLockT : NonCopyable
{
public:
    AutoWriteLockT(TLock& rwslock)
        : m_Rwslock(rwslock) { m_Rwslock.WriteLock(); }
    ~AutoWriteLockT() { m_Rwslock.WriteUnlock(); }

private:
    TLock& m_Rwslock;
};