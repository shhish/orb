"""Import the font generator's standalone modules without changing the install."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
