#pragma once
#include <variant>
namespace NovaTerm::Capabilities {
enum class Rejection { Unavailable, UntrustedPrincipal, Denied, AuthorizationChanged,
    ResourceMismatch, DeadlineExceeded, Cancelled, AlreadyCompleted, DuplicateOperation, Busy };
template<class T> using Result = std::variant<T, Rejection>;
}
