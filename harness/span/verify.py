#!/usr/bin/env python3
"""Independent verifier for the single-line span-name task.

Behaviour only: imports the real Service from the arm's src (check.sh puts it on
PYTHONPATH first) and compares exact strings. It never inspects the source.
"""
import json
import sys


def verify(root):
    failures = []
    from tabletopmagnat.services.llm_service import Service
    from tabletopmagnat.types.dialog import Dialog
    from tabletopmagnat.types.messages import UserMessage

    def name(*contents):
        return Service._build_span_name(Dialog(messages=[UserMessage(content=c) for c in contents]))

    def check(description, got, want):
        if got != want:
            failures.append(f'{description}: expected {want!r}, got {got!r}')

    check('no messages', name(), 'Request: (empty)')
    check('whitespace only', name('\n\n\t   \r\n'), 'Request: (empty)')
    check('newlines collapsed', name('  hello\n  world  '), 'Request: hello world')
    check('tabs collapsed', name('a\t\t\tb'), 'Request: a b')
    check('carriage returns collapsed', name('a\r\nb'), 'Request: a b')
    check('single line unchanged', name('just a question'), 'Request: just a question')
    check('exactly 50 chars', name('x' * 50), 'Request: ' + 'x' * 50)
    check('51 chars truncated', name('y' * 51), 'Request: ' + 'y' * 50 + '...')
    # Order matters: collapsing happens before truncation. Raw length is 62 but
    # the collapsed form is 61, and only the first 50 collapsed chars survive.
    check('collapse before truncate', name('a' * 30 + '\n\n\n' + 'b' * 30),
          'Request: ' + 'a' * 30 + ' ' + 'b' * 19 + '...')
    # A message that is long only because of whitespace must not be truncated.
    check('padding does not count', name('   short   '), 'Request: short')
    return {'passed': not failures, 'failures': failures}


if __name__ == '__main__':
    result = verify(sys.argv[1] if len(sys.argv) > 1 else '.')
    print(json.dumps(result))
    raise SystemExit(0 if result['passed'] else 1)