// The `unitymem` namespace, declared in one place.
//
// DECIDED (user): the namespace layout is Cabbird's to choose, and this is the choice.
//
// HISTORY, because the name matters and the old one was wrong for this project.  This layer was
// ported from the sister project Anomaly, which is an Unreal Engine 5 title, and Anomaly puts its
// process-memory layer and the service wrappers around it in a single namespace called `ue5mem` --
// a name that says which engine the port came FROM, not which engine the code is FOR.  Cabbird
// targets Unity IL2CPP, so every `ue5mem::` qualifier in the tree was renamed to `unitymem::`.
//
// The rename is mechanical and was done in one pass across 40 files, which is the right way to do
// it.  An earlier attempt had renamed the QUALIFIER without renaming the NAMESPACE that owned it
// and produced 89 + 45 compile errors, because a search-and-replace had invented an API name that
// did not exist.  The lesson is to rename the declaration and every use together, or not at all.
//
// The split Cabbird uses:
//
//     cabbird::mem   the memory layer itself     ModuleInfo, SectionInfo, RegionInfo,
//                                                Pattern, ReadMemory, ScanSection, ...
//     cabbird        the service layer +         ModuleMemoryService, PatternService,
//                    framework-general types     CoreMemoryServices, AnalyzerConfig, Analyzer
//
// `cabbird::mem` is the deliberate, reviewed name for the memory layer; the service layer sits in
// `cabbird` because that is where Cabbird's framework types live, and because Anomaly's own
// service classes are declared in the framework namespace (`namespace anomaly`), not in the memory
// one.  Keeping that division is what makes the alias EXACT: one `unitymem::X` resolves to one
// `cabbird::X`, never ambiguously.
//
// This header is the only place the alias is declared.  Declaring it in two places risks two
// different targets for one name, which is precisely the failure that once produced
// `error C2757: 'ue5mem' : a symbol with this name already exists` -- recorded here under its
// historical spelling, because that is the error a future reader would actually search for.
#pragma once

#include "cabbird/memory.hpp"
#include "cabbird/pattern.hpp"
#include "cabbird/module_memory_service.hpp"
#include "cabbird/pattern_service.hpp"

namespace unitymem = cabbird;
