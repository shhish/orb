"""Executable dependency rules for the hub's service boundaries."""

import ast
from pathlib import Path

import pytest

HUB = Path(__file__).resolve().parents[1]
SOURCE = HUB / "orb_hub"
BRIDGES = {"agent_data", "agents", "dot", "home", "nowplaying", "weather_delivery"}
COMPOSITION = {"server", "link", "sessions", *BRIDGES}
FORBIDDEN_IMPORTS = {
    "session_store": COMPOSITION | {"session_actions", "session_delivery"},
    "session_actions": COMPOSITION | {"session_delivery"},
    "session_delivery": COMPOSITION,
    "session_wire": COMPOSITION | {"session_store", "session_actions", "session_delivery"},
    "link": {"server", "sessions", *BRIDGES},
    "agent_data": {"server", "sessions", "agents", "dot", "link"},
}


def own_attribute(node):
    return isinstance(node, ast.Name) and node.id in {"self", "cls"}


def link_receiver(node):
    return (isinstance(node, ast.Name) and node.id == "link") or (
        isinstance(node, ast.Attribute) and node.attr == "link"
    )


def violations(source, module):
    tree = ast.parse(source)
    errors = []
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Attribute)
            and node.attr.startswith("_")
            and not node.attr.startswith("__")
            and not own_attribute(node.value)
        ):
            errors.append((node.lineno, "foreign private attribute"))
        if isinstance(node, ast.Attribute) and isinstance(node.ctx, ast.Store) and link_receiver(node.value):
            errors.append((node.lineno, "attaching state to Link"))
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if (
            isinstance(func, ast.Attribute)
            and func.attr in {"create_task", "ensure_future", "Task"}
            and module != "lifecycle"
        ):
            errors.append((node.lineno, "unsupervised task factory"))
        if isinstance(func, ast.Name) and func.id in {"create_task", "ensure_future", "Task"}:
            errors.append((node.lineno, "unowned imported task factory"))
        if isinstance(func, ast.Name) and func.id in {"getattr", "setattr", "delattr"} and len(node.args) >= 2:
            receiver, attr = node.args[:2]
            if func.id != "getattr" and link_receiver(receiver):
                errors.append((node.lineno, "attaching state to Link"))
            if (
                isinstance(attr, ast.Constant)
                and isinstance(attr.value, str)
                and attr.value.startswith("_")
                and not attr.value.startswith("__")
                and not own_attribute(receiver)
            ):
                # Existing lock eviction checks awaiters before removing a
                # keyed lock. This asyncio detail is not a service port.
                lock_waiters = (
                    module in {"home", "session_actions"}
                    and func.id == "getattr"
                    and isinstance(receiver, ast.Name)
                    and receiver.id == "lock"
                    and attr.value == "_waiters"
                )
                if not lock_waiters:
                    errors.append((node.lineno, "dynamic foreign private attribute"))
    for node in ast.walk(tree):
        names = []
        if isinstance(node, ast.Import):
            names = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom):
            prefix = node.module or ""
            if node.level:
                prefix = "orb_hub" + (f".{prefix}" if prefix else "")
            names = [prefix] + [f"{prefix}.{alias.name}" for alias in node.names]
            if prefix.startswith("orb_hub") and any(
                alias.name.startswith("_") and not alias.name.startswith("__") for alias in node.names
            ):
                errors.append((node.lineno, "foreign private import"))
        for name in names:
            if name.startswith("orb_hub.") and name.split(".")[1] in FORBIDDEN_IMPORTS.get(module, set()):
                errors.append((node.lineno, "dependency points toward composition/provider"))
    return errors


def test_hub_import_private_and_task_boundaries():
    failures = []
    for path in sorted(SOURCE.rglob("*.py")):
        failures.extend(
            f"{path.relative_to(HUB)}:{line}: {reason}" for line, reason in violations(path.read_text(), path.stem)
        )
    assert not failures, "\n".join(failures)


def test_provider_packages_remain_independent_of_hub():
    for package in ("orb_agents", "orb_mcp"):
        for path in (HUB / "packages" / package / "src").rglob("*.py"):
            tree = ast.parse(path.read_text())
            for node in ast.walk(tree):
                if isinstance(node, ast.ImportFrom):
                    assert not (node.module or "").startswith("orb_hub"), str(path)
                elif isinstance(node, ast.Import):
                    assert not any(alias.name.startswith("orb_hub") for alias in node.names), str(path)


def test_embedded_provider_tasks_use_the_injected_lifecycle_port():
    agents = HUB / "packages" / "orb_agents" / "src"
    for path in agents.rglob("*.py"):
        tree = ast.parse(path.read_text())
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            if isinstance(node.func, ast.Attribute) and node.func.attr in {"create_task", "ensure_future"}:
                assert path.name == "tasks.py", f"{path}:{node.lineno}: use the embedding task port"
            if isinstance(node.func, ast.Name) and node.func.id == "spawn":
                assert any(keyword.arg == "owner" for keyword in node.keywords), (
                    f"{path}:{node.lineno}: missing task owner"
                )
    server = HUB / "packages" / "orb_mcp" / "src" / "orb_mcp" / "server.py"
    tree = ast.parse(server.read_text())
    parents = {child: parent for parent in ast.walk(tree) for child in ast.iter_child_nodes(parent)}
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "create_task":
            ancestor = node
            standalone = False
            while ancestor in parents:
                child, ancestor = ancestor, parents[ancestor]
                if isinstance(ancestor, ast.If) and ast.unparse(ancestor.test) == "task_owner is None":
                    standalone = child in ancestor.body
                    break
            assert standalone, f"{server}:{node.lineno}: embedded MCP tasks require lifecycle ownership"


@pytest.mark.parametrize(
    "source,rule",
    [
        ("self.sessions._subscribers", "foreign private attribute"),
        ('getattr(self.sessions, "_cards")', "dynamic foreign private attribute"),
        ("link.recording_scope = 'x'", "attaching state to Link"),
        ('setattr(link, name, "x")', "attaching state to Link"),
        ("asyncio.create_task(work())", "unsupervised task factory"),
        ("loop.create_task(work())", "unsupervised task factory"),
        ("from orb_hub import server", "dependency points toward composition/provider"),
        ("from .server import Hub", "dependency points toward composition/provider"),
        ("from orb_hub.sessions import _private", "foreign private import"),
    ],
)
def test_boundary_check_rejects_leaks(source, rule):
    assert rule in {reason for _, reason in violations(source, "session_store")}


def test_boundary_check_accepts_own_state_and_public_ports():
    assert not violations("self._cards = {}\nself.delivery.publish()\nself.supervisor.spawn(work())", "session_store")
