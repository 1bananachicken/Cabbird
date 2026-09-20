// Brings the process-memory layer's names into `cabbird`, so the service wrappers ported from
// Anomaly can spell them unqualified.
//
// WHY.  Anomaly declares the memory layer (`ModuleInfo`, `SectionInfo`, `RegionInfo`, `Pattern`,
// ...) and the service wrappers around it (`ModuleMemoryService`, `PatternService`) in ONE
// namespace, `unitymem`, so the wrappers refer to those types by bare name.  Cabbird separates
// them -- `cabbird::mem` for the layer, `cabbird` for the services and framework types -- which
// is the namespace decision recorded in cabbird/unitymem_compat.hpp.
//
// Unqualified name lookup walks outward from `cabbird`, and `cabbird::mem` is a *deeper*
// namespace, so it is never found on its own: the copied service headers failed with
//     error C2039: 'ModuleInfo': is not a member of 'cabbird'
// This header supplies the one missing step with `using` declarations.  They are ordinary
// aliases of the same types, not second definitions, so `cabbird::ModuleInfo` and
// `cabbird::mem::ModuleInfo` are one type and no overload can become ambiguous.
//
// The service headers include THIS, not `unitymem_compat.hpp`, because that one includes them
// back -- a cycle that produced
//     error C4430: missing type specifier
// and 90 errors in module_memory_service.cpp.
#pragma once

#include "cabbird/memory.hpp"
#include "cabbird/pattern.hpp"

namespace cabbird {

using cabbird::mem::ModuleInfo;
using cabbird::mem::Pattern;
using cabbird::mem::PatternByte;
using cabbird::mem::RegionInfo;
using cabbird::mem::SectionInfo;

}  // namespace cabbird
