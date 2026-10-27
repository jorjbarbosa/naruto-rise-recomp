"""Archive exact distributed binaries and matching debug files; never uploads sources."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import uuid


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def pe_debug(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE image: ' + path.name)
    sections, optional_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    directories = optional + (112 if struct.unpack_from('<H', data, optional)[0] == 0x20b else 96)
    debug_rva, debug_size = struct.unpack_from('<II', data, directories + 6 * 8)
    for i in range(sections):
        section = optional + optional_size + i * 40
        virtual_size, rva, raw_size, raw = struct.unpack_from('<IIII', data, section + 8)
        if rva <= debug_rva < rva + max(virtual_size, raw_size):
            offset = raw + debug_rva - rva
            for entry in range(offset, offset + debug_size, 28):
                kind, size, _, pointer = struct.unpack_from('<IIII', data, entry + 12)
                if kind == 2 and data[pointer:pointer + 4] == b'RSDS':
                    guid = str(uuid.UUID(bytes_le=data[pointer + 4:pointer + 20]))
                    age = struct.unpack_from('<I', data, pointer + 20)[0]
                    name = data[pointer + 24:pointer + size].split(b'\0')[0].decode('utf-8', errors='replace').replace('\\', '/').rsplit('/', 1)[-1]
                    return {'guid': guid, 'age': age, 'pdb': name}
    return None


def pdb_debug(path):
    data = path.read_bytes()
    if not data.startswith(b'Microsoft C/C++ MSF 7.00'):
        raise ValueError('Unsupported PDB format: ' + path.name)
    block_size, _, _, directory_size, _, block_map = struct.unpack_from('<6I', data, 32)
    blocks = (directory_size + block_size - 1) // block_size
    numbers = struct.unpack_from('<' + 'I' * blocks, data, block_map * block_size)
    directory = b''.join(data[n * block_size:(n + 1) * block_size] for n in numbers)[:directory_size]
    count = struct.unpack_from('<I', directory)[0]
    sizes = struct.unpack_from('<' + 'I' * count, directory, 4)
    cursor = 4 + count * 4
    for stream, size in enumerate(sizes):
        blocks = 0 if size == 0xffffffff else (size + block_size - 1) // block_size
        numbers = struct.unpack_from('<' + 'I' * blocks, directory, cursor)
        cursor += blocks * 4
        if stream == 1:
            info = b''.join(data[n * block_size:(n + 1) * block_size] for n in numbers)[:size]
            return {'guid': str(uuid.UUID(bytes_le=info[12:28])), 'age': struct.unpack_from('<I', info, 8)[0]}
    raise ValueError('PDB info stream missing: ' + path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('destination', type=Path, help='New archive directory, outside the player package')
    parser.add_argument('--symbol-root', action='append', type=Path, default=[])
    parser.add_argument('--allow-missing', action='store_true', help='Archive with explicit missing coverage; not a release gate')
    args = parser.parse_args()
    build, destination = args.build.resolve(strict=True), args.destination.resolve()
    if destination.exists():
        parser.error('Archive destination already exists')
    names = ('narutorise.exe', 'narutorise', 'rexruntime.dll', 'librexruntime.so', 'rexgpu-xenos.dll',
             'sentry.dll', 'crashpad_handler.exe', 'crashpad_handler', 'crashpad_wer.dll')
    binaries = [build / name for name in names if (build / name).is_file()]
    binaries += sorted(build.glob('narutorise_ai2c*.dll')) + sorted(build.glob('libnarutorise_ai2c*.so'))
    if not any(p.name in ('narutorise.exe', 'narutorise') for p in binaries):
        parser.error('Application binary missing')
    modules, selected, missing = [], set(binaries), []
    for binary in binaries:
        module = {'binary': binary.name, 'sha256': sha(binary), 'debug_file': None}
        if binary.suffix.lower() in ('.exe', '.dll'):
            identity = pe_debug(binary)
            module['codeview'] = identity
            candidates = [build / identity['pdb']] if identity else []
            if identity:
                for root in args.symbol_root:
                    candidates.extend(root.resolve(strict=True).rglob(identity['pdb']))
            for candidate in candidates:
                if not candidate.is_file():
                    continue
                info = pdb_debug(candidate)
                if info['guid'] == identity['guid'] and info['age'] == identity['age']:
                    selected.add(candidate)
                    module['debug_file'] = candidate.name
                    module['debug_sha256'] = sha(candidate)
                    break
        else:
            readelf = shutil.which('readelf')
            if readelf:
                output = subprocess.check_output([readelf, '-n', '-S', str(binary)], text=True)
                module['elf_identity'] = output
                if '.debug_info' in output or '.debug_line' in output:
                    module['debug_file'] = binary.name
                else:
                    for candidate in (Path(str(binary) + '.debug'), binary.with_suffix('.debug')):
                        if candidate.is_file():
                            selected.add(candidate)
                            module['debug_file'] = candidate.name
                            module['debug_sha256'] = sha(candidate)
                            break
        if module['debug_file'] is None:
            missing.append(binary.name)
        modules.append(module)
    if missing and not args.allow_missing:
        parser.error('Missing matching debug information: ' + ', '.join(missing))
    destination.mkdir(parents=True)
    hashes = {}
    for source in sorted(selected):
        if source.name in hashes and hashes[source.name] != sha(source):
            parser.error('Conflicting artifact filenames: ' + source.name)
        shutil.copy2(source, destination / source.name)
        hashes[source.name] = sha(source)
        assert sha(destination / source.name) == hashes[source.name]
    manifest = {'schema_version': 1, 'modules': modules, 'files': hashes,
                'missing_debug_information': missing, 'complete': not missing, 'source_files_included': False}
    (destination / 'symbols-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'Archived {len(modules)} modules and {len(hashes)} artifacts; missing debug information: {len(missing)}')


if __name__ == '__main__':
    main()
