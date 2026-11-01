#pragma once
#include "SessionBinding.h"

namespace NovaTerm::Mcp {
struct SerialFileBinding { SessionBinding connection; quint64 ioGeneration{0}; QString configDigest; };

inline bool sameTransferBinding(const SessionBinding& a, const SessionBinding& b)
{
    return a.session == b.session && a.attachment == b.attachment && a.epoch == b.epoch
        && a.generation == b.generation && a.targetFingerprint == b.targetFingerprint;
}

inline bool sameTransferBinding(const SerialFileBinding& a, const SerialFileBinding& b)
{
    return sameTransferBinding(a.connection, b.connection)
        && a.ioGeneration == b.ioGeneration && a.configDigest == b.configDigest;
}
}
