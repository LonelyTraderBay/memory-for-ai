#!/usr/bin/env python3
"""Exercise the real Make object rules with two tiny translation units."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--make", default="make")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = root / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="incremental-contract-", dir=build) as tmp:
        fixture = Path(tmp)
        assert fixture.resolve().is_relative_to(build.resolve())
        relative = fixture.relative_to(root).as_posix()
        (fixture / "main.c").write_text("int answer(void); int main(void) { return answer() != 42; }\n")
        (fixture / "answer.c").write_text('#include "answer.h"\nint answer(void) { return ANSWER; }\n')
        (fixture / "answer.h").write_text("#define ANSWER 42\n")
        output = relative + "/out"
        base = [args.make, "-f", "Makefile.cbm", output + "/test-runner",
                "BUILD_DIR=" + output, "CC=" + args.cc, "SANITIZE=",
                "TEST_CORE_SRCS=" + relative + "/main.c " + relative + "/answer.c",
                "ALL_TEST_SRCS=", "TEST_FOUNDATION_SRCS=", "OBJS_VENDORED_TEST=",
                "LDFLAGS_TEST="]

        if os.environ.get("MSYSTEM"):
            base.append("OS=windows")

        def make(extra=""):
            result = subprocess.run(base + ["CFLAGS_TEST=-std=c11 -Wall -Wextra -Werror " + extra],
                                    cwd=root, capture_output=True, text=True, timeout=90)
            assert result.returncode == 0, result.stdout + result.stderr
            return result.stdout

        def times():
            return {p.name: p.stat().st_mtime_ns for p in (fixture / "out").rglob("*.o")}

        def edit(name, content):
            # Keep timestamps apart on filesystems/make builds with one-second resolution.
            time.sleep(1.05)
            (fixture / name).write_text(content)

        make()
        first = times()
        assert set(first) == {"main.o", "answer.o"}, first
        warm = make()
        assert times() == first and " -o " not in warm, "warm build compiled or relinked"
        edit("answer.c", '#include "answer.h"\nint answer(void) { return ANSWER + 0; }\n')
        make()
        second = times()
        assert second["main.o"] == first["main.o"] and second["answer.o"] > first["answer.o"]
        edit("answer.h", "#define ANSWER (40 + 2)\n")
        make()
        third = times()
        assert third["main.o"] == second["main.o"] and third["answer.o"] > second["answer.o"]
        time.sleep(1.05)
        make("-DTEST_CONFIGURATION_CHANGED=1")
        fourth = times()
        assert all(fourth[name] > third[name] for name in third), "flags did not invalidate objects"
        executable = fixture / "out" / ("test-runner.exe" if os.name == "nt" else "test-runner")
        subprocess.run([str(executable)], check=True, timeout=10)
        print("PASS: no-op 0 compiles/links; source/header 1 compile; flags 2 compiles; executable correct")


if __name__ == "__main__":
    main()
