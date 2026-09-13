"""Conservative change detection; unknown history always runs the full build."""
import json
import os
from pathlib import PurePosixPath
import subprocess


def docs_only(paths):
    return bool(paths) and all(
        PurePosixPath(path).suffix.lower() == ".md"
        or (path.startswith("docs/screenshots/") and PurePosixPath(path).suffix.lower() in {".png", ".jpg", ".webp"})
        for path in paths
    )


def requires_build(event_name, event):
    if event_name not in {"push", "pull_request"}:
        return True
    try:
        base = event["pull_request"]["base"]["sha"] if event_name == "pull_request" else event["before"]
        if not base or set(base) == {"0"}:
            return True
        if event_name == "pull_request":
            base = subprocess.check_output(["git", "merge-base", base, "HEAD"], text=True).strip()
        changed = subprocess.check_output(["git", "diff", "--name-only", "--no-renames", "-z", base, "HEAD"])
        paths = changed.decode().rstrip("\0").split("\0") if changed else []
        return not docs_only(paths)
    except (KeyError, subprocess.CalledProcessError, UnicodeError):
        return True


if __name__ == "__main__":
    with open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8") as source:
        build = requires_build(os.environ["GITHUB_EVENT_NAME"], json.load(source))
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write(f"build={str(build).lower()}\n")
    print("Full build required" if build else "Documentation-only change")
