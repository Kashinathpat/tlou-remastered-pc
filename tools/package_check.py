"""Checks a package folder holds no game file and nothing of this machine, then zips it.

usage: package_check.py <stage folder> <zip file>
The folder's name is the top folder in the zip.
"""
import sys
from pathlib import Path
import zipfile

# Formats the game comes in; art and icons are the game's too (Python's and Pillow's own stay).
FORBIDDEN = {".psarc", ".sprx", ".prx", ".elf", ".pkg", ".sfo", ".dds", ".bk2", ".bsf", ".at9", ".ico"}
FORBIDDEN_NAMES = {"eboot.bin", "tlou.ini", "launcher.json", "boot-linked.bin", "icon0.png", "pic0.png", "pic1.png"}
FORBIDDEN_FOLDERS = {"analysis", "user", "assets"}
ROOT = Path(__file__).resolve().parent.parent
# This machine: the user's folder name and where the repository is, in both slash forms.
PRIVATE = [Path.home().name.encode(), str(ROOT).encode(), ROOT.as_posix().encode()]


def main():
    stage, archive = Path(sys.argv[1]), Path(sys.argv[2])
    files = [p for p in sorted(stage.rglob("*")) if p.is_file()]
    bad = []
    for path in files:
        relative = path.relative_to(stage)
        inside_python = relative.parts[0] == "python"
        if path.name.lower() in FORBIDDEN_NAMES or set(relative.parts[:-1]) & FORBIDDEN_FOLDERS:
            bad.append(str(relative))
        elif path.suffix.lower() in FORBIDDEN and not inside_python:
            bad.append(str(relative))
        elif relative.parts[0] in ("launcher", "tools", "gow3_pc") or path.name in ("TLOU Launcher.exe", "TLOU Game.exe"):
            if path.suffix.lower() != ".dll" and any(word in path.read_bytes() for word in PRIVATE):
                bad.append(f"{relative} (a path of this machine)")
    if bad:
        raise SystemExit("Not packaged, these should not ship:\n  " + "\n  ".join(bad))
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as out:
        for path in files:
            out.write(path, Path(stage.name) / path.relative_to(stage))
    print(f"{len(files)} files, {sum(p.stat().st_size for p in files) >> 20} MiB -> {archive.stat().st_size >> 20} MiB")


if __name__ == "__main__":
    main()
