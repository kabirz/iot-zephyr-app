"""Ensure tests/ is importable as rootdir for `import config`."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
