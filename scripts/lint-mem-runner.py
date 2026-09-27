#!/usr/bin/env python3
"""Run clang-tidy concurrently while preserving Windows process arguments."""

from concurrent.futures import ThreadPoolExecutor
import shutil
import subprocess
import sys


def build_command(clang_tidy, checks, warnings_as_errors, source, compiler_args):
    return [
        clang_tidy,
        "--quiet",
        "--checks=" + checks,
        "--warnings-as-errors=" + warnings_as_errors,
        source,
        "--",
        *compiler_args,
    ]


def run_one(command):
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        return result.returncode, result.stdout, result.stderr
    except OSError as exc:
        return 1, b"", (str(exc) + "\n").encode("utf-8", "replace")


def main():
    if len(sys.argv) < 5:
        sys.stderr.write(
            "usage: lint-mem-runner.py CLANG_TIDY CHECKS WARNINGS_AS_ERRORS JOBS [COMPILER_ARGS...]\n"
        )
        return 2

    clang_tidy, checks, warnings_as_errors, jobs, *compiler_args = sys.argv[1:]
    try:
        workers = int(jobs)
    except ValueError:
        sys.stderr.write("lint-mem-runner: JOBS must be a positive integer\n")
        return 2
    if workers < 1:
        sys.stderr.write("lint-mem-runner: JOBS must be a positive integer\n")
        return 2

    sources = [line.strip() for line in sys.stdin if line.strip()]
    if not sources:
        sys.stderr.write("lint-mem-runner: no source files received\n")
        return 2

    executable_name = clang_tidy.replace("\\", "/").rsplit("/", 1)[-1]
    executable = shutil.which(executable_name) or clang_tidy
    commands = [
        build_command(executable, checks, warnings_as_errors, source, compiler_args)
        for source in sources
    ]

    failed = False
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for returncode, stdout, stderr in pool.map(run_one, commands):
            sys.stdout.buffer.write(stdout)
            sys.stderr.buffer.write(stderr)
            failed = failed or returncode != 0

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
