"""Exercise version selection through CMake and the idf.py wrapper, without IDF."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def git_version():
    revision = subprocess.check_output(
        ["git", "-c", "maintenance.auto=false", "rev-parse", "--short=7", "HEAD"],
        cwd=ROOT,
        text=True,
    ).strip()
    return f"0.3.0-{revision}"


def test_cmake_removes_stale_version_and_expires_override(tmp_path, git_version):
    source = tmp_path / "source"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.22)\n"
        f'include("{ROOT / "firmware/project-version.cmake"}")\n'
        "project(version_fixture NONE)\n"
        'file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VER}")\n'
    )
    build = tmp_path / "build"
    environment = os.environ.copy()
    environment.pop("ORB_PROJECT_VER", None)
    command = ["cmake", "-S", str(source), "-B", str(build)]
    subprocess.run(
        [*command, "-DPROJECT_VER=0.3.0-stale-cache"],
        env=environment,
        check=True,
        capture_output=True,
    )
    assert (build / "version.txt").read_text() == git_version
    assert not any(
        line.startswith("PROJECT_VER:")
        for line in (build / "CMakeCache.txt").read_text().splitlines()
    )

    subprocess.run(
        command,
        env={**environment, "ORB_PROJECT_VER": "0.3.0-one-build"},
        check=True,
        capture_output=True,
    )
    assert (build / "version.txt").read_text() == "0.3.0-one-build"
    subprocess.run(command, env=environment, check=True, capture_output=True)
    assert (build / "version.txt").read_text() == git_version
    assert f"ORB_BUILD_VERSION:INTERNAL={git_version}" in (build / "CMakeCache.txt").read_text()


@pytest.fixture
def wrapper(tmp_path):
    project = tmp_path / "project with spaces"
    (project / "scripts").mkdir(parents=True)
    (project / "firmware").mkdir()
    script = project / "scripts/idf.sh"
    shutil.copyfile(ROOT / "scripts/idf.sh", script)
    fake_idf = tmp_path / "idf"
    (fake_idf / "bin").mkdir(parents=True)
    (fake_idf / "export.sh").write_text('export PATH="$IDF_PATH/bin:$PATH"\n')
    executable = fake_idf / "bin/idf.py"
    executable.write_text(
        "#!/bin/bash\n"
        'printf "%s\\n" "$IDF_PY_BUILD_JOBS" "$CMAKE_BUILD_PARALLEL_LEVEL" > "$IDF_PATH/jobs.txt"\n'
        'printf "%s\\0" "${ORB_PROJECT_VER-}" "$@"\n'
    )
    executable.chmod(0o755)
    environment = os.environ.copy()
    environment.pop("ORB_PROJECT_VER", None)
    environment["IDF_PATH"] = str(fake_idf)

    def invoke(arguments=(), version=None):
        selected_environment = environment.copy()
        if version is not None:
            selected_environment["ORB_PROJECT_VER"] = version
        output = subprocess.check_output(
            ["bash", str(script), *arguments], env=selected_environment
        )
        return output.decode().rstrip("\0").split("\0")

    invoke.firmware = project / "firmware"
    invoke.jobs = lambda: (fake_idf / "jobs.txt").read_text().splitlines()
    return invoke


@pytest.mark.parametrize(
    "arguments",
    [
        ["-D", "PROJECT_VER=0.3.0-recovery"],
        ["-DPROJECT_VER=0.3.0-recovery"],
        ["-D", "PROJECT_VER:STRING=0.3.0-recovery"],
        ["-DPROJECT_VER:STRING=0.3.0-recovery"],
        ["--define-cache-entry", "PROJECT_VER=0.3.0-recovery"],
        ["--define-cache-entry=PROJECT_VER=0.3.0-recovery"],
        ["--define-cache-entry=PROJECT_VER:STRING=0.3.0-recovery"],
    ],
)
def test_wrapper_override_is_one_invocation_and_other_arguments_survive(wrapper, arguments):
    assert wrapper([*arguments, "-B", "build path", "-D", "OTHER=1", "build"]) == [
        "0.3.0-recovery",
        "-D",
        "ORB_BUILD_VERSION=0.3.0-recovery",
        "-D",
        "ORB_BUILD_CHANNEL=dev",
        "-B",
        "build path",
        "-D",
        "OTHER=1",
        "build",
    ]
    # The fixture has no Git repository; a later ordinary invocation cannot use
    # the previous release identity even when it targets the same build folder.
    assert wrapper(["build"]) == [
        "",
        "-D",
        "ORB_BUILD_VERSION=0.3.0-unknown",
        "-D",
        "ORB_BUILD_CHANNEL=dev",
        "build",
    ]


def test_wrapper_environment_override_and_last_cli_override_win(wrapper):
    assert wrapper(["build"], version="0.3.0-env") == [
        "0.3.0-env",
        "-D",
        "ORB_BUILD_VERSION=0.3.0-env",
        "-D",
        "ORB_BUILD_CHANNEL=dev",
        "build",
    ]
    assert wrapper(
        ["-DPROJECT_VER=0.3.0-first", "-D", "PROJECT_VER=0.3.0-last", "build"],
        version="0.3.0-env",
    ) == [
        "0.3.0-last",
        "-D",
        "ORB_BUILD_VERSION=0.3.0-last",
        "-D",
        "ORB_BUILD_CHANNEL=dev",
        "build",
    ]


def test_wrapper_stable_overlay_isolated_and_jobs_bounded(wrapper):
    firmware = wrapper.firmware
    assert wrapper(["--channel", "stable", "-j4", "build"]) == [
        "",
        "-D",
        "ORB_BUILD_VERSION=0.3.0-unknown",
        "-D",
        "ORB_BUILD_CHANNEL=stable",
        "-B",
        "build-stable",
        "-D",
        f"SDKCONFIG={firmware}/build-stable/sdkconfig",
        "-D",
        f"SDKCONFIG_DEFAULTS={firmware}/sdkconfig.defaults;{firmware}/sdkconfig.stable",
        "build",
    ]
    assert wrapper.jobs() == ["4", "4"]
    wrapper(["build"])
    assert wrapper.jobs() == ["4", "4"]


@pytest.mark.parametrize(
    "arguments,jobs",
    [(["-j", "1"], "1"), (["-j2"], "2"), (["--jobs", "3"], "3"), (["--jobs=4"], "4")],
)
def test_wrapper_jobs_reach_idf_and_nested_cmake(wrapper, arguments, jobs):
    assert wrapper([*arguments, "build"])[-1] == "build"
    assert wrapper.jobs() == [jobs, jobs]


@pytest.mark.parametrize("arguments", [["-j0"], ["-j5"], ["--jobs", "0"], ["--jobs=32"], ["-j"]])
def test_wrapper_rejects_jobs_outside_live_service_budget(wrapper, arguments):
    with pytest.raises(subprocess.CalledProcessError) as error:
        wrapper([*arguments, "build"])
    assert error.value.returncode == 2
