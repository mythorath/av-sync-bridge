#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded generated-only controller matrix; no devices or production configuration."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--long", action="store_true", help="600 simulated seconds per positive case")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    executable = str(args.executable.resolve(strict=True))
    cases = []
    for scenario, ppm, seconds in [*(('constant', p, 12) for p in (0, 100, -100, 499, -499)),
                                    ('step', 0, 45), ('ramp', 0, 125)]:
        for varied in (False, True):
            cases.append((scenario, ppm, 600 if args.long else seconds, varied, False))
    cases.append(('constant', -499, 60, False, True))

    def run(case):
        scenario, ppm, seconds, varied, negative = case
        command = [executable, '--scenario', scenario, '--ppm', str(ppm), '--seconds', str(seconds)]
        if varied:
            command += ['--varied-packets', '--pull-frames', '17']
        if negative:
            command += ['--incorrect-anchors']
        result = subprocess.run(command, capture_output=True, text=True, timeout=180, check=True)
        data = json.loads(result.stdout)
        if data['accepted'] != (not negative) or data['marker_count'] != 12:
            raise RuntimeError(f"unexpected verdict: {data}")
        if not all(math.isfinite(v) for v in data['marker_errors_ms']):
            raise RuntimeError('non-finite marker measurement')
        if negative and data['max_marker_error_ms'] <= 5:
            raise RuntimeError('negative control did not detect phase failure')
        return data

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = []
        for result in pool.map(run, cases):
            results.append(result)
            print(json.dumps(result, sort_keys=True), flush=True)
    differences = []
    for a, b in zip(results[0:-1:2], results[1:-1:2]):
        delta = max(abs(x-y) for x, y in zip(a['marker_errors_ms'], b['marker_errors_ms'], strict=True))
        differences.append(delta)
        if delta > 1000 / 48000 + .000002:  # One sample plus printed rounding.
            raise RuntimeError(f"packet/output partition phase failure: {delta} ms")
    print(json.dumps({'positive_cases': 14, 'negative_controls': 1,
                      'long': args.long, 'max_partition_phase_difference_ms': max(differences),
                      'accepted': True}), flush=True)


if __name__ == '__main__':
    main()
