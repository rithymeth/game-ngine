#include "graph/blueprint_graph.h"

#include <algorithm>
#include <set>

namespace aether::editor {

void DeleteNodes(bp::Graph& graph, const std::vector<bp::NodeId>& nodes) {
    const std::set<bp::NodeId> doomed(nodes.begin(), nodes.end());
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
                                     [&](const bp::Node& node) { return doomed.count(node.id) != 0; }),
                      graph.nodes.end());
    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                                     [&](const bp::Link& link) {
                                         return doomed.count(link.from.node) || doomed.count(link.to.node);
                                     }),
                      graph.links.end());
}

} // namespace aether::editor
