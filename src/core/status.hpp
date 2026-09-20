// Machine-readable run status.
//
// The log answers "why", this file answers "did it work" without a human having
// to read prose.  It is rewritten in full on every change (a handful of writes
// per run), so a test script can just parse `key=value` lines.
#pragma once

#include <string>

namespace cabbird {

void StatusOpen(const std::wstring& dll_dir, const std::wstring& file_name);
void StatusSet(const std::wstring& key, const std::wstring& value);
void StatusSet(const std::wstring& key, const char* value);
void StatusSetInt(const std::wstring& key, long long value);
void StatusClose();

}  // namespace cabbird
