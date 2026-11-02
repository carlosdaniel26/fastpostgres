#!/usr/bin/env python3
"""The release version lives in three places; this proves they agree.

`python/pyproject.toml` is what the wheel is called, `extension/fastpostgres.control`
is what `CREATE EXTENSION` installs, and the SQL file is named after the version
PostgreSQL finds in that control file. A mismatch is a release that half-installs.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    pyproject = (ROOT / "python" / "pyproject.toml").read_text()
    wheel = re.search(r'^version = "([^"]+)"$', pyproject, re.M)
    control = (ROOT / "extension" / "fastpostgres.control").read_text()
    extension = re.search(r"^default_version = '([^']+)'$", control, re.M)
    meta = json.loads((ROOT / "extension" / "META.json").read_text())

    versions = {
        "python/pyproject.toml": wheel.group(1) if wheel else None,
        "extension/fastpostgres.control": extension.group(1) if extension else None,
        "extension/META.json": meta.get("version"),
    }
    distinct = {v for v in versions.values() if v}
    if len(distinct) != 1 or None in versions.values():
        for where, version in versions.items():
            print(f"  {where}: {version}")
        sys.exit("check_versions: the release version disagrees")

    version = distinct.pop()
    sql = ROOT / "extension" / "sql" / f"fastpostgres--{version}.sql"
    if not sql.exists():
        sys.exit(f"check_versions: {sql.relative_to(ROOT)} is missing")
    print(f"version {version}: pyproject.toml, the control file, META.json and sql/ agree")


if __name__ == "__main__":
    main()
