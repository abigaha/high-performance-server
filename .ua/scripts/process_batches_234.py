#!/usr/bin/env python3
"""批处理 2、3、4：为知识图谱生成生成节点和边"""
import json, os, sys

UA_DIR = "/home/jmj/work-space/projects/high-performance-server/.ua"

# batch mapping: user batch -> batches.json batchIndex
BATCH_MAP = {2: 3, 3: 4, 4: 5}

def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

def process_batch(batch_idx):
    print(f"\n{'='*60}")
    print(f"处理批次 {batch_idx}")
    print(f"{'='*60}")

    extract = load_json(f"{UA_DIR}/tmp/ua-file-extract-results-{batch_idx}.json")
    analyzer = load_json(f"{UA_DIR}/tmp/ua-file-analyzer-input-{batch_idx}.json")
    batches_all = load_json(f"{UA_DIR}/intermediate/batches.json")

    real_batch_idx = BATCH_MAP[batch_idx]
    batch_meta = [b for b in batches_all["batches"] if b["batchIndex"] == real_batch_idx][0]
    batch_import_data = batch_meta["batchImportData"]
    exports_by_path = batches_all["exportsByPath"]

    results_by_path = {r["path"]: r for r in extract["results"]}
    files_info = {f["path"]: f for f in analyzer["batchFiles"]}

    nodes = []
    edges = []
    stats = {"files": 0, "classes": 0, "functions": 0, "edges_contains": 0, "edges_imports": 0}

    for file_path in batch_import_data:
        rel_path = file_path
        finfo = files_info.get(rel_path, {})
        result = results_by_path.get(rel_path, {})

        lang = finfo.get("language", result.get("language", "unknown"))
        lines = finfo.get("sizeLines", result.get("totalLines", 0))
        category = finfo.get("fileCategory", "code")
        exports = exports_by_path.get(rel_path, [])

        file_id = f"file:{rel_path}"

        # file node
        nodes.append({
            "id": file_id,
            "type": "file",
            "name": rel_path.split("/")[-1],
            "path": rel_path,
            "language": lang,
            "sizeLines": lines,
            "category": category
        })
        stats["files"] += 1

        seen_symbols = set()

        # class nodes
        for cls in result.get("classes", []):
            cls_name = cls["name"]
            cls_id = f"class:{rel_path}::{cls_name}"
            if cls_id in seen_symbols:
                continue
            seen_symbols.add(cls_id)
            nodes.append({
                "id": cls_id,
                "type": "class",
                "name": cls_name,
                "file": rel_path,
                "startLine": cls["startLine"],
                "endLine": cls["endLine"],
                "methods": cls.get("methods", []),
                "properties": cls.get("properties", [])
            })
            stats["classes"] += 1
            # contains edge: file -> class
            edges.append({
                "source": file_id,
                "target": cls_id,
                "type": "contains"
            })
            stats["edges_contains"] += 1

        # function nodes (from extract results functions list)
        for func in result.get("functions", []):
            func_name = func["name"]
            func_id = f"func:{rel_path}::{func_name}"
            if func_id in seen_symbols:
                continue
            seen_symbols.add(func_id)
            nodes.append({
                "id": func_id,
                "type": "function",
                "name": func_name,
                "file": rel_path,
                "startLine": func["startLine"],
                "endLine": func["endLine"],
                "params": func.get("params", [])
            })
            stats["functions"] += 1
            # contains edge: file -> function
            edges.append({
                "source": file_id,
                "target": func_id,
                "type": "contains"
            })
            stats["edges_contains"] += 1

        # also significant functions from exports that aren't covered by functions list
        for exp_name in exports:
            exp_id = f"func:{rel_path}::{exp_name}"
            if exp_id not in seen_symbols and not exp_name.startswith("~"):
                # check if it's a class name (already covered)
                is_class = any(cls["name"] == exp_name for cls in result.get("classes", []))
                if is_class:
                    continue
                nodes.append({
                    "id": exp_id,
                    "type": "function",
                    "name": exp_name,
                    "file": rel_path,
                    "startLine": 0,
                    "endLine": 0,
                    "params": []
                })
                stats["functions"] += 1
                edges.append({
                    "source": file_id,
                    "target": exp_id,
                    "type": "contains"
                })
                stats["edges_contains"] += 1
                seen_symbols.add(exp_id)

        # imports edges
        for callee_path in batch_import_data.get(rel_path, []):
            callee_id = f"file:{callee_path}"
            edges.append({
                "source": file_id,
                "target": callee_id,
                "type": "imports"
            })
            stats["edges_imports"] += 1

    batch_output = {
        "batchIndex": batch_idx,
        "language": "zh",
        "nodes": nodes,
        "edges": edges,
        "stats": stats
    }

    out_path = f"{UA_DIR}/intermediate/batch-{batch_idx}.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(batch_output, f, ensure_ascii=False, indent=2)
    print(f"  写入 {out_path}")
    print(f"  节点: {stats['files']} 文件, {stats['classes']} 类, {stats['functions']} 函数 = {len(nodes)} 总计")
    print(f"  边: {stats['edges_contains']} contains, {stats['edges_imports']} imports = {len(edges)} 总计")
    return stats

total = {"files": 0, "classes": 0, "functions": 0, "edges_contains": 0, "edges_imports": 0}
for b in [2, 3, 4]:
    s = process_batch(b)
    for k in total:
        total[k] += s[k]

print(f"\n{'='*60}")
print(f"汇总统计（批次 2+3+4）")
print(f"{'='*60}")
print(f"文件节点:      {total['files']}")
print(f"类节点:        {total['classes']}")
print(f"函数节点:      {total['functions']}")
print(f"contains 边:   {total['edges_contains']}")
print(f"imports 边:    {total['edges_imports']}")
print(f"节点总数:      {total['files'] + total['classes'] + total['functions']}")
print(f"边总数:        {total['edges_contains'] + total['edges_imports']}")
print("完成。")
