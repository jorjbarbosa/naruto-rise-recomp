"""Export the project's glossary for spreadsheets and AI translation.

python -I tools/localization/export-glossary.py [--catalog path/to/english-texts.json]

The optional local catalog adds a coverage report; its game text is never
copied into the distributable glossary. No translation service is contacted.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path)
    args = parser.parse_args()
    root = repo / 'localization/pt-BR'
    path = root / 'glossary.json'
    glossary = json.loads(path.read_text(encoding='utf-8'))
    entries, sources = glossary['entries'], glossary['sources']
    ids, terms = set(), set()
    for entry in entries:
        assert entry['id'] not in ids, entry['id']
        ids.add(entry['id'])
        assert entry['status'] in ('preferred', 'contextual', 'needs_review')
        assert entry['translation'] and all(ref in sources for ref in entry['source_refs'])
        for term in [entry['term'], *entry.get('aliases', [])]:
            assert term.casefold() not in terms, term
            terms.add(term.casefold())
        if entry['basis'] == 'user_decision':
            assert entry['id'] in glossary['user_decisions']
    preferred = [entry for entry in entries if entry['status'] == 'preferred']
    with (root / 'glossary.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=['en', 'pt-BR', 'aliases', 'category', 'status', 'basis', 'notes', 'sources'], delimiter=';')
        writer.writeheader()
        for entry in entries:
            writer.writerow({'en': entry['term'], 'pt-BR': entry['translation'],
                             'aliases': ' | '.join(entry.get('aliases', [])), 'category': entry['category'],
                             'status': entry['status'], 'basis': entry['basis'], 'notes': entry['notes'],
                             'sources': ' | '.join(sources[ref]['url'] for ref in entry['source_refs'])})
    compact = lambda entry: {key: entry[key] for key in ('term', 'translation', 'aliases', 'notes')}
    context = {'source_language': 'en', 'target_language': 'pt-BR',
               'glossary_version': glossary['version'], 'glossary_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
               'rules': glossary['translation_rules'],
               'preferred_terms': [compact(entry) for entry in preferred],
               'contextual_terms': [compact(entry) for entry in entries if entry['status'] == 'contextual'],
               'unresolved_terms': [compact(entry) for entry in entries if entry['status'] == 'needs_review']}
    write_json(root / 'ai-context.json', context)
    statuses = dict(collections.Counter(entry['status'] for entry in entries))
    result = {'entries': len(entries), 'statuses': statuses, 'sources': len(sources)}
    if args.catalog:
        rows = json.loads(args.catalog.read_text(encoding='utf-8'))
        hits, covered = [], set()
        for entry in entries:
            pattern = '|'.join(re.escape(term) for term in sorted([entry['term'], *entry['aliases']], key=len, reverse=True))
            expression = re.compile(r'(?<!\w)(?:' + pattern + r')(?!\w)', re.I)
            matches = [row['id'] for row in rows if expression.search(re.sub(r'<[^>]*>', '', row['text']))]
            hits.append({'glossary_id': entry['id'], 'status': entry['status'], 'count': len(matches), 'text_ids': matches})
            if entry['status'] == 'preferred':
                covered.update(matches)
        report = {'catalog_sha256': hashlib.sha256(args.catalog.read_bytes()).hexdigest(),
                  'catalog_entries': len(rows), 'preferred_term_candidate_texts': len(covered),
                  'match_limit': 'Lexical candidates only; no speaker, scene or correct sense is inferred.', 'terms': hits}
        output = args.catalog.parent / 'glossary-coverage.json'
        write_json(output, report)
        result.update(coverage_report=str(output), preferred_term_candidate_texts=len(covered))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
