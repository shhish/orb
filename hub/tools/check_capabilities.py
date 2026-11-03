"""Build local wheels, then check capabilities in separate offline clean venvs.

The caller supplies a wheelhouse containing the locked runtime dependencies and
PEP 517 build dependencies. There is deliberately no network-enabled mode.
"""

import argparse
import os
import shutil
import subprocess
import tempfile
import venv
from pathlib import Path

HUB = Path(__file__).resolve().parents[1]
CAPABILITIES = {
    "core": (),
    "agents": ("agents",),
    "dot": ("dot",),
    "mcp": ("mcp",),
    "combined": ("agents", "dot", "mcp"),
}


def environment():
    # Source-path injection and the owner's pip configuration cannot make an
    # incomplete wheel appear complete, or silently enable an index request.
    result = {key: value for key, value in os.environ.items() if not key.startswith(("PIP_", "PYTHON"))}
    result.update(PIP_NO_INDEX="1", PIP_DISABLE_PIP_VERSION_CHECK="1", PIP_CONFIG_FILE=os.devnull, PIP_NO_CACHE_DIR="1")
    return result


def run(command, *, cwd, env):
    subprocess.run(command, cwd=cwd, env=env, check=True)


def check(wheelhouse: Path):
    wheelhouse = wheelhouse.resolve()
    if not wheelhouse.is_dir() or not any(wheelhouse.glob("*.whl")):
        raise ValueError("Offline capability check needs a populated wheelhouse")
    env = environment()
    with tempfile.TemporaryDirectory(prefix="orb-capabilities-") as directory:
        work = Path(directory)
        wheels = work / "wheels"
        wheels.mkdir()
        source = work / "source"
        shutil.copytree(HUB, source, ignore=shutil.ignore_patterns("__pycache__", "build", "*.egg-info"))
        builder = work / "builder"
        venv.EnvBuilder(with_pip=True).create(builder)
        python = builder / "bin" / "python"
        # Build isolation resolves solely against the supplied wheelhouse.
        run(
            [
                str(python),
                "-m",
                "pip",
                "wheel",
                "--no-index",
                "--no-deps",
                "--find-links",
                str(wheelhouse),
                "--wheel-dir",
                str(wheels),
                str(source),
                str(source / "packages" / "orb_agents"),
                str(source / "packages" / "orb_mcp"),
            ],
            cwd=work,
            env=env,
        )
        hub_wheel = next(wheels.glob("orb_hub-*.whl"))
        for name, extras in CAPABILITIES.items():
            target = work / name
            venv.EnvBuilder(with_pip=True).create(target)
            python = target / "bin" / "python"
            requirement = str(hub_wheel) + (f"[{','.join(extras)}]" if extras else "")
            run(
                [
                    str(python),
                    "-m",
                    "pip",
                    "install",
                    "--no-index",
                    "--find-links",
                    str(wheelhouse),
                    "--find-links",
                    str(wheels),
                    "--constraint",
                    str(HUB / "capability-lock.txt"),
                    requirement,
                ],
                cwd=work,
                env=env,
            )
            modules = ["orb_hub.cli", "orb_hub.server"]
            if "agents" in extras:
                modules.extend(("orb_hub.agents", "orb_hub.agent_data", "orb_agents.hooks"))
            if {"dot", "mcp"}.intersection(extras):
                modules.extend(("orb_hub.dot", "orb_mcp.cli", "orb_mcp.server"))
            code = "import importlib; " + "; ".join(f"importlib.import_module({module!r})" for module in modules)
            run([str(python), "-I", "-c", code], cwd=work, env=env)
            run([str(python), "-m", "pip", "check"], cwd=work, env=env)
            print(f"{name}: clean wheel imports and dependency check passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheelhouse", type=Path, required=True)
    args = parser.parse_args()
    check(args.wheelhouse)


if __name__ == "__main__":
    main()
