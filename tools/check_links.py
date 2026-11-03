#!/usr/bin/env python3
"""Every markdown link in the repository resolves, and every cross-file anchor exists.

Run from the repository root (`just check-links`, and CI): exits non-zero with the
list of broken links. External URLs are not fetched - a link to a page that moved
is the page owner's problem; a link to a file that is not there is ours.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP = {".git", "third_party", "data", ".commandcode", "node_modules", "build", "dist",
        "magicstack-pgbench"}
LINK = re.compile(r"\]\((?!https?:|#)([^)#]+)(#[^)]+)?\)")
SRC = re.compile(r'src="(?!https?:)([^"]+)"')
HREF = re.compile(r'href="(?!https?:|#)([^"#]+)"')
HEADING = re.compile(r"^#{1,6} (.+)$", re.M)


def slug(heading):
    return re.sub(r"[^a-z0-9 -]", "", heading.lower()).replace(" ", "-")


def for_each_page():
    for path in sorted(ROOT.rglob("*.md")):
        if any(part in SKIP for part in path.relative_to(ROOT).parts):
            continue
        if "bench/results" in path.as_posix():
            continue
        yield path


def main():
    broken = []
    for page in for_each_page():
        text = page.read_text()
        links = [(link, anchor) for link, anchor in LINK.findall(text)]
        links += [(target, "") for target in SRC.findall(text)]
        links += [(target, "") for target in HREF.findall(text)]
        for link, anchor in links:
            target = (page.parent / link).resolve()
            if not target.exists():
                broken.append(f"{page.relative_to(ROOT)}: missing {link}")
                continue
            if anchor and target.suffix == ".md":
                slugs = {slug(h) for h in HEADING.findall(target.read_text())}
                if anchor[1:] not in slugs:
                    broken.append(f"{page.relative_to(ROOT)}: no anchor {link}{anchor}")
    if broken:
        print("\n".join(broken))
        sys.exit(1)
    print("markdown links: all resolve")


if __name__ == "__main__":
    main()
