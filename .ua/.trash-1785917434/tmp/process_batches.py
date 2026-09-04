#!/usr/bin/env python3
"""生成 batch 知识图谱节点和边。"""

import json
import os

BASE = "/home/jmj/work-space/projects/high-performance-server"
TMP = os.path.join(BASE, ".ua/tmp")

def load_json(path):
    with open(path, "r") as f:
        return json.load(f)

def get_file_id(filepath):
    return f"file:{filepath}"

def get_class_id(filepath, class_name):
    return f"class:{filepath}::{class_name}"

def get_func_id(filepath, func_name):
    return f"func:{filepath}::{func_name}"

def process_batch(batch_index):
    extract = load_json(os.path.join(TMP, f"ua-file-extract-results-{batch_index}.json"))
    analyzer = load_json(os.path.join(TMP, f"ua-file-analyzer-input-{batch_index}.json"))

    batch_files = {f["path"]: f for f in analyzer["batchFiles"]}
    import_data = analyzer.get("batchImportData", {})

    nodes = []
    edges = []

    file_node_ids = set()
    class_node_ids = set()
    func_node_ids = set()

    for result in extract["results"]:
        path = result["path"]
        meta = batch_files.get(path, {})
        lang = result.get("language", meta.get("language", "unknown"))
        category = result.get("fileCategory", meta.get("fileCategory", "code"))

        file_id = get_file_id(path)
        if file_id not in file_node_ids:
            nodes.append({
                "id": file_id,
                "type": "file",
                "label": f"文件: {path}",
                "properties": {
                    "path": path,
                    "language": lang,
                    "category": category,
                    "totalLines": result.get("totalLines", meta.get("sizeLines", 0)),
                    "nonEmptyLines": result.get("nonEmptyLines", 0)
                }
            })
            file_node_ids.add(file_id)

        classes = result.get("classes", [])
        for cls in classes:
            class_name = cls["name"]
            cid = get_class_id(path, class_name)
            if cid not in class_node_ids:
                methods = cls.get("methods", [])
                props = cls.get("properties", [])
                nodes.append({
                    "id": cid,
                    "type": "class",
                    "label": f"类: {class_name}",
                    "properties": {
                        "name": class_name,
                        "startLine": cls.get("startLine"),
                        "endLine": cls.get("endLine"),
                        "methodCount": len(methods),
                        "propertyCount": len(props)
                    }
                })
                class_node_ids.add(cid)
                edges.append({
                    "source": file_id,
                    "target": cid,
                    "relation": "contains",
                    "properties": {
                        "kind": "file_contains_class"
                    }
                })

            # methods -> func nodes
            for m in methods:
                fid = get_func_id(path, m)
                if fid not in func_node_ids:
                    nodes.append({
                        "id": fid,
                        "type": "function",
                        "label": f"函数: {m}",
                        "properties": {
                            "name": m,
                            "file": path,
                            "isMethod": True
                        }
                    })
                    func_node_ids.add(fid)
                edges.append({
                    "source": cid,
                    "target": fid,
                    "relation": "contains",
                    "properties": {
                        "kind": "class_contains_method"
                    }
                })

        functions = result.get("functions", [])
        for func in functions:
            func_name = func["name"]
            fid = get_func_id(path, func_name)
            if fid not in func_node_ids:
                nodes.append({
                    "id": fid,
                    "type": "function",
                    "label": f"函数: {func_name}",
                    "properties": {
                        "name": func_name,
                        "file": path,
                        "startLine": func.get("startLine"),
                        "endLine": func.get("endLine"),
                        "params": func.get("params", []),
                        "isMethod": False
                    }
                })
                func_node_ids.add(fid)

            # Check if already has class->func edge; if not, add file->func
            already_has_class_edge = False
            for e in edges:
                if e.get("relation") == "contains" and e.get("target") == fid and e.get("source", "").startswith("class:"):
                    already_has_class_edge = True
                    break
            if not already_has_class_edge:
                edges.append({
                    "source": file_id,
                    "target": fid,
                    "relation": "contains",
                    "properties": {
                        "kind": "file_contains_function"
                    }
                })

        # Include edges from import data
        imports = import_data.get(path, [])
        for imp in imports:
            edges.append({
                "source": file_id,
                "target": get_file_id(imp),
                "relation": "include",
                "properties": {
                    "kind": "file_include_file"
                }
            })

        # Call edges from callGraph
        call_graph = result.get("callGraph", [])
        for call in call_graph:
            caller = call["caller"]
            callee = call["callee"]
            caller_id = get_func_id(path, caller)
            callee_id = get_func_id(path, callee)

            # Only emit if callee is also in this file (project-internal calls)
            # Cross-file calls need more sophisticated resolution
            if callee_id in func_node_ids or any(
                f["name"] == callee for f in functions + classes
            ):
                edges.append({
                    "source": caller_id,
                    "target": callee_id,
                    "relation": "calls",
                    "properties": {
                        "lineNumber": call.get("lineNumber")
                    }
                })

        # Export edges: file declares/defines exports
        exports = result.get("exports", [])
        for exp in exports:
            exp_name = exp["name"]
            # Could be class or function
            cid = get_class_id(path, exp_name)
            fid = get_func_id(path, exp_name)
            target = None
            if cid in class_node_ids:
                target = cid
            elif fid in func_node_ids:
                target = fid
            if target:
                edges.append({
                    "source": file_id,
                    "target": target,
                    "relation": "declares",
                    "properties": {
                        "line": exp.get("line"),
                        "isDefault": exp.get("isDefault", False)
                    }
                })

    return {
        "batchIndex": batch_index,
        "algorithm": "louvain",
        "totalFiles": len(batch_files),
        "nodes": nodes,
        "edges": edges
    }


def main():
    for batch in [10, 11, 12, 13]:
        print(f"正在处理 batch {batch}...")
        data = process_batch(batch)
        outpath = os.path.join(TMP, f"batch-{batch}.json")
        with open(outpath, "w") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        print(f"  -> 已写入 {outpath}")
        print(f"     nodes: {len(data['nodes'])}, edges: {len(data['edges'])}")

if __name__ == "__main__":
    main()
