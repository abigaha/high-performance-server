#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");

const NODE_TYPES = new Set([
  "file", "function", "class", "module", "concept",
  "config", "document", "service", "table", "endpoint",
  "pipeline", "schema", "resource", "domain", "flow", "step",
]);

const NODE_PREFIXES = new Set(NODE_TYPES);

const EDGE_TYPES = new Set([
  "imports", "exports", "contains", "inherits", "implements",
  "calls", "subscribes", "publishes", "middleware",
  "reads_from", "writes_to", "transforms", "validates",
  "depends_on", "tested_by", "configures",
  "related", "similar_to",
  "deploys", "serves", "provisions", "triggers",
  "migrates", "documents", "routes", "defines_schema",
  "contains_flow", "flow_step", "cross_domain",
]);

const DIRECTIONS = new Set(["forward", "backward", "bidirectional"]);
const COMPLEXITY = new Set(["simple", "moderate", "complex"]);

// file-level types that must be assigned to exactly one layer
const FILE_LEVEL_TYPES = new Set([
  "file", "config", "document", "service", "pipeline", "table", "schema", "resource", "endpoint",
]);

// node type -> expected ID prefix (with special-case for table/endpoint extra segments)
const TYPE_TO_PREFIX = {
  file: "file", function: "function", class: "class", module: "module",
  concept: "concept", config: "config", document: "document",
  service: "service", table: "table", endpoint: "endpoint",
  pipeline: "pipeline", schema: "schema", resource: "resource",
  domain: "domain", flow: "flow", step: "step",
};

// Check 8: non-code node expected incoming/outgoing edge types
const EXPECTED_EDGES_BY_TYPE = {
  document: ["documents"],
  service: ["deploys", "depends_on"],
  pipeline: ["triggers"],
  table: ["migrates", "defines_schema"],
  schema: ["defines_schema"],
};

function normalizePath(p) {
  if (!p) return p;
  let s = String(p).replace(/\\/g, "/");
  while (s.startsWith("./")) s = s.slice(2);
  return s;
}

function main() {
  const graphPath = process.argv[2];
  const outPath = process.argv[3];
  const scanPath = process.argv[4] || "/home/jmj/work-space/projects/high-performance-server/.ua/intermediate/scan-result.json";

  const raw = fs.readFileSync(graphPath, "utf8");
  const graph = JSON.parse(raw);
  const issues = [];
  const warnings = [];

  const nodes = Array.isArray(graph.nodes) ? graph.nodes : [];
  const edges = Array.isArray(graph.edges) ? graph.edges : [];
  const layers = Array.isArray(graph.layers) ? graph.layers : [];
  const tour = Array.isArray(graph.tour) ? graph.tour : [];

  const stats = {
    totalNodes: nodes.length,
    totalEdges: edges.length,
    totalLayers: layers.length,
    tourSteps: tour.length,
    nodeTypes: {},
    edgeTypes: {},
  };

  for (const n of nodes) {
    stats.nodeTypes[n.type] = (stats.nodeTypes[n.type] || 0) + 1;
  }
  for (const e of edges) {
    stats.edgeTypes[e.type] = (stats.edgeTypes[e.type] || 0) + 1;
  }

  // ---------- Check 1: pattern validation (critical) ----------
  const nodeIds = new Set();
  const nodeById = new Map();
  nodes.forEach((n, i) => {
    if (!n || typeof n !== "object") {
      issues.push(`Check1[节点索引 ${i}] 节点不是对象`);
      return;
    }
    const label = `节点[${n.id || `索引${i}`}]`;

    if (typeof n.id !== "string" || n.id.trim() === "") {
      issues.push(`Check1 ${label} 缺少非空 id`);
    } else {
      const prefix = n.id.split(":")[0];
      if (!NODE_PREFIXES.has(prefix)) {
        issues.push(`Check1 ${label} id 前缀非法: "${prefix}"`);
      }
      nodeIds.add(n.id);
      nodeById.set(n.id, n);
    }

    if (!NODE_TYPES.has(n.type)) {
      issues.push(`Check1 ${label} type 非法: "${n.type}"`);
    }

    if (typeof n.name !== "string" || n.name.trim() === "") {
      issues.push(`Check1 ${label} 缺少非空 name`);
    }

    if (typeof n.summary !== "string" || n.summary.trim() === "") {
      issues.push(`Check1 ${label} 缺少非空 summary`);
    }

    if (!Array.isArray(n.tags) || n.tags.length === 0) {
      issues.push(`Check1 ${label} tags 为空或非数组`);
    } else {
      for (const t of n.tags) {
        if (typeof t !== "string" || !/^[a-z0-9-]+$/.test(t)) {
          issues.push(`Check1 ${label} tag 非法(需全小写连字符): "${t}"`);
        }
      }
    }

    if (!COMPLEXITY.has(n.complexity)) {
      issues.push(`Check1 ${label} complexity 非法: "${n.complexity}"`);
    }
  });

  edges.forEach((e, i) => {
    if (!e || typeof e !== "object") {
      issues.push(`Check1[边索引 ${i}] 边不是对象`);
      return;
    }
    const label = `边[${e.source}->${e.target} (${e.type})]`;
    if (typeof e.source !== "string" || e.source.trim() === "") {
      issues.push(`Check1 ${label} 缺少非空 source`);
    }
    if (typeof e.target !== "string" || e.target.trim() === "") {
      issues.push(`Check1 ${label} 缺少非空 target`);
    }
    if (!EDGE_TYPES.has(e.type)) {
      issues.push(`Check1 ${label} type 非法: "${e.type}"`);
    }
    if (!DIRECTIONS.has(e.direction)) {
      issues.push(`Check1 ${label} direction 非法: "${e.direction}"`);
    }
    if (typeof e.weight !== "number" || e.weight < 0 || e.weight > 1 || Number.isNaN(e.weight)) {
      issues.push(`Check1 ${label} weight 越界(需 0.0-1.0): "${e.weight}"`);
    }
  });

  // ---------- Check 2: reference integrity (critical) ----------
  edges.forEach((e, i) => {
    if (e && e.source && !nodeIds.has(e.source)) {
      issues.push(`Check2[边索引 ${i}] 悬空 source: "${e.source}"`);
    }
    if (e && e.target && !nodeIds.has(e.target)) {
      issues.push(`Check2[边索引 ${i}] 悬空 target: "${e.target}"`);
    }
  });

  layers.forEach((layer, li) => {
    if (!layer || typeof layer !== "object") {
      issues.push(`Check2[层索引 ${li}] 层不是对象`);
      return;
    }
    const nodeList = Array.isArray(layer.nodeIds) ? layer.nodeIds : [];
    nodeList.forEach((nid, ni) => {
      if (!nodeIds.has(nid)) {
        issues.push(`Check2[层 "${layer.id || li}" 索引 ${ni}] 悬空 nodeId: "${nid}"`);
      }
    });
  });

  tour.forEach((step, si) => {
    if (!step || typeof step !== "object") {
      issues.push(`Check2[导览步索引 ${si}] 步不是对象`);
      return;
    }
    const nodeList = Array.isArray(step.nodeIds) ? step.nodeIds : [];
    nodeList.forEach((nid, ni) => {
      if (!nodeIds.has(nid)) {
        issues.push(`Check2[导览步 ${step.order || si} 索引 ${ni}] 悬空 nodeId: "${nid}"`);
      }
    });
  });

  // ---------- Check 3: completeness (critical, relaxed for domain graphs) ----------
  const hasDomain = nodes.some((n) => n && ["domain", "flow", "step"].includes(n.type));
  if (nodes.length === 0) issues.push("Check3 图谱节点数为 0");
  if (edges.length === 0) issues.push("Check3 图谱边数为 0");
  if (layers.length === 0) {
    if (hasDomain) warnings.push("Check3 域图谱无层(layers=0)，已放宽为警告");
    else issues.push("Check3 图谱层数为 0");
  }
  if (tour.length === 0) {
    if (hasDomain) warnings.push("Check3 域图谱无导览(tour=0)，已放宽为警告");
    else issues.push("Check3 图谱导览步数为 0");
  }

  // ---------- Check 4: layer coverage (critical) ----------
  const layerNodeCounts = new Map(); // nodeId -> count of layers containing it
  layers.forEach((layer) => {
    if (layer && Array.isArray(layer.nodeIds) && layer.nodeIds.length === 0) {
      issues.push(`Check4 层 "${layer.id || "?"}" 的 nodeIds 为空`);
    }
    if (layer && Array.isArray(layer.nodeIds)) {
      for (const nid of layer.nodeIds) {
        layerNodeCounts.set(nid, (layerNodeCounts.get(nid) || 0) + 1);
      }
    }
  });

  const fileLevelNodes = nodes.filter((n) => n && FILE_LEVEL_TYPES.has(n.type));
  for (const n of fileLevelNodes) {
    const count = layerNodeCounts.get(n.id) || 0;
    if (count === 0) {
      issues.push(`Check4 ${n.type} 节点 "${n.id}" 未出现在任何层的 nodeIds 中`);
    } else if (count > 1) {
      issues.push(`Check4 ${n.type} 节点 "${n.id}" 出现在 ${count} 个层中（要求恰好 1 个）`);
    }
  }

  // ---------- Check 5: uniqueness (critical) ----------
  if (nodeIds.size !== nodes.length) {
    const seen = new Map();
    nodes.forEach((n) => {
      if (n && typeof n.id === "string") seen.set(n.id, (seen.get(n.id) || 0) + 1);
    });
    for (const [id, c] of seen) {
      if (c > 1) issues.push(`Check5 重复节点 ID: "${id}" (×${c})`);
    }
  }

  // ---------- Check 6: tour validation (warning) ----------
  if (tour.length > 0) {
    const orders = new Set();
    let prev = 0;
    let orderOk = true;
    for (let si = 0; si < tour.length; si++) {
      const step = tour[si];
      if (!step || typeof step !== "object") { orderOk = false; continue; }
      if (orders.has(step.order)) {
        warnings.push(`Check6 导览步 order 重复: ${step.order}`);
        orderOk = false;
      }
      orders.add(step.order);
      if (si > 0 && step.order !== prev + 1) {
        warnings.push(`Check6 导览步 order 非顺序递增(第 ${si} 步): ${step.order}（前一为 ${prev}）`);
        orderOk = false;
      }
      prev = step.order;
      if (!Array.isArray(step.nodeIds) || step.nodeIds.length === 0) {
        warnings.push(`Check6 导览步 ${step.order} 的 nodeIds 为空`);
      }
    }
    if (tour.length < 5) warnings.push(`Check6 导览总步数 ${tour.length} < 5（推荐 5-15）`);
    if (tour.length > 15) warnings.push(`Check6 导览总步数 ${tour.length} > 15（推荐 5-15）`);
  }

  // ---------- Check 7: quality (warning) ----------
  for (const n of nodes) {
    if (!n || typeof n !== "object") continue;
    const label = `节点[${n.id || "?"}]`;
    if (typeof n.summary === "string") {
      const s = n.summary.trim();
      if (s === "") {
        warnings.push(`Check7 ${label} summary 为空`);
      } else {
        const base = n.name ? String(n.name).toLowerCase() : "";
        if (s.toLowerCase() === base) {
          warnings.push(`Check7 ${label} summary 只复述节点名`);
        }
      }
      if (s === "暂无摘要") {
        warnings.push(`Check7 ${label} summary 为占位符"暂无摘要"`);
      }
      if (n.filePath && s.toLowerCase() === normalizePath(n.filePath).toLowerCase()) {
        warnings.push(`Check7 ${label} summary 只复述文件名`);
      }
    }
    if (Array.isArray(n.tags) && n.tags.length === 1 && n.tags[0] === "untagged") {
      warnings.push(`Check7 ${label} tags 为占位符["untagged"]`);
    }
  }

  edges.forEach((e, i) => {
    if (e && e.source && e.target && e.source === e.target) {
      warnings.push(`Check7[边索引 ${i}] 自引用边: ${e.source}->${e.target} (${e.type})`);
    }
  });

  // orphan nodes: no edges at all, and not referenced by layers/tour
  const referenced = new Set();
  for (const e of edges) {
    if (e && e.source) referenced.add(e.source);
    if (e && e.target) referenced.add(e.target);
  }
  for (const layer of layers) {
    if (layer && Array.isArray(layer.nodeIds)) for (const nid of layer.nodeIds) referenced.add(nid);
  }
  for (const step of tour) {
    if (step && Array.isArray(step.nodeIds)) for (const nid of step.nodeIds) referenced.add(nid);
  }
  let orphanCount = 0;
  for (const n of nodes) {
    if (n && typeof n.id === "string" && !referenced.has(n.id)) {
      orphanCount++;
      if (orphanCount <= 20) warnings.push(`Check7 孤儿节点(无边无层无导览): "${n.id}"`);
    }
  }
  if (orphanCount > 20) warnings.push(`Check7 孤儿节点共 ${orphanCount} 个（已省略明细）`);

  // ---------- Check 8: non-code node quality (warning) ----------
  const edgeTypesByNode = new Map(); // nodeId -> Set of edge types touching it
  for (const e of edges) {
    if (!e) continue;
    if (e.source) {
      if (!edgeTypesByNode.has(e.source)) edgeTypesByNode.set(e.source, new Set());
      edgeTypesByNode.get(e.source).add(e.type);
    }
    if (e.target) {
      if (!edgeTypesByNode.has(e.target)) edgeTypesByNode.set(e.target, new Set());
      edgeTypesByNode.get(e.target).add(e.type);
    }
  }
  for (const n of nodes) {
    if (!n || !EXPECTED_EDGES_BY_TYPE[n.type]) continue;
    const have = edgeTypesByNode.get(n.id) || new Set();
    const expected = EXPECTED_EDGES_BY_TYPE[n.type];
    const missing = expected.filter((t) => !have.has(t));
    if (missing.length === expected.length) {
      warnings.push(`Check8 ${n.type} 节点 "${n.id}" 缺少预期边类型 ${expected.join("/")}（无任何连接）`);
    } else if (missing.length > 0) {
      warnings.push(`Check8 ${n.type} 节点 "${n.id}" 缺少预期边类型之一: ${missing.join("/")}`);
    }
  }

  // ---------- Check 9: type/prefix consistency (warning) ----------
  for (const n of nodes) {
    if (!n || typeof n.id !== "string" || !NODE_TYPES.has(n.type)) continue;
    const prefix = n.id.split(":")[0];
    if (prefix !== TYPE_TO_PREFIX[n.type]) {
      warnings.push(`Check9 ${n.type} 节点 "${n.id}" 的 id 前缀 "${prefix}" 与 type 不一致`);
    }
  }

  // ---------- Check 10: scan manifest cross-check (warning) ----------
  let manifestPaths = [];
  try {
    if (fs.existsSync(scanPath)) {
      const scan = JSON.parse(fs.readFileSync(scanPath, "utf8"));
      if (Array.isArray(scan.files)) {
        manifestPaths = scan.files
          .map((f) => normalizePath(f && f.path))
          .filter(Boolean);
      }
    }
  } catch (err) {
    warnings.push(`Check10 无法读取扫描清单: ${err.message}`);
  }

  if (manifestPaths.length > 0) {
    const manifestSet = new Set(manifestPaths);
    const nodeFilePaths = new Set();
    for (const n of nodes) {
      if (n && n.filePath) nodeFilePaths.add(normalizePath(n.filePath));
    }

    let missingFromGraph = 0;
    for (const mp of manifestSet) {
      if (!nodeFilePaths.has(mp)) {
        missingFromGraph++;
        if (missingFromGraph <= 20) warnings.push(`Check10 清单文件无对应节点: "${mp}"`);
      }
    }
    if (missingFromGraph > 20) warnings.push(`Check10 清单中无对应节点的文件共 ${missingFromGraph} 个（已省略明细）`);

    let notInManifest = 0;
    for (const fp of nodeFilePaths) {
      if (!manifestSet.has(fp)) {
        notInManifest++;
        if (notInManifest <= 20) warnings.push(`Check10 图中文件不在扫描清单内: "${fp}"`);
      }
    }
    if (notInManifest > 20) warnings.push(`Check10 图中不在清单内的文件共 ${notInManifest} 个（已省略明细）`);
  }

  return {
    scriptCompleted: true,
    issues,
    warnings,
    stats,
  };
}

let result;
try {
  result = main();
} catch (err) {
  process.stderr.write(`脚本崩溃: ${err.stack || err.message}\n`);
  process.exit(1);
}

const outPath = process.argv[3];
if (outPath) {
  fs.writeFileSync(outPath, JSON.stringify(result, null, 2), "utf8");
} else {
  process.stdout.write(JSON.stringify(result, null, 2));
}
process.stdout.write(
  `VALIDATION: issues=${result.issues.length} warnings=${result.warnings.length} nodes=${result.stats.totalNodes} edges=${result.stats.totalEdges}\n`
);
process.exit(0);
