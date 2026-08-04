#!/usr/bin/env python3
"""Copy legacy benchmark reports into the read-only layered report archive."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


FILENAME_PATTERN = re.compile(r"^(micro|qps|load)(?:_[A-Za-z0-9-]+)*_(\d{8}_\d{6})\.txt$")
SECTION_PATTERN = re.compile(rb"(?m)^---[ \t]*(.*?)[ \t]*---[ \t]*(?:\r?\n|$)")
KIND_BY_LEGACY_PREFIX = {"micro": "micro", "qps": "qps", "load": "rps"}
LEGACY_HEADER_DELIMITERS = frozenset(
    {
        "==========================================",
        "=======================================================",
    }
)
LEGACY_HEADER_TITLES = {
    "micro": frozenset({"  微基准测试报告", "  微基准测试报告（Release 模式）"}),
    "qps": frozenset({"  QPS + 压力测试报告"}),
}
LEGACY_HEADER_FIELDS = (
    "  时间: ",
    "  Git:  ",
    "  主机: ",
    "  CPU:  ",
    "  核心: ",
)
LEGACY_PROFILE_FIELD = "  Profile: "


@dataclass(frozen=True)
class MigrationEntry:
    kind: str
    source_path: Path
    source_relative_path: str
    source_sha256: str
    timestamp: str
    byte_start: int
    byte_end: int
    section_title: str | None
    destination_relative_path: Path
    content: bytes


@dataclass(frozen=True)
class PublishedFile:
    directory_fd: int
    filename: str
    display_path: Path
    device: int
    inode: int


def parse_args() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="迁移只读历史基准 TXT 报告")
    parser.add_argument("--source", type=Path, default=project_root / "benchmark" / "reports")
    parser.add_argument("--destination", type=Path, default=project_root / "benchmark" / "report")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--apply", action="store_true", help="实际复制迁移结果")
    mode.add_argument("--dry-run", action="store_true", help="仅显示迁移计划（默认）")
    parser.add_argument("--no-clobber", action="store_true", help="若任一目标已存在则拒绝写入")
    return parser.parse_args()


def discover_targets(project_root: Path) -> dict[str, set[str]]:
    benchmark_dir = project_root / "benchmark"
    return {
        "micro": {path.stem for path in benchmark_dir.glob("bench_*.cpp") if path.is_file()},
        "qps": {path.stem for path in benchmark_dir.glob("qps_*.cpp") if path.is_file()},
    }


def normalize_section_target(kind: str, title: str, targets: set[str]) -> str | None:
    if title in targets:
        return title
    if kind == "micro" and title.startswith("bench_bench_"):
        normalized = title[len("bench_") :]
        if normalized in targets:
            return normalized
    return None


def is_legacy_report_header(legacy_kind: str, preamble: bytes) -> bool:
    """Accept only complete metadata blocks produced by known legacy writers."""
    try:
        lines = preamble.decode("utf-8").splitlines()
    except UnicodeDecodeError:
        return False
    if not lines or lines[-1] != "":
        return False

    header_lines = lines[:-1]
    if len(header_lines) not in {len(LEGACY_HEADER_FIELDS) + 3, len(LEGACY_HEADER_FIELDS) + 4}:
        return False
    if header_lines[0] != header_lines[-1] or header_lines[0] not in LEGACY_HEADER_DELIMITERS:
        return False
    if header_lines[1] not in LEGACY_HEADER_TITLES.get(legacy_kind, frozenset()):
        return False

    for line, prefix in zip(header_lines[2 : 2 + len(LEGACY_HEADER_FIELDS)], LEGACY_HEADER_FIELDS):
        if not line.startswith(prefix) or line == prefix:
            return False

    has_profile = len(header_lines) == len(LEGACY_HEADER_FIELDS) + 4
    return not has_profile or (
        header_lines[-2].startswith(LEGACY_PROFILE_FIELD) and header_lines[-2] != LEGACY_PROFILE_FIELD
    )


def aggregate_entry(
    source_path: Path,
    source_relative_path: str,
    kind: str,
    timestamp: str,
    source_sha256: str,
    content: bytes,
) -> MigrationEntry:
    destination = Path(kind) / "_legacy_aggregate" / timestamp / f"legacy_{source_path.name}"
    return MigrationEntry(
        kind=kind,
        source_path=source_path,
        source_relative_path=source_relative_path,
        source_sha256=source_sha256,
        timestamp=timestamp,
        byte_start=0,
        byte_end=len(content),
        section_title=None,
        destination_relative_path=destination,
        content=content,
    )


def split_entries(
    source_path: Path,
    source_relative_path: str,
    legacy_kind: str,
    timestamp: str,
    targets: set[str],
) -> list[MigrationEntry]:
    content = source_path.read_bytes()
    digest = hashlib.sha256(content).hexdigest()
    kind = KIND_BY_LEGACY_PREFIX[legacy_kind]
    if legacy_kind not in {"micro", "qps"} or not content:
        return [aggregate_entry(source_path, source_relative_path, kind, timestamp, digest, content)]

    matches = list(SECTION_PATTERN.finditer(content))
    if not matches or not is_legacy_report_header(legacy_kind, content[: matches[0].start()]):
        return [aggregate_entry(source_path, source_relative_path, kind, timestamp, digest, content)]

    sections: list[tuple[str, str, int, int]] = []
    seen_targets: set[str] = set()
    for index, match in enumerate(matches):
        try:
            title = match.group(1).decode("ascii")
        except UnicodeDecodeError:
            return [aggregate_entry(source_path, source_relative_path, kind, timestamp, digest, content)]
        target = normalize_section_target(kind, title, targets)
        if target is None or target in seen_targets:
            return [aggregate_entry(source_path, source_relative_path, kind, timestamp, digest, content)]
        seen_targets.add(target)
        end = matches[index + 1].start() if index + 1 < len(matches) else len(content)
        sections.append((title, target, match.start(), end))

    entries: list[MigrationEntry] = []
    for title, target, start, end in sections:
        if start >= end:
            return [aggregate_entry(source_path, source_relative_path, kind, timestamp, digest, content)]
        destination = Path(kind) / target / timestamp / f"legacy_{source_path.name}"
        entries.append(
            MigrationEntry(
                kind=kind,
                source_path=source_path,
                source_relative_path=source_relative_path,
                source_sha256=digest,
                timestamp=timestamp,
                byte_start=start,
                byte_end=end,
                section_title=title,
                destination_relative_path=destination,
                content=content[start:end],
            )
        )
    return entries


def discover_entries(source_dir: Path, project_root: Path) -> list[MigrationEntry]:
    targets = discover_targets(project_root)
    entries: list[MigrationEntry] = []
    for source_path in sorted(source_dir.rglob("*.txt")):
        if not source_path.is_file() or source_path.is_symlink():
            continue
        match = FILENAME_PATTERN.fullmatch(source_path.name)
        if match is None:
            print(f"跳过不受支持的历史文件名: {source_path.relative_to(source_dir)}", file=sys.stderr)
            continue
        legacy_kind, timestamp = match.groups()
        source_relative_path = source_path.relative_to(source_dir).as_posix()
        entries.extend(split_entries(source_path, source_relative_path, legacy_kind, timestamp, targets.get(legacy_kind, set())))
    return entries


def resolve_safe_destination_dir(destination: Path) -> Path:
    """Return an absolute lexical destination path without following links."""
    expanded = destination.expanduser()
    candidate = expanded if expanded.is_absolute() else Path.cwd() / expanded
    current = Path(candidate.anchor)
    for part in candidate.parts[1:]:
        if part == ".":
            continue
        if part == "..":
            if current.is_symlink():
                raise ValueError(f"不安全的目标目录: {current}")
            current = current.parent
            continue
        current /= part
        if current.is_symlink() or (current.exists() and not current.is_dir()):
            raise ValueError(f"不安全的目标目录: {current}")
    return current


def open_child_directory(parent_fd: int, name: str, create: bool) -> int:
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    try:
        return os.open(name, flags, dir_fd=parent_fd)
    except FileNotFoundError:
        if not create:
            raise
        try:
            os.mkdir(name, 0o755, dir_fd=parent_fd)
        except FileExistsError:
            pass
        return os.open(name, flags, dir_fd=parent_fd)


def open_safe_directory(path: Path, create: bool) -> int:
    if not path.is_absolute():
        raise ValueError(f"目标目录必须是绝对路径: {path}")
    directory_fd = os.open(path.anchor, os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            next_fd = open_child_directory(directory_fd, part, create)
            os.close(directory_fd)
            directory_fd = next_fd
        return directory_fd
    except BaseException:
        os.close(directory_fd)
        raise


def open_safe_relative_directory(root_fd: int, relative_path: Path, create: bool) -> int:
    if relative_path.is_absolute() or any(part in {"", ".", ".."} for part in relative_path.parts):
        raise ValueError(f"不安全的相对目标目录: {relative_path}")
    directory_fd = os.dup(root_fd)
    try:
        for part in relative_path.parts:
            next_fd = open_child_directory(directory_fd, part, create)
            os.close(directory_fd)
            directory_fd = next_fd
        return directory_fd
    except BaseException:
        os.close(directory_fd)
        raise


def verify_relative_directory(root_fd: int, relative_path: Path, expected_fd: int) -> None:
    verified_fd = open_safe_relative_directory(root_fd, relative_path, create=False)
    try:
        verified = os.fstat(verified_fd)
        expected = os.fstat(expected_fd)
        if (verified.st_dev, verified.st_ino) != (expected.st_dev, expected.st_ino):
            raise ValueError(f"目标目录在发布期间被替换: {relative_path}")
    finally:
        os.close(verified_fd)


def unlink_owned_file(directory_fd: int, filename: str, device: int, inode: int) -> None:
    try:
        current = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
        if (current.st_dev, current.st_ino) == (device, inode):
            os.unlink(filename, dir_fd=directory_fd)
    except FileNotFoundError:
        pass


def create_temporary_file(directory_fd: int, filename: str) -> tuple[int, str]:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    for _ in range(100):
        temporary_name = f".{filename}.{secrets.token_hex(16)}"
        try:
            return os.open(temporary_name, flags, 0o600, dir_fd=directory_fd), temporary_name
        except FileExistsError:
            continue
    raise FileExistsError(f"无法创建唯一临时文件: {filename}")


def atomic_write(
    destination_root_fd: int, destination_relative_path: Path, content: bytes, no_clobber: bool = False
) -> PublishedFile | None:
    if destination_relative_path.is_absolute() or destination_relative_path.name in {"", ".", ".."}:
        raise ValueError(f"不安全的目标文件: {destination_relative_path}")

    directory_relative_path = destination_relative_path.parent
    filename = destination_relative_path.name
    directory_fd = open_safe_relative_directory(destination_root_fd, directory_relative_path, create=True)
    temporary_name: str | None = None
    temporary_stat: os.stat_result | None = None
    try:
        descriptor, temporary_name = create_temporary_file(directory_fd, filename)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fchmod(stream.fileno(), 0o600)
            os.fsync(stream.fileno())
            temporary_stat = os.fstat(stream.fileno())

        verify_relative_directory(destination_root_fd, directory_relative_path, directory_fd)
        if no_clobber:
            os.link(
                temporary_name,
                filename,
                src_dir_fd=directory_fd,
                dst_dir_fd=directory_fd,
                follow_symlinks=False,
            )
        else:
            os.replace(temporary_name, filename, src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
        temporary_name = None

        try:
            verify_relative_directory(destination_root_fd, directory_relative_path, directory_fd)
        except BaseException:
            assert temporary_stat is not None
            unlink_owned_file(directory_fd, filename, temporary_stat.st_dev, temporary_stat.st_ino)
            raise

        if no_clobber:
            assert temporary_stat is not None
            published_file = PublishedFile(
                directory_fd, filename, destination_relative_path, temporary_stat.st_dev, temporary_stat.st_ino
            )
            directory_fd = -1
            return published_file
        return None
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name, dir_fd=directory_fd)
            except FileNotFoundError:
                pass
        if directory_fd >= 0:
            os.close(directory_fd)


def destination_conflicts(destination_dir: Path, entries: list[MigrationEntry]) -> list[Path]:
    destinations = [destination_dir / entry.destination_relative_path for entry in entries]
    duplicate_destinations = {path for path in destinations if destinations.count(path) > 1}
    existing = [path for path in destinations if os.path.lexists(path)]
    return sorted(duplicate_destinations | set(existing))


def write_index(
    destination_root_fd: int, entries: list[MigrationEntry], migrated_at: str, no_clobber: bool = False
) -> tuple[Path, PublishedFile | None]:
    index_path = Path("_legacy_migrations") / f"migration-{datetime.now(timezone.utc):%Y%m%d_%H%M%S_%f}.json"
    data = {
        "schema_version": 1,
        "migrated_at": migrated_at,
        "entries": [
            {
                "kind": entry.kind,
                "source_path": entry.source_relative_path,
                "source_sha256": entry.source_sha256,
                "byte_start": entry.byte_start,
                "byte_end": entry.byte_end,
                "section_title": entry.section_title,
                "destination_path": entry.destination_relative_path.as_posix(),
            }
            for entry in entries
        ],
    }
    published_file = atomic_write(
        destination_root_fd, index_path, (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf-8"), no_clobber
    )
    return index_path, published_file


def cleanup_published_files(published_files: list[PublishedFile]) -> None:
    for published_file in reversed(published_files):
        try:
            unlink_owned_file(
                published_file.directory_fd, published_file.filename, published_file.device, published_file.inode
            )
        except OSError as error:
            print(f"错误: 无法清理本次迁移文件 {published_file.display_path}: {error}", file=sys.stderr)


def close_published_files(published_files: list[PublishedFile]) -> None:
    for published_file in published_files:
        os.close(published_file.directory_fd)


def main() -> int:
    args = parse_args()
    source_dir = args.source.resolve(strict=False)
    project_root = Path(__file__).resolve().parents[2]
    if not source_dir.is_dir() or source_dir.is_symlink():
        print(f"错误: 历史报告源目录不存在或不安全: {source_dir}", file=sys.stderr)
        return 2
    try:
        destination_dir = resolve_safe_destination_dir(args.destination)
    except ValueError as error:
        print(f"错误: 历史报告目标目录不安全: {error}", file=sys.stderr)
        return 2

    entries = discover_entries(source_dir, project_root)
    if not entries:
        print("没有可迁移的历史 TXT 报告")
        return 0

    for entry in entries:
        section = entry.section_title if entry.section_title is not None else "_legacy_aggregate"
        print(
            f"{entry.source_relative_path}[{entry.byte_start}:{entry.byte_end}] "
            f"({section}) -> {entry.destination_relative_path.as_posix()}"
        )

    if not args.apply:
        print(f"dry-run: 计划复制 {len(entries)} 个只读历史片段")
        return 0

    if args.no_clobber:
        conflicts = destination_conflicts(destination_dir, entries)
        if conflicts:
            print("错误: --no-clobber 拒绝已有或重复目标:", file=sys.stderr)
            for conflict in conflicts:
                print(conflict, file=sys.stderr)
            return 1

    published_files: list[PublishedFile] = []
    destination_root_fd: int | None = None
    try:
        destination_root_fd = open_safe_directory(destination_dir, create=True)
        for entry in entries:
            published_file = atomic_write(
                destination_root_fd, entry.destination_relative_path, entry.content, args.no_clobber
            )
            if published_file is not None:
                published_files.append(published_file)
        migrated_at = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
        index_path, published_file = write_index(destination_root_fd, entries, migrated_at, args.no_clobber)
        if published_file is not None:
            published_files.append(published_file)
    except (OSError, ValueError) as error:
        if args.no_clobber:
            cleanup_published_files(published_files)
        print(f"错误: 写入历史迁移结果失败: {error}", file=sys.stderr)
        return 1
    finally:
        if destination_root_fd is not None:
            os.close(destination_root_fd)
        close_published_files(published_files)

    print(f"已复制 {len(entries)} 个只读历史片段，迁移清单: {destination_dir / index_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
