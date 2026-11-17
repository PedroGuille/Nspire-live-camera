#!/usr/bin/env python3
"""Report deterministic synthetic codec payloads; does not estimate hardware FPS."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
fixture = root / 'build' / 'fixtures.tsv'
if not fixture.exists():
    raise SystemExit('Run make test first')
names = ['Solid black', 'Ramp (all 256 colours)', 'Random RGB332',
         'Unchanged random image', 'Unchanged random image again',
         'One pixel changed', 'Solid white', 'Random four-colour image']
rows = [line.split('\t') for line in fixture.read_text().splitlines()]
print('| Synthetic 72×72 frame | Mode | Payload bytes | Reduction vs raw Base64 |')
print('| --- | --- | ---: | ---: |')
for name, row in zip(names, [row for row in rows if row[2] == '72']):
    size = len(row[5])
    print(f'| {name} | {row[3]} | {size:,} | {100 * (1 - size / 6912):.1f}% |')
print('\nPayload only: headers, packet acknowledgements, capture, rendering and USB latency excluded.')
