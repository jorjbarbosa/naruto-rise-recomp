"""Validate a flat translation dictionary against its original ID/text catalog.

Usage: python -I tools/localization/validate-translation.py original.json texts.json
       --report report.json [--glossary localization/pt-BR/glossary.json]

Completeness, UTF-8/UTF-16 limits and engine tokens are mandatory. Identical
strings and inconsistent duplicate-source translations are review candidates,
not automatic language-quality judgments.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import sys


def dictionary(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f'Duplicate JSON key {key}')
            result[key] = value
        return result
    result = json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=pairs)
    if not isinstance(result, dict):
        raise ValueError('Expected a flat ID/text dictionary')
    for key, value in result.items():
        if not re.fullmatch(r'0|[1-9][0-9]*', key) or int(key) > 0xFFFFFFFF or not isinstance(value, str):
            raise ValueError(f'Invalid ID or value type: {key}')
    return result


def tokens(text):
    return collections.Counter(re.findall(r'<[^>]*>', text))


def arguments(text):
    return collections.Counter(re.findall(r'%[1-9][0-9]*', text))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('translation', type=Path)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--glossary', type=Path)
    args = parser.parse_args()
    errors, warnings, unchanged = [], [], []
    try:
        source, translated = dictionary(args.original), dictionary(args.translation)
        for key in sorted(source.keys() - translated.keys(), key=int):
            errors.append({'id': key, 'problem': 'missing_id'})
        for key in sorted(translated.keys() - source.keys(), key=int):
            errors.append({'id': key, 'problem': 'unexpected_id'})
        duplicates = collections.defaultdict(list)
        for key in source.keys() & translated.keys():
            original, text = source[key], translated[key]
            if '\0' in text:
                errors.append({'id': key, 'problem': 'nul_character'})
            if '\ufffd' in text and '\ufffd' not in original:
                errors.append({'id': key, 'problem': 'unicode_replacement_character'})
            try:
                text.encode('utf-8', errors='strict')
                if len(text.encode('utf-16-le')) // 2 > 65535:
                    errors.append({'id': key, 'problem': 'text_unit_limit'})
            except UnicodeError:
                errors.append({'id': key, 'problem': 'invalid_unicode'})
            if original and not text:
                errors.append({'id': key, 'problem': 'empty_translation'})
            if tokens(original) != tokens(text):
                errors.append({'id': key, 'problem': 'changed_engine_tokens',
                               'expected': dict(tokens(original)), 'actual': dict(tokens(text))})
            if arguments(original) != arguments(text):
                errors.append({'id': key, 'problem': 'changed_numbered_arguments',
                               'expected': dict(arguments(original)), 'actual': dict(arguments(text))})
            if original == text:
                unchanged.append({'id': key, 'original': original})
            duplicates[original].append((key, text))
        if args.glossary:
            glossary = json.loads(args.glossary.read_text(encoding='utf-8-sig'))
            for entry in glossary['entries']:
                if entry['basis'] != 'user_decision':
                    continue
                terms = [entry['term'], *entry['aliases']]
                pattern = re.compile(r'(?<!\w)(?:' + '|'.join(re.escape(term) for term in terms) + r')(?!\w)', re.I)
                for key in source.keys() & translated.keys():
                    if pattern.search(source[key]):
                        target = re.sub(r'<[^>]*>', ' ', translated[key])
                        target = ' '.join(target.split()).casefold()
                        if entry['translation'].casefold() not in target:
                            warnings.append({'id': key, 'problem': 'user_term_needs_check',
                                             'term': entry['term'], 'expected': entry['translation'],
                                             'translation': translated[key]})
        for original, instances in duplicates.items():
            if len({text for _, text in instances}) > 1:
                warnings.append({'problem': 'duplicate_source_variants', 'original': original,
                                 'translations': [{'id': key, 'text': text} for key, text in instances]})
        report = {'valid': not errors, 'source_ids': len(source), 'translation_ids': len(translated),
                  'translated_differently': sum(source[key] != translated.get(key) for key in source.keys() & translated.keys()),
                  'unchanged': sorted(unchanged, key=lambda item: int(item['id'])),
                  'errors': sorted(errors, key=lambda item: (int(item.get('id', 0)), item['problem'])),
                  'review_candidates': warnings,
                  'original_sha256': hashlib.sha256(args.original.read_bytes()).hexdigest(),
                  'translation_sha256': hashlib.sha256(args.translation.read_bytes()).hexdigest()}
    except (OSError, ValueError) as error:
        report = {'valid': False, 'errors': [{'problem': str(error)}]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in report.items() if key not in ('unchanged', 'errors', 'review_candidates')}, indent=2))
    print(f"errors={len(report.get('errors', []))} review_candidates={len(report.get('review_candidates', []))}")
    return 0 if report['valid'] else 1


if __name__ == '__main__':
    sys.exit(main())
