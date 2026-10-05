#!/usr/bin/env python3
"""Checks the Aether module dependency graph (Phase 37 step 2, docs/design/UPGRADE_PLAN.md).

  1. The CMake link graph between first-party targets (aether_*) has no cycles.
  2. Every link edge goes from a higher layer to a strictly lower one, per docs/design/module_layers.txt
     (a new target must be added there; a new edge must respect the layers).
  3. Every `#include "aether/<x>/..."` in a module's sources and headers names a header owned by a
     module it links to (directly or through what it links).

Usage:
  cmake --graphviz=build/graph.dot <build-dir>            # makes the graph
  tools/check_module_graph.py --dot build/graph.dot        # checks (exit 1 on a problem)
  tools/check_module_graph.py --dot build/graph.dot --update   # rewrites module_layers.txt from the graph
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRST_PARTY = re.compile(r"^aether_")
EDGE = re.compile(r'"node\d+"\s*->\s*"node\d+".*//\s*(\S+)\s*->\s*(\S+)')


def read_edges(dot_path):
    edges = set()
    with open(dot_path, encoding="utf-8") as f:
        for line in f:
            m = EDGE.search(line)
            if m and FIRST_PARTY.match(m.group(1)) and FIRST_PARTY.match(m.group(2)):
                edges.add((m.group(1), m.group(2)))
    return edges


def nodes_of(edges):
    n = set()
    for a, b in edges:
        n.add(a)
        n.add(b)
    return n


def find_cycles(edges):
    """Strongly connected components with more than one node (Tarjan)."""
    graph = {n: [] for n in nodes_of(edges)}
    for a, b in edges:
        graph[a].append(b)
    index, low, on, stack, out, counter = {}, {}, set(), [], [], [0]

    def visit(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        on.add(v)
        for w in graph[v]:
            if w not in index:
                visit(w)
                low[v] = min(low[v], low[w])
            elif w in on:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on.discard(w)
                comp.append(w)
                if w == v:
                    break
            if len(comp) > 1:
                out.append(sorted(comp))

    sys.setrecursionlimit(10000)
    for v in sorted(graph):
        if v not in index:
            visit(v)
    return out


def compute_layers(edges):
    """Layer 0 depends on nothing; a target's layer is one more than the highest layer it links."""
    deps = {n: set() for n in nodes_of(edges)}
    for a, b in edges:
        deps[a].add(b)
    layer = {}

    def depth(n):
        if n not in layer:
            layer[n] = 0 if not deps[n] else 1 + max(depth(d) for d in deps[n])
        return layer[n]

    for n in deps:
        depth(n)
    return layer


def read_layers(path):
    layers = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if line:
                name, num = line.split()
                layers[name] = int(num)
    return layers


def write_layers(path, layers):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Module layers (Phase 37 step 2). A target may link only to targets in a strictly lower layer.\n")
        f.write("# Regenerate with tools/check_module_graph.py --update after an intentional change to the graph.\n")
        for name in sorted(layers, key=lambda n: (layers[n], n)):
            f.write(f"{name} {layers[name]}\n")


def reachable(edges):
    deps = {n: set() for n in nodes_of(edges)}
    for a, b in edges:
        deps[a].add(b)
    out = {}
    for n in deps:
        seen, todo = set(), list(deps[n])
        while todo:
            d = todo.pop()
            if d not in seen:
                seen.add(d)
                todo.extend(deps.get(d, ()))
        out[n] = seen
    return out


INCLUDE = re.compile(r'^\s*#\s*include\s*"aether/([A-Za-z0-9_]+)/')


def check_includes(edges, reach):
    """Which target owns `aether/<x>/`: the top-level directory D whose include/aether/<x> exists, target aether_D."""
    owner = {}
    top_dirs = []
    for d in sorted(os.listdir(ROOT)):
        inc = os.path.join(ROOT, d, "include", "aether")
        if os.path.isdir(inc):
            top_dirs.append(d)
            for sub in os.listdir(inc):
                if os.path.isdir(os.path.join(inc, sub)):
                    owner.setdefault(sub, "aether_" + d)
    problems = []
    for d in top_dirs:
        me = "aether_" + d
        if me not in reach:
            continue
        allowed = reach[me] | {me}
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            for fn in files:
                if not fn.endswith((".h", ".cpp", ".hpp", ".cc")):
                    continue
                path = os.path.join(base, fn)
                with open(path, encoding="utf-8", errors="replace") as f:
                    for n, line in enumerate(f, 1):
                        m = INCLUDE.match(line)
                        # A module this configuration doesn't build (e.g. physics when it is off) isn't in the graph: skip it.
                        if m and m.group(1) in owner and owner[m.group(1)] in reach and owner[m.group(1)] not in allowed:
                            problems.append(f"{os.path.relpath(path, ROOT)}:{n}: includes aether/{m.group(1)}/ (owned by {owner[m.group(1)]}), which {me} doesn't link")
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dot", required=True, help="the file from cmake --graphviz")
    ap.add_argument("--layers", default=os.path.join(ROOT, "docs", "design", "module_layers.txt"))
    ap.add_argument("--update", action="store_true", help="rewrite the layers file from the graph")
    ap.add_argument("--no-includes", action="store_true", help="skip the #include scan")
    args = ap.parse_args()

    edges = read_edges(args.dot)
    if not edges:
        print("check_module_graph: no first-party edges found in", args.dot)
        return 1
    cycles = find_cycles(edges)
    problems = [f"dependency cycle: {' <-> '.join(c)}" for c in cycles]
    if args.update:
        if cycles:
            print("\n".join(problems))
            return 1
        write_layers(args.layers, compute_layers(edges))
        print("wrote", args.layers)
        return 0
    layers = read_layers(args.layers)
    for n in sorted(nodes_of(edges)):
        if n not in layers:
            problems.append(f"{n} isn't in {os.path.relpath(args.layers, ROOT)} (run with --update if the new target is intended)")
    for a, b in sorted(edges):
        if a in layers and b in layers and layers[a] <= layers[b]:
            problems.append(f"{a} (layer {layers[a]}) links {b} (layer {layers[b]}): a target may link only to a lower layer")
    if not args.no_includes:
        problems += check_includes(edges, reachable(edges))
    for p in problems:
        print("check_module_graph:", p)
    if problems:
        print(f"check_module_graph: {len(problems)} problem(s)")
        return 1
    print(f"check_module_graph: ok ({len(nodes_of(edges))} targets, {len(edges)} edges, no cycles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
