"""Build the comparison page of the widescreen boots (comparacao.html).

Usage: python -I app/tests/widescreen_compare.py <root> [--baseline=<run>] [--regression=<run>]

<root> holds one folder per widescreen_boot_test.py run. The page shows each
captured step of every run side by side at the same height (a wider screen is
wider), an optional outline of the 16:9 middle, the 16:9 regression (the
baseline run against the regression run: identical, or the count of differing
pixels with a difference image and a before/after slider), and two tabs with
the screen changes of the log and the Flash catalog the runs drew. Images stay
next to the page (relative links); the page is a local file.
"""
import html
import json
from pathlib import Path
import re
import struct
import sys
import zlib

ORDER = ['baseline', '16:9', '16:10', '4:3', '21:9', 'monitor', '32:9']


def read_png(path):
    raw = path.read_bytes()
    at, data, width, height = 8, b'', 0, 0
    while at < len(raw):
        length = struct.unpack_from('>I', raw, at)[0]
        kind, chunk = raw[at + 4:at + 8], raw[at + 8:at + 8 + length]
        if kind == b'IHDR': width, height = struct.unpack_from('>II', chunk)
        if kind == b'IDAT': data += chunk
        at += length + 12
    rows = zlib.decompress(data)
    stride = width * 3 + 1
    if any(rows[y * stride] for y in range(height)): raise ValueError(f'{path}: filtered rows are not supported')
    return width, height, b''.join(rows[y * stride + 1:(y + 1) * stride] for y in range(height))


def write_png(path, width, height, rgb):
    def chunk(name, payload):
        return struct.pack('>I', len(payload)) + name + payload + struct.pack('>I', zlib.crc32(name + payload))
    scan = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND', b''))


def difference(a, b, out):
    wa, ha, pa = read_png(a)
    wb, hb, pb = read_png(b)
    if (wa, ha) != (wb, hb): return dict(size=[[wa, ha], [wb, hb]], differing=None)
    image = bytearray(pa)
    differing = 0
    for i in range(0, len(pa), 3):
        if max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2])) > 8:
            differing += 1
            image[i:i + 3] = b'\xff\x20\x20'
        else:
            gray = (pa[i] + pa[i + 1] + pa[i + 2]) // 9
            image[i:i + 3] = bytes((gray, gray, gray))
    write_png(out, wa, ha, bytes(image))
    return dict(size=[wa, ha], differing=differing, total=wa * ha)


def option(name, default=None):
    prefix = f'--{name}='
    return next((arg[len(prefix):] for arg in sys.argv[2:] if arg.startswith(prefix)), default)


def main():
    root = Path(sys.argv[1]).resolve()
    runs = []
    for folder in sorted(p for p in root.iterdir() if (p / 'identity.json').exists()):
        identity = json.loads((folder / 'identity.json').read_text(encoding='utf-8'))
        summary = json.loads((folder / 'summary.json').read_text(encoding='utf-8')) if (folder / 'summary.json').exists() else {}
        runs.append(dict(name=folder.name, folder=folder, identity=identity, summary=summary))
    baseline, regression = option('baseline'), option('regression')

    def rank(run):
        key = 'baseline' if run['name'] == baseline else run['identity'].get('aspect', '')
        return (ORDER.index(key) if key in ORDER else len(ORDER), run['name'])
    runs.sort(key=rank)
    steps = []
    for run in runs:
        for png in sorted(run['folder'].glob('*.png')):
            if png.stem not in steps: steps.append(png.stem)

    diffs = {}
    by_name = {run['name']: run for run in runs}
    if baseline in by_name and regression in by_name:
        (root / 'diff').mkdir(exist_ok=True)
        for step in steps:
            a, b = by_name[baseline]['folder'] / f'{step}.png', by_name[regression]['folder'] / f'{step}.png'
            if a.exists() and b.exists(): diffs[step] = difference(a, b, root / 'diff' / f'{step}.png')

    def label(run):
        identity = run['identity']
        name = 'main (16:9)' if run['name'] == baseline else identity.get('aspect', run['name'])
        client = identity.get('client', [0, 0])
        size = 'tela cheia' if client == [0, 0] else f'{client[0]}×{client[1]}'
        failure = run['summary'].get('failure')
        return f'{html.escape(name)} <span class="muted">{size} · {html.escape(run["name"])}</span>' + (
            f' <span class="bad">falhou</span>' if failure else '')

    sections = []
    for step in steps:
        cells = []
        for run in runs:
            png = run['folder'] / f'{step}.png'
            if not png.exists():
                cells.append(f'<figure class="shot missing"><div class="empty">sem captura</div><figcaption>{label(run)}</figcaption></figure>')
                continue
            rel = png.relative_to(root).as_posix()
            cells.append(f'<figure class="shot"><a href="{rel}" target="_blank"><span class="frame"><img src="{rel}" alt="{html.escape(step)}" '
                         f'loading="lazy"><span class="box"></span></span></a><figcaption>{label(run)}</figcaption></figure>')
        badge = ''
        if step in diffs:
            d = diffs[step]
            if d['differing'] is None:
                badge = '<span class="badge bad">tamanhos diferentes</span>'
            elif d['differing'] == 0:
                badge = '<span class="badge good">16:9 idêntico à main</span>'
            else:
                share = 100.0 * d['differing'] / d['total']
                base_rel = (by_name[baseline]['folder'] / f'{step}.png').relative_to(root).as_posix()
                new_rel = (by_name[regression]['folder'] / f'{step}.png').relative_to(root).as_posix()
                badge = (f'<span class="badge muted">16:9 vs main: {share:.1f}% dos pixels (animação e momento da captura variam)</span>'
                         f'<details><summary>diferença e antes/depois</summary><div class="pair">'
                         f'<img src="diff/{html.escape(step)}.png" alt="diferença">'
                         f'<div class="slider"><img src="{base_rel}" alt="main"><div class="top"><img src="{new_rel}" alt="novo"></div>'
                         f'<input type="range" min="0" max="100" value="50" aria-label="antes e depois"></div></div></details>')
        sections.append(f'<section><h2>{html.escape(step)} {badge}</h2><div class="row">{"".join(cells)}</div></section>')

    changes, movies = [], {}
    pattern = re.compile(r'widescreen movie "([^"]*)": rect \[([^\]]*)\] -> \[([^\]]*)\], catalog (\w+), anchors (\d+), group (\d+)')
    for run in runs:
        log = run['folder'] / 'narutorise.log'
        if not log.exists(): continue
        for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
            if any(tag in line for tag in ('widescreen:', 'widescreen window', 'widescreen target', 'widescreen resize')):
                changes.append((run['name'], line.strip()))
            match = pattern.search(line)
            if match:
                entry = movies.setdefault(match[1], dict(rects=set(), known=match[4], anchors=match[5], group=match[6], runs=set()))
                entry['rects'].add(match[2])
                entry['runs'].add(run['name'])
    proof = ''
    if regression in by_name:
        log = by_name[regression]['folder'] / 'narutorise.log'
        text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
        session = [l for l in text.splitlines() if 'widescreen:' in l and 'two ' not in l]
        changed_targets = [l for l in text.splitlines() if 'widescreen target' in l or 'widescreen resize' in l]
        moved = [m for m in pattern.finditer(text) if m[2] != m[3]]
        ok = bool(session) and all('1280x720' in l for l in session) and not changed_targets and not moved
        proof = (f'<p class="badge {"good" if ok else "bad"}">Regressão 16:9 ({html.escape(regression)}): '
                 f'{"nenhum gancho alterou o jogo" if ok else "o jogo foi alterado"}</p><ul>'
                 f'<li>tela da sessão: {html.escape(session[0].split("] ")[-1]) if session else "sem registro"}</li>'
                 f'<li>alvos redimensionados: {len(changed_targets)}</li>'
                 f'<li>filmes Flash com retângulo alterado: {len(moved)} de {len(list(pattern.finditer(text)))}</li></ul>')
    groups = {'0': '—', '1': 'mundo', '2': 'luta', '3': 'ambos'}
    change_rows = ''.join(f'<tr><td>{html.escape(r)}</td><td><code>{html.escape(l)}</code></td></tr>' for r, l in changes)
    movie_rows = ''.join(
        f'<tr class="{"" if v["known"] == "true" else "unknown"}"><td>{html.escape(k)}</td><td>{"sim" if v["known"] == "true" else "não"}</td>'
        f'<td>{v["anchors"]}</td><td>{groups.get(v["group"], v["group"])}</td><td>{len(v["rects"])}</td>'
        f'<td>{html.escape(", ".join(sorted(v["rects"])[:4]))}</td><td>{html.escape(", ".join(sorted(v["runs"])))}</td></tr>'
        for k, v in sorted(movies.items()))

    page = f'''<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Comparação ultrawide</title>
<style>
:root {{ --bg:#f6f5f2; --panel:#ffffff; --text:#1d1d1b; --muted:#6b6a66; --line:#d9d6cf; --accent:#d4561b;
  --good:#1f7a3a; --warn:#a15c00; --bad:#b3261e; --box:#00b7ff; }}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ --bg:#151513; --panel:#1f1f1c; --text:#ecebe6;
  --muted:#a3a19a; --line:#3a3934; --accent:#ff8a4c; --good:#5fd38a; --warn:#f0b35a; --bad:#ff8a80; --box:#4fd2ff; }} }}
:root[data-theme="dark"] {{ --bg:#151513; --panel:#1f1f1c; --text:#ecebe6; --muted:#a3a19a; --line:#3a3934;
  --accent:#ff8a4c; --good:#5fd38a; --warn:#f0b35a; --bad:#ff8a80; --box:#4fd2ff; }}
body {{ margin:0; padding:16px; background:var(--bg); color:var(--text); font:14px/1.45 system-ui, sans-serif; }}
header {{ display:flex; flex-wrap:wrap; gap:12px; align-items:center; margin-bottom:12px; }}
h1 {{ font-size:20px; margin:0 12px 0 0; }} h2 {{ font-size:15px; margin:18px 0 8px; display:flex; flex-wrap:wrap; gap:8px; align-items:center; }}
.tabs button, label.toggle {{ background:var(--panel); color:var(--text); border:1px solid var(--line); border-radius:6px; padding:6px 10px; cursor:pointer; }}
.tabs button[aria-selected="true"] {{ border-color:var(--accent); color:var(--accent); }}
.row {{ display:flex; gap:12px; overflow-x:auto; padding-bottom:6px; }}
.shot {{ margin:0; flex:none; }} .shot img {{ height:min(240px, 30vw); display:block; }}
.frame {{ position:relative; display:inline-block; }}
.box {{ display:none; position:absolute; border:2px dashed var(--box); pointer-events:none; }}
body.outline .box {{ display:block; }}
figcaption {{ font-size:12px; margin-top:4px; }} .muted {{ color:var(--muted); }}
.empty {{ height:min(240px, 30vw); width:160px; display:grid; place-items:center; border:1px dashed var(--line); color:var(--muted); }}
.badge {{ font-size:12px; border-radius:999px; padding:2px 8px; border:1px solid currentColor; font-weight:600; }}
.good {{ color:var(--good); }} .warn {{ color:var(--warn); }} .bad {{ color:var(--bad); }}
details {{ width:100%; }} .pair {{ display:flex; gap:12px; flex-wrap:wrap; margin-top:8px; }} .pair img {{ height:min(240px, 30vw); }}
.slider {{ position:relative; display:inline-block; }} .slider > img {{ display:block; }}
.slider .top {{ position:absolute; inset:0; overflow:hidden; width:50%; }} .slider .top img {{ display:block; }}
.slider input {{ position:absolute; left:0; right:0; bottom:4px; width:100%; }}
table {{ border-collapse:collapse; width:100%; background:var(--panel); }} td, th {{ border-bottom:1px solid var(--line); padding:4px 8px; text-align:left; vertical-align:top; }}
tr.unknown td:first-child {{ color:var(--warn); }} code {{ font-size:12px; word-break:break-all; }}
[hidden] {{ display:none !important; }}
</style></head><body>
<header><h1>Comparação ultrawide</h1>
<nav class="tabs" role="tablist"><button role="tab" aria-selected="true" data-tab="shots">Capturas</button>
<button role="tab" aria-selected="false" data-tab="changes">Trocas</button><button role="tab" aria-selected="false" data-tab="catalog">Catálogo</button></nav>
<label class="toggle"><input type="checkbox" id="outline"> Contorno 16:9</label></header>
<main><div id="shots">{proof}{"".join(sections) or "<p>Nenhuma captura.</p>"}</div>
<div id="changes" hidden><table><tr><th>Rodada</th><th>Linha do log</th></tr>{change_rows}</table></div>
<div id="catalog" hidden><p class="muted">Filmes Flash desenhados (widescreen_log). Os que não estão no catálogo ficam no 16:9 central.</p>
<table><tr><th>Filme</th><th>No catálogo</th><th>Âncoras</th><th>Grupo</th><th>Retângulos</th><th>Exemplos</th><th>Rodadas</th></tr>{movie_rows}</table></div></main>
<script>
document.querySelectorAll('.tabs button').forEach(b => b.addEventListener('click', () => {{
  document.querySelectorAll('.tabs button').forEach(o => o.setAttribute('aria-selected', o === b));
  ['shots','changes','catalog'].forEach(id => document.getElementById(id).hidden = id !== b.dataset.tab);
}}));
document.getElementById('outline').addEventListener('change', e => document.body.classList.toggle('outline', e.target.checked));
function place(img) {{
  const box = img.parentElement.querySelector('.box'); if (!box) return;
  const w = img.clientWidth, h = img.clientHeight, a = img.naturalWidth / img.naturalHeight;
  let bw = w, bh = h; if (a > 16/9) bw = h * 16 / 9; else bh = w * 9 / 16;
  Object.assign(box.style, {{ left: (w - bw) / 2 + 'px', top: (h - bh) / 2 + 'px', width: bw + 'px', height: bh + 'px' }});
}}
document.querySelectorAll('.frame img').forEach(img => img.complete ? place(img) : img.addEventListener('load', () => place(img)));
document.querySelectorAll('.slider').forEach(s => {{
  const top = s.querySelector('.top'), input = s.querySelector('input'), inner = top.querySelector('img'), under = s.querySelector(':scope > img');
  const sync = () => {{ top.style.width = input.value + '%'; inner.style.width = under.clientWidth + 'px'; }};
  input.addEventListener('input', sync); under.complete ? sync() : under.addEventListener('load', sync);
}});
</script></body></html>'''
    (root / 'comparacao.html').write_text(page, encoding='utf-8')
    print(root / 'comparacao.html')


if __name__ == '__main__':
    main()
