#pragma once
#include "EvidenceTypes.h"
namespace NovaTerm::Evidence {
class EvidenceSourceReader {
public:
    virtual ~EvidenceSourceReader() = default;
    virtual bool available(const Source&) const { return true; }
    virtual bool authorized(const Source&,const Context&) const = 0;
    virtual void read(const Source&,const Context&,Done) = 0;
};
}
