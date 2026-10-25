"""Index a local ASS subtitle collection without distributing the source files.

python -I tools/localization/extract-subtitle-corpus.py SUBTITLE_DIR OUTPUT_DIR --first 1 --last 81
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def extract(path, episode):
    raw = path.read_bytes()
    # Reject undecodable files instead of silently damaging terminology.
    text = raw.decode('utf-8-sig')
    fields, events, in_events = None, [], False
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith('['):
            in_events = line.strip().casefold() == '[events]'
        if not in_events:
            continue
        if line.startswith('Format:'):
            fields = [field.strip().casefold() for field in line.split(':', 1)[1].split(',')]
        if not line.startswith('Dialogue:'):
            continue
        if not fields or fields[-1] != 'text':
            raise ValueError(f'{path}:{number}: unsupported ASS event format')
        parts = line.split(':', 1)[1].lstrip().split(',', len(fields) - 1)
        if len(parts) != len(fields):
            raise ValueError(f'{path}:{number}: incomplete ASS event')
        event = dict(zip(fields, parts))
        content = event['text']
        if re.search(r'\\p[1-9]', content):
            continue  # ASS vector drawing, not a subtitle.
        content = re.sub(r'\{[^}]*\}', '', content)
        content = re.sub(r'\\[Nn]', ' ', content).replace(r'\h', ' ')
        content = ' '.join(content.split())
        if not content:
            continue
        style = event.get('style', '')
        if re.search(r'pesnya|karaoke|perevod|naruto_label', style, re.I):
            kind = 'song_or_credit'
        elif re.search(r'komment|seriya', style, re.I):
            kind = 'annotation_or_title'
        else:
            kind = 'dialogue'
        events.append({'episode': episode, 'file': path.name, 'line': number,
                       'start': event['start'], 'end': event['end'], 'style': style,
                       'name': event.get('name', ''), 'kind': kind, 'text': content})
    if not events:
        raise ValueError(f'{path}: no readable events')
    return events, {'episode': episode, 'file': path.name, 'sha256': hashlib.sha256(raw).hexdigest(),
                    'encoding': 'utf-8-sig', 'events': len(events)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('subtitles', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--first', type=int, default=1)
    parser.add_argument('--last', type=int, default=81)
    args = parser.parse_args()
    selected = {}
    for path in sorted(args.subtitles.glob('*.ass')):
        match = re.search(r'Naruto\s*-\s*(\d{3})\b', path.name, re.I)
        if match and args.first <= int(match[1]) <= args.last:
            episode = int(match[1])
            if episode in selected:
                raise ValueError(f'Duplicate episode {episode}')
            selected[episode] = path
    missing = sorted(set(range(args.first, args.last + 1)) - selected.keys())
    if missing:
        raise ValueError(f'Missing episodes: {missing}')
    events, files = [], []
    for episode, path in sorted(selected.items()):
        extracted, metadata = extract(path, episode)
        events.extend(extracted)
        files.append(metadata)
    args.output.mkdir(parents=True, exist_ok=True)
    write_json(args.output / 'corpus.json', events)
    manifest = {'schema': 'narutorise-subtitle-corpus-v1', 'language': 'ru',
                'collection': 'Alex & Julia / SOFCJ-Raws Naruto',
                'source_directory': str(args.subtitles.resolve()),
                'episode_range': [args.first, args.last], 'files': files,
                'events': len(events), 'event_kinds': dict(collections.Counter(e['kind'] for e in events)),
                'limits': ['Styles and Name fields are subtitle metadata, not verified game speaker identities.',
                           'Song/credit events are indexed separately and excluded from glossary matching.',
                           'Russian-only subtitles do not provide automatic English/Russian alignment.']}
    write_json(args.output / 'manifest.json', manifest)
    print(json.dumps({'episodes': len(files), 'events': len(events), 'kinds': manifest['event_kinds']}))


if __name__ == '__main__':
    main()
