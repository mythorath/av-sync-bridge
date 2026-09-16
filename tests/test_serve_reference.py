# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('serve_reference', Path(__file__).resolve().parents[1] / 'tools' / 'serve-reference.py')
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


class ReferenceRoutes(unittest.TestCase):
    def test_explicit_module_mime(self):
        self.assertEqual(server.asset_for_path('/reference-timeline.mjs')[1], 'text/javascript; charset=utf-8')
        self.assertEqual(server.asset_for_path('/av-reference.html?version=2')[0], 'av-reference.html')

    def test_no_arbitrary_paths_or_remote_targets(self):
        for path in ('/../README.md', '/%2e%2e/README.md', '/secret', '/reference-timeline.mjs/..',
                     '//example.org/av-reference.html', 'https://example.org/av-reference.html', '/tools/', '/?file=secret/..'):
            if path.startswith('/?'):
                self.assertEqual(server.asset_for_path(path)[0], 'av-reference.html')
            else:
                self.assertIsNone(server.asset_for_path(path), path)

    def test_only_public_assets(self):
        self.assertEqual({asset[0] for asset in server.ASSETS.values()}, {'av-reference.html', 'reference-timeline.mjs'})


if __name__ == '__main__':
    unittest.main()
