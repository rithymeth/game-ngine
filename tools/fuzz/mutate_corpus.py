#!/usr/bin/env python3
"""Writes mutated copies of a fuzz corpus, then (with --run) replays each one on its own.

A stand-in for libFuzzer on a machine without Clang: it takes the checked-in seeds of one target, swaps tokens
for wrong-typed values, flips, deletes and inserts bytes, and reports the files that make the replay binary
exit non-zero (a crash or an abort). The first byte of a seed is kept most of the time, since several targets
use it to pick a loader.

  python3 tools/fuzz/mutate_corpus.py tools/fuzz/corpus/ui /tmp/mutants 3000 --seed 7 \
      --run build/tools/fuzz/aether_replay_ui

Seeds whose name contains "regress" are skipped (they are already known crashers).
"""
import argparse
import glob
import os
import random
import re
import subprocess
import sys

REPLACEMENTS = [b'5', b'"x"', b'[]', b'null', b'{}', b'-1', b'4294967296', b'1e999', b'true', b'[1]', b'[[],[]]', b'""', b'0']
TOKEN = re.compile(rb'"[^"]*"|-?\d+\.?\d*|true|false|null|\[[^\[\]]*\]|\{[^{}]*\}')


def mutate(seed: bytes, rng: random.Random) -> bytes:
    b = bytearray(seed)
    for _ in range(rng.randint(1, 4)):
        k = rng.random()
        if k < 0.35 and len(b) > 2:
            tokens = list(TOKEN.finditer(bytes(b[1:])))
            if tokens:
                m = rng.choice(tokens)
                b[1 + m.start():1 + m.end()] = rng.choice(REPLACEMENTS)
        elif k < 0.6 and len(b) > 2:
            b[rng.randrange(1, len(b))] = rng.randrange(256)
        elif k < 0.8 and len(b) > 3:
            j = rng.randrange(1, len(b))
            del b[j:j + rng.randint(1, 6)]
        else:
            j = rng.randrange(1, len(b) + 1)
            b[j:j] = bytes(rng.randrange(256) for _ in range(rng.randint(1, 4)))
    if rng.random() < 0.15:
        b[0] = rng.randrange(256)
    return bytes(b)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('corpus')
    ap.add_argument('out')
    ap.add_argument('count', type=int)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--run', metavar='REPLAY_BINARY', help='replay each mutant and list the ones that fail')
    args = ap.parse_args()

    rng = random.Random(args.seed)
    seeds = [open(f, 'rb').read() for f in sorted(glob.glob(os.path.join(args.corpus, '*'))) if 'regress' not in f]
    if not seeds:
        print('no seeds in', args.corpus, file=sys.stderr)
        return 2
    os.makedirs(args.out, exist_ok=True)
    files = []
    for i in range(args.count):
        path = os.path.join(args.out, f'm{i}')
        with open(path, 'wb') as f:
            f.write(mutate(rng.choice(seeds), rng))
        files.append(path)
    if not args.run:
        print(f'wrote {len(files)} mutants to {args.out}')
        return 0
    failures = 0
    for path in files:
        try:
            rc = subprocess.run([args.run, path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10).returncode
        except subprocess.TimeoutExpired:
            rc = 'timeout'
        if rc != 0:
            print('FAIL', rc, path)
            failures += 1
            if failures >= 5:
                break
    print(f'{len(files)} mutants, {failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
