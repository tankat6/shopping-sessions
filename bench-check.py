#!/usr/bin/env python3
"""Check challenge accounting invariants on benchmark output (not exact correctness)."""
import sys
from pathlib import Path
import duckdb

if len(sys.argv) != 2:
    raise SystemExit('Usage: py bench-check.py bench-output')
root = Path(sys.argv[1]).resolve()
u = str(root / 'user_daily' / '*.parquet').replace('\\', '/')
c = str(root / 'campaign_daily' / '*.parquet').replace('\\', '/')
con = duckdb.connect()
user = con.execute(f"SELECT count(*), coalesce(sum(sessions),0), coalesce(sum(revenue_cents),0) FROM read_parquet('{u}')").fetchone()
campaign = con.execute(f"SELECT count(*), coalesce(sum(sessions),0), coalesce(sum(revenue_cents),0), coalesce(sum(assisted_sessions),0), coalesce(sum(CASE WHEN campaign_id <> 'organic' THEN sessions ELSE 0 END),0) FROM read_parquet('{c}')").fetchone()
print(f'user_daily: rows={user[0]:,}, sessions={user[1]:,}, revenue_cents={user[2]:,}')
print(f'campaign_daily: rows={campaign[0]:,}, sessions={campaign[1]:,}, revenue_cents={campaign[2]:,}, assisted_sessions={campaign[3]:,}')
ok = user[0] > 0 and campaign[0] > 0 and user[1] == campaign[1] and user[2] == campaign[2] and campaign[3] >= campaign[4]
print('INVARIANTS PASS' if ok else 'INVARIANTS FAIL')
if not ok:
    sys.exit(1)
