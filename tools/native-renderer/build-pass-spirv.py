"""The SPIR-V of the passes of the native renderer besides the game's shaders, for its
Vulkan backend: the presentation with the gamma ramp, the UI layer and the resolves
(app/src/gpu/backend/*.hlsl), FXAA (backend/fxaa) and the output effects of the SDK
(backend/sdk_shaders/vulkan_spirv).

Usage: python -I build-pass-spirv.py <backend dir> <new output dir> --dxc <dxc.exe> --spirv-val <spirv-val.exe>
       [--store <dir>]

The own passes and FXAA are compiled from their HLSL, the source of the DXIL and DXBC
the D3D12 backend runs, in the environment of the game's shaders
(build-sdb-shader-spirv.py): Vulkan 1.2, the DX layout, -fvk-invert-y in a VS, and the
entry point named main. Their __spirv__ branch declares the Vulkan bindings. The SDK's
modules are taken as the SDK built them (glslang, SPIR-V 1.0), from the copies of its
headers; its vertex shader writes the rectangle of its push constants as given, in
Vulkan's clip space. Each module goes through spirv-val in the same environment and is
checked against the pipeline layout the Vulkan backend builds for it (LAYOUTS, the
root signatures of the D3D12 backend: root constants as push constants, a static
sampler as a sampler binding): one entry point, main, of its stage; every binding in
set 0 and in its layout with the same descriptor type (a storage image with the
format, a uniform buffer within the bytes); the push constants inside the range of its
stage. A pass that does not compile or validate, or breaks a check, is reported; the
caller fails the build. With --store, the SPIR-V of a pass is reused when its sources,
the arguments and DXC did not change, and the verdict of spirv-val when the module, its
arguments and spirv-val did not.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))  # shader_store (the scripts run with python -I).
import shader_store
import spirv_module

TARGET_ENVIRONMENT = 'vulkan1.2'
PROFILES = {'vs': 'vs_6_0', 'ps': 'ps_6_0', 'cs': 'cs_6_0'}
MODELS = {'vs': 'vertex', 'ps': 'fragment', 'cs': 'compute'}
# The pipeline layouts of the passes: the descriptor type of each binding of set 0 (with
# the format of a storage image, the bytes of a uniform buffer) and the push constant
# range [offset, end) of each stage. The sampler of fxaa and effect is the static
# sampler of D3D12: linear, clamp.
LAYOUTS = {
    'present': dict(bindings={0: ('sampled-image', None), 1: ('uniform-buffer', 4096)},
                    push_constants=dict(fragment=(0, 32))),
    'resolve': dict(bindings={0: ('sampled-image', None)}, push_constants=dict(fragment=(0, 8))),
    'fxaa': dict(bindings={0: ('storage-image', 'Rgb10A2'), 1: ('sampled-image', None), 2: ('sampler', 'linear-clamp')},
                 push_constants=dict(compute=(0, 16))),
    'effect': dict(bindings={0: ('sampled-image', None), 1: ('sampler', 'linear-clamp')},
                   push_constants=dict(vertex=(0, 16), fragment=(16, 48))),
    'ui': dict(bindings={0: ('sampled-image', None), 1: ('sampler', 'linear-clamp')},
               push_constants=dict(vertex=(0, 8))),
}
SDK_HEADERS = 'sdk_shaders/vulkan_spirv'
FXAA_SOURCES = ['fxaa/fxaa.hlsli', 'fxaa/FXAA3_11.h']


def hlsl(name, layout, stage, source, entry, defines=(), includes=()):
    return dict(name=name, layout=layout, stage=stage, origin='hlsl', sources=[source, *includes], entry=entry,
                defines=list(defines))


def sdk(name, stage):
    return dict(name=name, layout='effect', stage=stage, origin='sdk', sources=[f'{SDK_HEADERS}/{name}.h'])


# By the names of the D3D12 backend (kPresentVs as present_vs, the SDK's as they are).
PASSES = [
    hlsl('present_vs', 'present', 'vs', 'present_pass.hlsl', 'vs_main'),
    hlsl('present_ps', 'present', 'ps', 'present_pass.hlsl', 'ps_main'),
    hlsl('present_gamma_ps', 'present', 'ps', 'present_pass.hlsl', 'ps_gamma'),
    hlsl('present_gamma_luma_ps', 'present', 'ps', 'present_pass.hlsl', 'ps_gamma_luma'),
    hlsl('depth_resolve_vs', 'resolve', 'vs', 'depth_resolve.hlsl', 'vs_main'),
    hlsl('depth_resolve_ms_ps', 'resolve', 'ps', 'depth_resolve.hlsl', 'ps_main', ['MSAA=1']),
    hlsl('depth_resolve_single_ps', 'resolve', 'ps', 'depth_resolve.hlsl', 'ps_main', ['MSAA=0']),
    hlsl('sample_resolve_ps', 'resolve', 'ps', 'sample_resolve.hlsl', 'ps_main'),
    hlsl('fxaa_cs', 'fxaa', 'cs', 'fxaa/fxaa.cs.hlsl', 'main', includes=FXAA_SOURCES),
    hlsl('fxaa_extreme_cs', 'fxaa', 'cs', 'fxaa/fxaa_extreme.cs.hlsl', 'main', includes=FXAA_SOURCES),
    hlsl('ui_vs', 'ui', 'vs', 'ui_pass.hlsl', 'vs_main'),
    hlsl('ui_ps', 'ui', 'ps', 'ui_pass.hlsl', 'ps_main'),
    sdk('guest_output_triangle_strip_rect_vs', 'vs'),
    sdk('guest_output_bilinear_ps', 'ps'),
    sdk('guest_output_ffx_cas_sharpen_ps', 'ps'),
    sdk('guest_output_ffx_cas_resample_ps', 'ps'),
    sdk('guest_output_ffx_fsr_easu_ps', 'ps'),
    sdk('guest_output_ffx_fsr_rcas_ps', 'ps'),
]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('backend', type=Path)
parser.add_argument('out', type=Path)
parser.add_argument('--dxc', type=Path, required=True)
parser.add_argument('--spirv-val', type=Path, required=True)
parser.add_argument('--store', type=Path, help='the content-addressed store (shader_store.py); none if absent')
args = parser.parse_args()
dxc, spirv_val = args.dxc.resolve(), args.spirv_val.resolve()
backend, out = args.backend.resolve(), args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
for name in ('spirv', 'log'):
    (out / name).mkdir()
sha = lambda data: hashlib.sha256(data).hexdigest()
store = shader_store.Store(args.store)
dxc_id = shader_store.dxc_identity(dxc)
VALIDATOR = [str(spirv_val), '--target-env', TARGET_ENVIRONMENT]
validator_id = shader_store.files_identity([spirv_val])


def arguments(stage: str, entry: str, defines):
    return ['-T', PROFILES[stage], '-E', entry, '-HV', '2021', '-spirv', f'-fspv-target-env={TARGET_ENVIRONMENT}',
            '-fvk-use-dx-layout', '-fvk-stage-io-order=decl', *(['-fvk-invert-y'] if stage == 'vs' else []),
            '-fspv-entrypoint-name=main', '-Qstrip_debug', *(f'-D{d}' for d in defines)]


def source(name: str) -> bytes:
    """A source with LF line ends: its identity does not depend on the checkout."""
    return (backend / name).read_bytes().replace(b'\r\n', b'\n')


def header_module(header: Path) -> bytes:
    """The module of a SPIR-V header of the SDK (a uint32_t array)."""
    text = header.read_text(encoding='utf-8')
    body = text[text.index('{', text.index('[]')) + 1:text.rindex('}')]
    words = [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]
    return struct.pack(f'<{len(words)}I', *words)


def descriptor(binding: dict):
    """The descriptor type of a binding, as LAYOUTS states it."""
    if binding['storage'] == 'uniform':
        return 'uniform-buffer', binding['bytes']
    if binding['type'].startswith('storage-image'):
        return 'storage-image', binding['type'].rsplit(':', 1)[1]
    if binding['type'].startswith('image'):
        return 'sampled-image', None
    if binding['type'] == 'sampler':
        return 'sampler', None
    return binding['type'], None


def check(entry: dict, interface: dict):
    """The layout checks of one module; the names of those it breaks."""
    broken = []
    layout, model = LAYOUTS[entry['layout']], MODELS[entry['stage']]
    if [(e['model'], e['name']) for e in interface['entry_points']] != [(model, 'main')]:
        broken.append('entry-point')
    for b in interface['bindings']:
        kind, detail = descriptor(b)
        expected = layout['bindings'].get(b['binding']) if b['set'] == 0 else None
        if (not expected or kind != expected[0] or (kind == 'uniform-buffer' and detail > expected[1]) or
                (kind == 'storage-image' and detail != expected[1])):
            broken.append('binding')
    if interface['push_constant_bytes']:
        first, end = layout['push_constants'].get(model, (0, 0))
        if interface['push_constant_offset'] < first or interface['push_constant_bytes'] > end:
            broken.append('push-constant')
    return broken


def one(p: dict):
    entry = dict(p, sources={s: sha(source(s)) for s in p['sources']})
    spirv = out / 'spirv' / (p['name'] + '.spv')
    if p['origin'] == 'sdk':
        spirv.write_bytes(header_module(backend / p['sources'][0]))
    else:
        flags = arguments(p['stage'], p['entry'], p['defines'])
        entry['arguments'] = flags
        compiled = shader_store.key('dxc', *(source(s) for s in p['sources']), *flags, dxc_id)
        blob = store.get('pass-spirv', compiled)
        if blob is not None:
            spirv.write_bytes(blob)
            entry['dxc'] = 'store'
        else:
            run = subprocess.run([str(dxc), *flags, '-Fo', str(spirv), str(backend / p['sources'][0])],
                                 capture_output=True, timeout=300)
            if run.returncode or not spirv.exists():
                (out / 'log' / (p['name'] + '.dxc.txt')).write_bytes(run.stdout + run.stderr)
                first = next((l for l in (run.stdout + run.stderr).decode('utf-8', 'replace').splitlines()
                              if 'error:' in l), '')
                entry['first_error'] = first.split('error:', 1)[-1].strip()
                entry['result'] = 'compile-failed'
                return entry
            store.put('pass-spirv', compiled, spirv.read_bytes())
    module = spirv.read_bytes()
    entry['spirv_sha256'], entry['spirv_bytes'] = sha(module), len(module)
    validated = shader_store.key('spirv-val', module, *VALIDATOR[1:], validator_id)
    if store.get('validated', validated) is not None:
        entry['spirv_val'] = 'store'
    else:
        validation = subprocess.run([*VALIDATOR, str(spirv)], capture_output=True, timeout=300)
        if validation.returncode:
            (out / 'log' / (p['name'] + '.spirv-val.txt')).write_bytes(validation.stdout + validation.stderr)
            entry['first_error'] = (validation.stdout + validation.stderr).decode('utf-8', 'replace').strip()[:300]
            entry['result'] = 'validation-failed'
            return entry
        store.put('validated', validated, b'ok')
    entry['interface'] = spirv_module.parse(module)
    if p['stage'] == 'vs':
        entry['vertex_y'] = 'inverted (-fvk-invert-y)' if p['origin'] == 'hlsl' else 'as its push constants'
    broken = check(entry, entry['interface'])
    entry['result'] = 'check-failed' if broken else 'ok'
    if broken:
        entry['broken'] = broken
    return entry


started = datetime.datetime.now(datetime.timezone.utc)
results = [one(p) for p in PASSES]
finished = datetime.datetime.now(datetime.timezone.utc)
counts, broken = {}, {}
for r in results:
    counts[f"{r['stage']}:{r['result']}"] = counts.get(f"{r['stage']}:{r['result']}", 0) + 1
    for b in r.get('broken', ()):
        broken[f"{r['name']}:{b}"] = 1
ok = [r for r in results if r['result'] == 'ok']
union = lambda field: sorted({c for r in ok for c in r['interface'][field]})
layouts = {name: dict(set=0, bindings=[dict(binding=b, type=t, **({'detail': d} if d else {}))
                                       for b, (t, d) in sorted(l['bindings'].items())],
                      push_constants={stage: list(r) for stage, r in l['push_constants'].items()})
           for name, l in LAYOUTS.items()}
report = dict(started_at_utc=started.isoformat(), finished_at_utc=finished.isoformat(), backend=str(backend),
              target_environment=TARGET_ENVIRONMENT,
              arguments=dict(stages={s: arguments(s, '<entry>', ()) for s in PROFILES}, spirv_val=VALIDATOR[1:]),
              tools=dict(dxc=sha(dxc.read_bytes()), spirv_val=sha(spirv_val.read_bytes()),
                         script=sha(Path(__file__).read_bytes()),
                         spirv_module=sha(Path(spirv_module.__file__).read_bytes()), dxc_files=dxc_id),
              store=store.summary(), counts=dict(sorted(counts.items())), broken=dict(sorted(broken.items())),
              layouts=layouts, summary=dict(spirv_versions=sorted({r['interface']['version'] for r in ok}),
                                            capabilities=union('capabilities'), extensions=union('extensions')),
              results=results)
(out / 'report.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
print(json.dumps(dict(counts=report['counts'], broken=report['broken'], store=report['store'],
                      seconds=(finished - started).total_seconds(), summary=report['summary'],
                      errors={r['name']: r['first_error'] for r in results if r.get('first_error')}), indent=1))
