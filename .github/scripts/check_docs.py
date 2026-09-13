"""Check local inline Markdown links/images in tracked project documentation.

External URLs and heading anchors are deliberately not fetched/validated.
"""
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit


def missing_links(root, file):
    text = file.read_text(encoding="utf-8")
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    errors = []
    for match in re.finditer(r"\]\((<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)", text):
        target = match[1].strip("<>")
        url = urlsplit(target)
        if url.scheme or url.netloc or not url.path:
            continue
        path = unquote(url.path)
        resolved = root / path.lstrip("/") if path.startswith("/") else file.parent / path
        if not resolved.exists():
            errors.append(f"{file.relative_to(root)}: missing {target}")
    # The README uses an HTML logo rather than Markdown image syntax.
    for target in re.findall(r'<img\b[^>]*\bsrc="([^"]+)"', text):
        if not urlsplit(target).scheme and not (file.parent / target).exists():
            errors.append(f"{file.relative_to(root)}: missing image {target}")
    return errors


if __name__ == "__main__":
    root = Path.cwd()
    tracked = subprocess.check_output(["git", "ls-files", "-z"]).decode().split("\0")
    files = [root / name for name in tracked if name.endswith(".md") and not name.startswith("third_party/")]
    errors = [error for file in files for error in missing_links(root, file)]
    print("\n".join(errors) if errors else f"Local links checked in {len(files)} Markdown files.")
    raise SystemExit(bool(errors))
