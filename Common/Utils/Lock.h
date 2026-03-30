#pragma once

#include <mutex>
#include <shared_mutex>

namespace UTILITY
{
    using Lock = std::shared_timed_mutex;
    using WriteLock = std::unique_lock<Lock>;
    using ReadLock = std::shared_lock<Lock>;

} //namespace UTILITY