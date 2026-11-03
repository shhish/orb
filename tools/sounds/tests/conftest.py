"""Keep the owner's personal dev settings (~/.config/orb/dev.env) out of tests,
so they run against the repo's generic defaults on every machine."""

import os

os.environ["ORB_DEV_ENV"] = os.devnull
