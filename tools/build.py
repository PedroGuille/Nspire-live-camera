#!/usr/bin/env python3
"""Embed the protocol module and optionally package a TI-Nspire document with Luna."""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--luna', help='path to the ndless-nspire/Luna executable')
args = parser.parse_args()
module = (root / 'camera_protocol.lua').read_text()
client = (root / 'readport3.0.lua').read_text()
marker = "local protocol = require('camera_protocol')"
assert client.count(marker) == 1
source = client.replace(marker, 'local protocol = (function()\n' + module + '\nend)()')
output = root / 'build' / 'camera_nc2.lua'
output.parent.mkdir(exist_ok=True)
output.write_text(source)
if args.luna:
    subprocess.run([args.luna, str(output), str(root / 'readport3.0.tns')], check=True)
print(output)
