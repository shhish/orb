"""Pinned raw-storage choices for the six complete accessibility font sets.

The offline cost model balances every old/new union within the flash aperture
and signed assets partition. This changes storage only, never glyph data or IDs.
budget.py and test_pack.py enforce the resulting PSRAM/flash bounds.
"""

RAW_WEIGHTS = {
    "icons": {
        21: (500,),
        23: (500,),
        25: (400, 500, 600, 700),
        26: (400, 500, 600, 700),
        28: (500, 600, 700),
        29: (400, 500, 600, 700),
        31: (400, 500, 600, 700),
        33: (400, 500, 600, 700),
        36: (600,),
        38: (400, 500, 600, 700),
        42: (400, 500, 600, 700),
        52: (500, 600),
        59: (400, 500, 600),
        62: (500,),
        65: (400, 500, 700),
        69: (400,),
        72: (500, 600, 700),
        75: (400, 500),
    },
    "text": {
        28: (500, 600, 700),
        29: (400,),
        31: (400, 500, 600, 700),
        33: (400, 500, 600, 700),
        65: (400, 500, 700),
    },
    "tnum": {
        25: (500, 700),
        26: (400, 500, 600, 700),
        28: (400, 500, 600, 700),
        29: (400, 500, 600, 700),
        31: (400, 500, 600, 700),
        33: (400, 500, 600, 700),
        36: (400, 500, 600),
        38: (400, 600, 700),
        42: (500, 600, 700),
        49: (500,),
        52: (400, 500, 600, 700),
        59: (500, 600, 700),
        65: (400, 500, 600, 700, 800),
        69: (400, 500, 700),
        72: (400, 500, 600),
        75: (400, 500, 700),
    },
}


def uses_raw_record(kind: str, px: int, weight: int) -> bool:
    return kind == "clock" or weight in RAW_WEIGHTS.get(kind, {}).get(px, ())
