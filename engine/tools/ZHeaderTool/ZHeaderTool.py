#!/usr/bin/env python3
"""ZHeaderTool -- ZEngine reflection code generator (UHT-style, stdlib only).

Requires Python 3.9+. See zht/cli.py for the command-line contract and
cmake/ZHeaderTool.cmake for how the build invokes it.
"""

import os
import sys

if sys.version_info < (3, 9):
    sys.stderr.write("ZHeaderTool requires Python 3.9+, found %s\n" % sys.version.split()[0])
    sys.exit(1)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zht.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
