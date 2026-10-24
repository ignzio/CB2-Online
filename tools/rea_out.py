"""Prints only the Rizin stdout from a saved REA execute_rizin_command evidence file.

Usage: python rea_out.py <saved-file> [max_lines]
"""
import json
import re
import sys

text = open(sys.argv[1], encoding='utf-8').read()
evidence = json.loads(text[text.index('{'):])
raw = evidence['raw_result']
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 10**9
for n, line in enumerate(raw['stdout'].splitlines()):
    if n >= limit:
        break
    print(re.sub(r'\s+', ' ', line).strip())
if raw.get('stderr', '').strip():
    print('[stderr]', raw['stderr'].strip()[:200])
