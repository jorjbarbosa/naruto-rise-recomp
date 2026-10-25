"""Content-addressed store of the shader cache steps, kept across builds.

The steps of build-shader-cache.py are pure functions of their inputs: the
translation of a container by the XenosRecomp fork, and DXC on a translated
HLSL. Each result is kept under the SHA-256 of everything that makes it (the
input bytes, the tool identity, the arguments), so a build whose fork, DXC and
scripts did not change reuses every shader and one that changed a single input
redoes only what depends on it. The tool identities are of the sources and
files that decide the output, not of the rebuilt XenosRecomp.exe (its bytes
change with the build directory). Writes are atomic (a temporary file in the
same folder, then a replace), so concurrent builds share the store safely. The
store derives from the game files and lives in a git-ignored folder.
"""
import hashlib
import os
import tempfile
import threading
import zlib
from pathlib import Path

VERSION = 'v1'


def key(*parts) -> str:
    """SHA-256 of the parts, each length-prefixed (bytes, str or Path contents are not read)."""
    h = hashlib.sha256(VERSION.encode())
    for part in parts:
        data = part if isinstance(part, bytes) else str(part).encode('utf-8')
        h.update(len(data).to_bytes(8, 'little'))
        h.update(data)
    return h.hexdigest()


def files_identity(paths) -> str:
    """SHA-256 of the names and bytes of the files, in the given order."""
    h = hashlib.sha256()
    for path in paths:
        path = Path(path)
        data = path.read_bytes()
        h.update(path.name.encode('utf-8') + b'\0' + len(data).to_bytes(8, 'little'))
        h.update(data)
    return h.hexdigest()


def dxc_identity(dxc: Path) -> str:
    """DXC and the compiler and validator it loads: from its folder on Windows
    (dxcompiler.dll, dxil.dll), from dxc-bin's lib/<arch> on Linux (libdxcompiler.so,
    libdxil.so)."""
    dxc = Path(dxc)
    if dxc.suffix == '.exe':
        libraries = [dxc.parent / name for name in ('dxcompiler.dll', 'dxil.dll')]
    else:
        libraries = [dxc.parent.parent.parent / 'lib' / dxc.parent.name / name
                     for name in ('libdxcompiler.so', 'libdxil.so')]
    return files_identity([dxc] + [path for path in libraries if path.exists()])


def fork_identity(common: Path) -> str:
    """The sources of the XenosRecomp fork (the folder of shader_common.h and its build files)."""
    folder = Path(common).parent
    files = sorted(p for p in folder.iterdir() if p.is_file() and p.suffix in ('.cpp', '.h', '.txt'))
    files += [p for p in (folder.parent / 'CMakeLists.txt',) if p.exists()]
    return files_identity(files)


class Store:
    """Blobs by kind and key; None as root disables the store (every get misses, put is a no-op)."""

    def __init__(self, root):
        self.root = Path(root).resolve() if root else None
        self.counts = {}  # kind -> [hits, misses]
        self._lock = threading.Lock()

    def _count(self, kind: str, hit: bool):
        with self._lock:
            self.counts.setdefault(kind, [0, 0])[0 if hit else 1] += 1

    def _path(self, kind: str, k: str) -> Path:
        return self.root / VERSION / kind / k[:2] / k

    def get(self, kind: str, k: str, compressed=False):
        if self.root:
            try:
                data = self._path(kind, k).read_bytes()
                data = zlib.decompress(data) if compressed else data
                self._count(kind, True)
                return data
            except (OSError, zlib.error):
                pass
        self._count(kind, False)
        return None

    def put(self, kind: str, k: str, data: bytes, compressed=False):
        if not self.root:
            return
        path = self._path(kind, k)
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, temp = tempfile.mkstemp(dir=path.parent, prefix='.tmp-')
        try:
            with os.fdopen(fd, 'wb') as out:
                out.write(zlib.compress(data, 6) if compressed else data)
            os.replace(temp, path)
        except BaseException:
            Path(temp).unlink(missing_ok=True)
            raise

    def summary(self) -> dict:
        return dict(root=str(self.root) if self.root else None,
                    **{kind: dict(hits=h, misses=m) for kind, (h, m) in sorted(self.counts.items())})
