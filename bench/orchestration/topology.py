from __future__ import annotations

from pathlib import Path


CPU_ROOT = Path("/sys/devices/system/cpu")


def _read_list(path: Path) -> list[int]:
    """Parses a sysfs cpu list such as "0-2,5"."""
    try:
        text = path.read_text(encoding="ascii").strip()
    except (FileNotFoundError, PermissionError, OSError):
        return []
    values: list[int] = []
    for part in filter(None, text.split(",")):
        if "-" in part:
            first, last = (int(bound) for bound in part.split("-", 1))
            values.extend(range(first, last + 1))
        else:
            values.append(int(part))
    return values


def online_cpus() -> list[int]:
    return sorted(_read_list(CPU_ROOT / "online"))


def siblings(cpu: int) -> frozenset[int]:
    """The SMT threads sharing a physical core with cpu, cpu included."""
    listed = _read_list(
        CPU_ROOT / f"cpu{cpu}/topology/thread_siblings_list"
    )
    return frozenset(listed or [cpu])


def physical_cores() -> list[list[int]]:
    """One entry per physical core, each holding its sibling threads."""
    seen: set[frozenset[int]] = set()
    cores: list[list[int]] = []
    for cpu in online_cpus():
        group = siblings(cpu)
        if group in seen:
            continue
        seen.add(group)
        cores.append(sorted(group))
    return cores


def select_cpus(count: int, *, avoid_cpu0: bool = True) -> list[int]:
    """Picks `count` CPUs on distinct physical cores.

    CPU 0 is skipped by default: it is the usual default IRQ target, so it is
    the one CPU whose noise correlates with the I/O rate being measured.
    """
    candidates = [core[0] for core in physical_cores()]
    preferred = [cpu for cpu in candidates if cpu != 0] if avoid_cpu0 else candidates
    if len(preferred) < count:
        # a small or non-SMT machine: fall back rather than refuse to run
        preferred = candidates
    if len(preferred) < count:
        raise ValueError(
            f"{count} CPUs on distinct physical cores are required, "
            f"found {len(preferred)}"
        )
    return preferred[:count]


def sibling_conflicts(cpus: tuple[int, ...]) -> list[tuple[int, int]]:
    """Configured CPUs that share a physical core, which would silently halve
    the parallelism a concurrency scenario believes it has."""
    conflicts: list[tuple[int, int]] = []
    for index, cpu in enumerate(cpus):
        for other in cpus[index + 1 :]:
            if other in siblings(cpu):
                conflicts.append((cpu, other))
    return conflicts


def describe(cpus: tuple[int, ...]) -> dict[str, object]:
    return {
        "online": online_cpus(),
        "physical_cores": physical_cores(),
        "selected": list(cpus),
        "selected_siblings": {
            str(cpu): sorted(siblings(cpu)) for cpu in cpus
        },
    }
