#pragma once

#include "cabbird/unitymem_compat.hpp"
#include "cabbird/analyzer.hpp"

#include <stop_token>
#include <string>

namespace cabbird {

std::wstring BuildPipeName(std::wstring_view prefix, unsigned long process_id);
void RunPipeServer(
    const Analyzer& analyzer,
    const std::wstring& pipe_name,
    std::stop_token stop_token);

}  // namespace cabbird
