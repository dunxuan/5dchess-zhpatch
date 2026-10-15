# -*- coding: utf-8 -*-
"""Validate translation.tsv against ui_strings.tsv."""
import os
import sys

base = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(base, 'ui_strings.tsv')
dst = os.path.join(os.path.dirname(base), 'translation.tsv')


def load(path):
    raw = open(path, encoding='utf-8').read()
    lines = raw.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    return lines


inp = load(src)
out = load(dst)

errors = []
expected = len(inp) - 1
if len(out) != expected:
    errors.append('line count mismatch: out=%d expected=%d (input=%d incl. header)'
                  % (len(out), expected, len(inp)))

src_pairs = [l.split('\t', 1) for l in inp[1:]]
if any(len(p) != 2 for p in src_pairs):
    errors.append('input has a line without tab')

translated = 0
copied = 0
n = min(len(src_pairs), len(out))
for i in range(n):
    s_kind, s_en = src_pairs[i]
    o = out[i]
    parts = o.split('\t')
    if len(parts) != 2:
        errors.append('out line %d: expected exactly 1 tab, found %d' % (i + 1, len(parts) - 1))
        continue
    o_en, o_zh = parts
    if o_en != s_en:
        errors.append('out line %d: english mismatch\n  in : %r\n  out: %r' % (i + 1, s_en, o_en))
        continue
    if not o_zh:
        errors.append('out line %d: empty chinese' % (i + 1))
    if '\n' in o_zh or '\r' in o_zh:
        errors.append('out line %d: chinese contains raw newline' % (i + 1))
    # structural escapes must match position counts
    for esc in ('\\n', '\\r', '\\t', '\\\\'):
        if s_en.count(esc) != o_zh.count(esc):
            errors.append('out line %d: escape %s count en=%d zh=%d\n  en=%r'
                          % (i + 1, esc, s_en.count(esc), o_zh.count(esc), s_en[:120]))
    if s_en.count('@') != o_zh.count('@'):
        errors.append('out line %d: @ count en=%d zh=%d' % (i + 1, s_en.count('@'), o_zh.count('@')))
    # leading indentation of tutorial paragraphs must be preserved
    lead_en = len(s_en) - len(s_en.lstrip(' '))
    lead_zh = len(o_zh) - len(o_zh.lstrip(' '))
    if s_en[:1] == ' ' and lead_en != lead_zh:
        errors.append('out line %d: leading spaces en=%d zh=%d' % (i + 1, lead_en, lead_zh))
    if o_zh == s_en:
        copied += 1
    else:
        translated += 1

print('input lines (with header): %d' % len(inp))
print('input entries           : %d' % (len(inp) - 1))
print('output lines            : %d' % len(out))
print('translated              : %d' % translated)
print('copied (untranslatable) : %d' % copied)
if errors:
    print('FAILED: %d error(s)' % len(errors))
    for e in errors[:50]:
        print(' -', e)
    sys.exit(1)
print('PASS: every output line matches input english byte-for-byte; structure preserved')
