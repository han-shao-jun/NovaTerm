#pragma once
#include "EvidenceTypes.h"

namespace NovaTerm::Evidence {
enum class AuthorizationPurpose { Admission, Delivery, CrossPrincipal };
// Must recheck the live grant and token generation, plus EVERY fragment on every invocation.
// CrossPrincipal is a separate explicit sharing grant; an ID is never a grant.
using EvidenceAuthorization = std::function<bool(const ResourceContext&, const ResourceBinding&,
                                                const QVector<RawFragment>&, AuthorizationPurpose)>;
}
