"""Assign one numeric version to every job in the Continuous workflow."""
import os
from pathlib import Path
import re


def assign(series, counter):
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", series):
        raise ValueError("VERSION must contain major.minor")
    if not re.fullmatch(r"[1-9][0-9]*", counter):
        raise ValueError("Build counter must be a positive integer")
    return f"{series}.{counter}"


if __name__ == "__main__":
    version = assign(Path("VERSION").read_text().strip(), os.environ["GITHUB_RUN_NUMBER"])
    with open(os.environ["GITHUB_OUTPUT"], "a") as output:
        output.write(f"version={version}\n")
    print(version)
