#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIGRATOR="$PROJECT_ROOT/benchmark/tools/migrate_legacy_reports.py"
TEST_TMP_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "$TEST_TMP_DIR"
}
trap cleanup EXIT

failures=0

fail() {
  printf '失败: %s\n' "$*" >&2
  failures=$((failures + 1))
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local description="$3"
  [ "$expected" = "$actual" ] || fail "$description，期望: [$expected]，实际: [$actual]"
}

assert_file() {
  local path="$1"
  local description="$2"
  [ -s "$path" ] || fail "$description: $path"
}

assert_missing() {
  local path="$1"
  local description="$2"
  [ ! -e "$path" ] && [ ! -L "$path" ] || fail "$description: $path"
}

sha256() {
  /usr/bin/sha256sum "$1" | awk '{print $1}'
}

file_size() {
  stat -c '%s' "$1"
}

source_dir="$TEST_TMP_DIR/source"
destination_dir="$TEST_TMP_DIR/report"
mkdir -p "$source_dir"

micro_source="$source_dir/micro_20260803_120000.txt"
qps_source="$source_dir/qps_20260803_120001.txt"
ambiguous_qps_source="$source_dir/qps_20260803_120002.txt"
load_source="$source_dir/load_20260803_120003.txt"
headerless_micro_source="$source_dir/micro_20260803_120004.txt"
metadata_like_micro_source="$source_dir/micro_20260803_120005.txt"
normalized_micro_source="$source_dir/micro_release_20260803_120006.txt"

cat > "$micro_source" <<'EOF'
==========================================
  微基准测试报告
  时间: 20260803_120000
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
==========================================

--- bench_chunk_header ---
micro chunk header bytes
--- bench_coroutine ---
micro coroutine bytes
EOF
cat > "$qps_source" <<'EOF'
==========================================
  QPS + 压力测试报告
  时间: 20260803_120001
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
==========================================

--- qps_chunk_header ---
qps chunk header bytes
EOF
cat > "$ambiguous_qps_source" <<'EOF'
==========================================
  QPS + 压力测试报告
  时间: 20260803_120002
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
==========================================

unsectioned historical QPS bytes
--- qps_chunk_header ---
late section must not discard the earlier bytes
EOF
cat > "$load_source" <<'EOF'
Legacy load output is intentionally not parsed.
EOF
cat > "$headerless_micro_source" <<'EOF'
--- bench_chunk_header ---
headerless measured bytes must not be discarded
EOF
cat > "$metadata_like_micro_source" <<'EOF'
时间: measured data is not a legacy metadata header
--- bench_chunk_header ---
metadata-like preamble must not be discarded
EOF
cat > "$normalized_micro_source" <<'EOF'
=======================================================
  微基准测试报告（Release 模式）
  时间: 20260803_120006
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
=======================================================

--- bench_bench_chunk_header ---
normalized historical target bytes
EOF

declare -A source_hashes=()
declare -A source_sizes=()
for source in "$micro_source" "$qps_source" "$ambiguous_qps_source" "$load_source" \
  "$headerless_micro_source" "$metadata_like_micro_source" "$normalized_micro_source"; do
  source_hashes["$source"]="$(sha256 "$source")"
  source_sizes["$source"]="$(file_size "$source")"
done

if python3 "$MIGRATOR" --source "$source_dir" --destination "$destination_dir" > "$TEST_TMP_DIR/dry-run.log" 2>&1; then
  :
else
  fail '默认 dry-run 应成功'
fi
assert_missing "$destination_dir" 'dry-run 不得创建目标目录'
for source in "$micro_source" "$qps_source" "$ambiguous_qps_source" "$load_source" \
  "$headerless_micro_source" "$metadata_like_micro_source" "$normalized_micro_source"; do
  assert_equals "${source_hashes[$source]}" "$(sha256 "$source")" "dry-run 后原件 SHA-256"
  assert_equals "${source_sizes[$source]}" "$(file_size "$source")" "dry-run 后原件大小"
done

if python3 "$MIGRATOR" --source "$source_dir" --destination "$destination_dir" --apply --no-clobber \
  > "$TEST_TMP_DIR/apply.log" 2>&1; then
  :
else
  fail '显式 apply 应成功'
fi

micro_chunk="$destination_dir/micro/bench_chunk_header/20260803_120000/legacy_micro_20260803_120000.txt"
micro_coroutine="$destination_dir/micro/bench_coroutine/20260803_120000/legacy_micro_20260803_120000.txt"
qps_chunk="$destination_dir/qps/qps_chunk_header/20260803_120001/legacy_qps_20260803_120001.txt"
ambiguous_qps="$destination_dir/qps/_legacy_aggregate/20260803_120002/legacy_qps_20260803_120002.txt"
load_aggregate="$destination_dir/rps/_legacy_aggregate/20260803_120003/legacy_load_20260803_120003.txt"
headerless_micro_aggregate="$destination_dir/micro/_legacy_aggregate/20260803_120004/legacy_micro_20260803_120004.txt"
metadata_like_micro_aggregate="$destination_dir/micro/_legacy_aggregate/20260803_120005/legacy_micro_20260803_120005.txt"
normalized_micro_chunk="$destination_dir/micro/bench_chunk_header/20260803_120006/legacy_micro_release_20260803_120006.txt"

assert_file "$micro_chunk" '可唯一归属的微基准段'
assert_file "$micro_coroutine" '第二个可唯一归属的微基准段'
assert_file "$qps_chunk" '可唯一归属的 QPS 段'
assert_file "$ambiguous_qps" '歧义 QPS 必须聚合保留'
assert_file "$load_aggregate" 'load_* 必须聚合保留'
assert_file "$headerless_micro_aggregate" '无历史元数据头的微基准必须聚合保留'
assert_file "$metadata_like_micro_aggregate" '类似元数据的前导测量数据必须聚合保留'
assert_file "$normalized_micro_chunk" 'bench_bench_* 标题必须兼容映射到 bench_* 目标'

expected_micro_chunk="$TEST_TMP_DIR/expected-micro-chunk.txt"
expected_micro_coroutine="$TEST_TMP_DIR/expected-micro-coroutine.txt"
expected_qps_chunk="$TEST_TMP_DIR/expected-qps-chunk.txt"
expected_normalized_micro_chunk="$TEST_TMP_DIR/expected-normalized-micro-chunk.txt"
cat > "$expected_micro_chunk" <<'EOF'
--- bench_chunk_header ---
micro chunk header bytes
EOF
cat > "$expected_micro_coroutine" <<'EOF'
--- bench_coroutine ---
micro coroutine bytes
EOF
cat > "$expected_qps_chunk" <<'EOF'
--- qps_chunk_header ---
qps chunk header bytes
EOF
cat > "$expected_normalized_micro_chunk" <<'EOF'
--- bench_bench_chunk_header ---
normalized historical target bytes
EOF
assert_equals "$(sha256 "$expected_micro_chunk")" "$(sha256 "$micro_chunk")" '微基准段必须逐字节保留'
assert_equals "$(sha256 "$expected_micro_coroutine")" "$(sha256 "$micro_coroutine")" '第二个微基准段必须逐字节保留'
assert_equals "$(sha256 "$expected_qps_chunk")" "$(sha256 "$qps_chunk")" 'QPS 段必须逐字节保留'
assert_equals "$(sha256 "$expected_normalized_micro_chunk")" "$(sha256 "$normalized_micro_chunk")" \
  'bench_bench_* 归一化段必须逐字节保留'
assert_equals "${source_hashes[$ambiguous_qps_source]}" "$(sha256 "$ambiguous_qps")" '歧义 QPS 必须逐字节聚合保留'
assert_equals "${source_hashes[$load_source]}" "$(sha256 "$load_aggregate")" 'load_* 必须逐字节聚合保留'
assert_equals "${source_hashes[$headerless_micro_source]}" "$(sha256 "$headerless_micro_aggregate")" \
  '无历史元数据头的微基准必须逐字节聚合保留'
assert_equals "${source_hashes[$metadata_like_micro_source]}" "$(sha256 "$metadata_like_micro_aggregate")" \
  '类似元数据的前导测量数据必须逐字节聚合保留'

mapfile -t migration_indexes < <(find "$destination_dir/_legacy_migrations" -maxdepth 1 -type f -name '*.json' | sort)
if [ "${#migration_indexes[@]}" -ne 1 ]; then
  fail 'apply 必须生成唯一迁移清单'
elif ! /usr/bin/python3 - "${migration_indexes[0]}" "$source_dir" "$destination_dir" <<'PY'
import hashlib
import json
import os
import sys

index_path, source_dir, destination_dir = sys.argv[1:]
with open(index_path, encoding="utf-8") as stream:
    index = json.load(stream)
assert index["schema_version"] == 1
assert isinstance(index["migrated_at"], str) and index["migrated_at"]
entries = index["entries"]
assert len(entries) == 8
for entry in entries:
    source_path = os.path.join(source_dir, entry["source_path"])
    destination_path = os.path.join(destination_dir, entry["destination_path"])
    assert os.path.isfile(source_path)
    assert os.path.isfile(destination_path)
    assert entry["source_sha256"] == hashlib.sha256(open(source_path, "rb").read()).hexdigest()
    assert 0 <= entry["byte_start"] < entry["byte_end"] <= os.path.getsize(source_path)
    assert entry["kind"] in {"micro", "qps", "rps"}
PY
then
  fail '迁移清单必须记录来源、摘要、字节范围、目标路径和时间'
fi

before_tree="$(find "$destination_dir" -type f -print0 | sort -z | xargs -0 /usr/bin/sha256sum)"
if python3 "$MIGRATOR" --source "$source_dir" --destination "$destination_dir" --apply --no-clobber \
  > "$TEST_TMP_DIR/no-clobber.log" 2>&1; then
  fail '--no-clobber 遇到已有副本必须失败'
fi
after_tree="$(find "$destination_dir" -type f -print0 | sort -z | xargs -0 /usr/bin/sha256sum)"
assert_equals "$before_tree" "$after_tree" '--no-clobber 失败不得改变已有迁移产物'

create_publish_race_hook() {
  local hook_dir="$1"
  mkdir -p "$hook_dir"
  cat > "$hook_dir/sitecustomize.py" <<'PY'
import os

mode = os.environ["MIGRATOR_RACE_MODE"]
original_link = os.link
raced = False


def destination_path(destination, kwargs):
    directory_fd = kwargs.get("dst_dir_fd")
    if directory_fd is None:
        return os.fspath(destination)
    return os.path.join(os.readlink(f"/proc/self/fd/{directory_fd}"), os.fspath(destination))


def should_race(destination, kwargs):
    normalized = destination_path(destination, kwargs).replace(os.sep, "/")
    if mode == "second-destination":
        return "/micro/bench_coroutine/" in normalized
    return "/_legacy_migrations/" in normalized


def link(source, destination, *args, **kwargs):
    global raced
    if not raced and should_race(destination, kwargs):
        directory_fd = kwargs.get("dst_dir_fd")
        if directory_fd is None:
            with open(destination, "xb") as stream:
                stream.write(b"concurrent publisher\n")
        else:
            publisher_fd = os.open(
                os.fspath(destination), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=directory_fd
            )
            try:
                os.write(publisher_fd, b"concurrent publisher\n")
            finally:
                os.close(publisher_fd)
        raced = True
    return original_link(source, destination, *args, **kwargs)


os.link = link
PY
}

assert_no_clobber_destination_publish_race() {
  local race_source_dir="$TEST_TMP_DIR/race-destination-source"
  local race_destination_dir="$TEST_TMP_DIR/race-destination-report"
  local race_hook_dir="$TEST_TMP_DIR/race-destination-hook"
  local first_destination
  local raced_destination
  mkdir -p "$race_source_dir"
  cat > "$race_source_dir/micro_20260803_120007.txt" <<'EOF'
==========================================
  微基准测试报告
  时间: 20260803_120007
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
==========================================

--- bench_chunk_header ---
first migration payload
--- bench_coroutine ---
second migration payload
EOF
  create_publish_race_hook "$race_hook_dir"

  if PYTHONPATH="$race_hook_dir" MIGRATOR_RACE_MODE=second-destination \
    python3 "$MIGRATOR" --source "$race_source_dir" --destination "$race_destination_dir" --apply --no-clobber \
      > "$TEST_TMP_DIR/race-destination.log" 2>&1; then
    fail '--no-clobber 在发布时遇到后续目标冲突必须失败'
  fi

  first_destination="$race_destination_dir/micro/bench_chunk_header/20260803_120007/legacy_micro_20260803_120007.txt"
  raced_destination="$race_destination_dir/micro/bench_coroutine/20260803_120007/legacy_micro_20260803_120007.txt"
  assert_file "$raced_destination" '并发写入的后续目标必须保留'
  assert_equals 'concurrent publisher' "$(cat "$raced_destination")" '并发写入的后续目标不得被覆盖'
  assert_missing "$first_destination" '后续目标冲突后必须清理本次已发布的迁移文件'
  assert_missing "$race_destination_dir/_legacy_migrations" '后续目标冲突时不得生成迁移清单'
}

assert_no_clobber_index_publish_race() {
  local race_source_dir="$TEST_TMP_DIR/race-index-source"
  local race_destination_dir="$TEST_TMP_DIR/race-index-report"
  local race_hook_dir="$TEST_TMP_DIR/race-index-hook"
  local payload_destination
  local -a raced_indexes=()
  mkdir -p "$race_source_dir"
  cat > "$race_source_dir/micro_20260803_120008.txt" <<'EOF'
==========================================
  微基准测试报告
  时间: 20260803_120008
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
==========================================

--- bench_chunk_header ---
index race migration payload
EOF
  create_publish_race_hook "$race_hook_dir"

  if PYTHONPATH="$race_hook_dir" MIGRATOR_RACE_MODE=index \
    python3 "$MIGRATOR" --source "$race_source_dir" --destination "$race_destination_dir" --apply --no-clobber \
      > "$TEST_TMP_DIR/race-index.log" 2>&1; then
    fail '--no-clobber 在发布迁移清单时遇到冲突必须失败'
  fi

  payload_destination="$race_destination_dir/micro/bench_chunk_header/20260803_120008/legacy_micro_20260803_120008.txt"
  mapfile -t raced_indexes < <(find "$race_destination_dir/_legacy_migrations" -maxdepth 1 -type f -name '*.json' | sort)
  assert_equals '1' "${#raced_indexes[@]}" '并发写入的迁移清单必须保留'
  if [ "${#raced_indexes[@]}" -eq 1 ]; then
    assert_equals 'concurrent publisher' "$(cat "${raced_indexes[0]}")" '并发迁移清单不得被覆盖'
  fi
  assert_missing "$payload_destination" '迁移清单冲突后必须清理本次已发布的迁移文件'
}

assert_no_clobber_destination_publish_race
assert_no_clobber_index_publish_race

assert_destination_root_symlink_is_rejected() {
  local external_destination="$TEST_TMP_DIR/external-report"
  local linked_destination="$TEST_TMP_DIR/linked-report"

  mkdir -p "$external_destination"
  ln -s "$external_destination" "$linked_destination"
  if python3 "$MIGRATOR" --source "$source_dir" --destination "$linked_destination" --apply --no-clobber \
    > "$TEST_TMP_DIR/destination-symlink.log" 2>&1; then
    fail '符号链接 destination root 必须被拒绝'
  fi
  assert_missing "$external_destination/micro" '被拒绝的 destination 链接不得向工作区外发布报告'
  assert_missing "$external_destination/_legacy_migrations" '被拒绝的 destination 链接不得向工作区外发布迁移清单'
}

assert_destination_root_symlink_is_rejected

assert_unknown_section_title_aggregates_entire_report() {
  local unknown_source_dir="$TEST_TMP_DIR/unknown-section-source"
  local unknown_destination_dir="$TEST_TMP_DIR/unknown-section-report"
  local unknown_source="$unknown_source_dir/micro_20260803_120009.txt"
  local aggregate_destination
  local split_destination
  mkdir -p "$unknown_source_dir"
  cat > "$unknown_source" <<'EOF'
==========================================
  微基准测试报告
  时间: 20260803_120009
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
==========================================

--- bench_chunk_header ---
recognized section bytes
--- unknown-target ---
hyphenated unknown section bytes
--- invalid title ---
spaced unknown section bytes
EOF

  if ! python3 "$MIGRATOR" --source "$unknown_source_dir" --destination "$unknown_destination_dir" --apply --no-clobber \
    > "$TEST_TMP_DIR/unknown-section.log" 2>&1; then
    fail '包含未知分段标题的报告仍应作为 aggregate 复制'
  fi

  aggregate_destination="$unknown_destination_dir/micro/_legacy_aggregate/20260803_120009/legacy_micro_20260803_120009.txt"
  split_destination="$unknown_destination_dir/micro/bench_chunk_header/20260803_120009/legacy_micro_20260803_120009.txt"
  assert_missing "$split_destination" '未知分段不得被并入此前有效分段'
  assert_file "$aggregate_destination" '未知分段报告必须整份 aggregate 保存'
  assert_equals "$(sha256 "$unknown_source")" "$(sha256 "$aggregate_destination")" \
    '未知分段 aggregate 必须逐字节保留原报告'
}

assert_intermediate_symlink_publish_race_stays_outside() {
  local race_source_dir="$TEST_TMP_DIR/directory-race-source"
  local race_destination_dir="$TEST_TMP_DIR/directory-race-report"
  local race_external_dir="$TEST_TMP_DIR/directory-race-external"
  local race_backup_dir="$TEST_TMP_DIR/directory-race-micro-backup"
  local race_hook_dir="$TEST_TMP_DIR/directory-race-hook"
  local race_source="$race_source_dir/micro_20260803_120010.txt"
  local race_external_payload="$race_external_dir/bench_chunk_header/20260803_120010/legacy_micro_20260803_120010.txt"
  mkdir -p "$race_source_dir" "$race_external_dir/bench_chunk_header/20260803_120010" "$race_hook_dir"
  cat > "$race_source" <<'EOF'
==========================================
  微基准测试报告
  时间: 20260803_120010
  Git:  test-revision (dirty=0)
  主机: migration-test
  CPU:  test-cpu
  核心: 1
==========================================

--- bench_chunk_header ---
directory replacement race payload
EOF
  cat > "$race_hook_dir/sitecustomize.py" <<'PY'
import os

target = os.environ["MIGRATOR_DIRECTORY_RACE_TARGET"]
backup = os.environ["MIGRATOR_DIRECTORY_RACE_BACKUP"]
external = os.environ["MIGRATOR_DIRECTORY_RACE_EXTERNAL"]
original_open = os.open
raced = False


def open(path, flags, mode=0o777, *, dir_fd=None):
    global raced
    if not raced and os.fsdecode(path).startswith(".legacy_micro_20260803_120010.txt."):
        os.rename(target, backup)
        os.symlink(external, target)
        raced = True
    return original_open(path, flags, mode, dir_fd=dir_fd)


os.open = open
PY

  if PYTHONPATH="$race_hook_dir" \
    MIGRATOR_DIRECTORY_RACE_TARGET="$race_destination_dir/micro" \
    MIGRATOR_DIRECTORY_RACE_BACKUP="$race_backup_dir" \
    MIGRATOR_DIRECTORY_RACE_EXTERNAL="$race_external_dir" \
    python3 "$MIGRATOR" --source "$race_source_dir" --destination "$race_destination_dir" --apply --no-clobber \
      > "$TEST_TMP_DIR/directory-race.log" 2>&1; then
    fail '中间目录被替换为符号链接时迁移必须失败'
  fi

  assert_missing "$race_external_payload" '中间目录符号链接竞态不得在工作区外创建任何迁移文件'
}

assert_unknown_section_title_aggregates_entire_report
assert_intermediate_symlink_publish_race_stays_outside

for source in "$micro_source" "$qps_source" "$ambiguous_qps_source" "$load_source" \
  "$headerless_micro_source" "$metadata_like_micro_source" "$normalized_micro_source"; do
  assert_equals "${source_hashes[$source]}" "$(sha256 "$source")" "apply 后原件 SHA-256"
  assert_equals "${source_sizes[$source]}" "$(file_size "$source")" "apply 后原件大小"
done

if [ "$failures" -ne 0 ]; then
  printf '历史报告迁移回归测试失败：%s 个断言失败\n' "$failures" >&2
  exit 1
fi

printf '历史报告迁移回归测试通过\n'
