`v11-session.json` and `v11-card.json` are the exact dictionaries returned by
`session()` and `card()` in `hub/tests/test_sessions.py`, wrapped in the v1.1
event envelope specified by `docs/protocol.md`. Other malformed and boundary
fixtures are derived from these dictionaries in `test_v11.c` so each field can
be varied independently.
