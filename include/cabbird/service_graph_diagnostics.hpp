/* cabbird/service_graph_diagnostics.hpp -- the graph as a JSON artifact.
 *
 * Copied from include/anomaly/service_graph_diagnostics.hpp as part of the UE -> Unity port.
 *
 * The point of serializing the snapshot rather than logging lines: a service graph failure
 * is a SHAPE (this service failed, which made that one fail, and these three never started),
 * and a shape is what a log loses.  The offline regression parses this JSON to assert the
 * same thing the in-process snapshot says, so the two cannot drift apart silently.
 */
#pragma once

#include "cabbird/service_graph.hpp"

#include <string>

namespace cabbird {

[[nodiscard]] std::string SerializeServiceGraphSnapshotJson(
    const ServiceGraphSnapshot& snapshot);

}  // namespace cabbird
