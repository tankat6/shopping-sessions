#!/usr/bin/env python3
"""Shopping sessions pipeline."""
import csv
import glob
import json
import mmap
import re
import os
import sys
import time
import subprocess
from datetime import datetime, timezone, timedelta
from pathlib import Path

import duckdb

DATA = os.environ.get('DATA_DIR', '/data')
OUT = os.environ.get('OUTPUT_DIR', '/output')
SCRATCH = os.environ.get('SCRATCH_DIR', '/scratch')
NCPU = int(os.environ.get('NCPU', '8'))
MEM_GB = int(os.environ.get('MEM_GB', '24'))
WEEK = 604800000


def log(message):
    print(message, file=sys.stderr, flush=True)


def profile_stage(name, seconds):
    msg = f'PROFILE_STAGE {name} {seconds:.3f}s'
    log(msg)
    with open(os.path.join(SCRATCH, 'profile-timings.txt'), 'a', encoding='utf-8') as f:
        f.write(msg + '\n')


def dialect_fields(line, expected):
    """Parse strict CSV fields."""
    if not line or line == b'\n':
        return None
    if line.endswith(b'\n'):
        line = line[:-1]
    cells = line.split(b',')
    if len(cells) != expected:
        return None
    result = []
    for cell in cells:
        if cell.startswith(b'"') and cell.endswith(b'"') and len(cell) >= 2:
            cell = cell[1:-1]
        if b'"' in cell:
            return None
        try:
            result.append(cell.decode('utf-8'))
        except UnicodeDecodeError:
            return None
    return result






# Accept only the specified timestamp formats.
CLICK_ISO = re.compile(
    r"^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})"
    r"(Z|([+-])([0-9]{2}):([0-9]{2}))$"
)
CLICK_EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


def exact_click_ts(value):
    """Convert a valid timestamp to UTC milliseconds."""
    if not value:
        return None
    if re.fullmatch(r'[0-9]+', value):
        
        
        
        try:
            return int(value)
        except ValueError:
            return None
    m = CLICK_ISO.fullmatch(value)
    if m is None:
        return None
    year, month, day, hour, minute, second = map(int, m.group(1, 2, 3, 4, 5, 6))
    if m.group(7) == 'Z':
        offset = timedelta(0)
    else:
        off_hour, off_minute = int(m.group(9)), int(m.group(10))
        if off_hour > 23 or off_minute > 59:
            return None
        direction = 1 if m.group(8) == '+' else -1
        offset = direction * timedelta(hours=off_hour, minutes=off_minute)
    try:
        local = datetime(year, month, day, hour, minute, second,
                         tzinfo=timezone(offset))
        delta = local.astimezone(timezone.utc) - CLICK_EPOCH
    except (ValueError, OverflowError):
        return None
    return (delta.days * 86400 + delta.seconds) * 1000 + delta.microseconds // 1000


def verified_click_file(window_start, window_end):
    """Validate and stage click records."""
    output = os.path.join(SCRATCH, 'verified_clicks.csv')
    n_written = 0
    with open(output, 'w', encoding='utf-8', newline='') as outf:
        writer = csv.writer(outf, lineterminator='\n')
        writer.writerow(['click_id', 'user_id', 'campaign_id', 'tms'])
        for path in sorted(glob.glob(os.path.join(DATA, 'clicks', '*.csv'))):
            with open(path, 'rb') as inp:
                next(inp, None)  
                for line in inp:
                    cells = dialect_fields(line, 4)
                    if cells is None:
                        continue
                    click_id, user_id, campaign_id, ts = cells
                    if not click_id or not user_id:
                        continue
                    tms = exact_click_ts(ts)
                    if tms is None or not (window_start - WEEK <= tms < window_end):
                        continue
                    
                    
                    writer.writerow((click_id.replace('\r', '\ue000'),
                                     user_id.replace('\r', '\ue000'),
                                     campaign_id.replace('\r', '\ue000'), tms))
                    n_written += 1
    log(f'Exact-valid click rows: {n_written}')
    return output


def valid_event_csv_line(raw_line):
    """Check event row structure."""
    line = raw_line[:-1] if raw_line.endswith(b'\n') else raw_line
    if not line or line.count(b',') != 6:
        return False
    
    for field in line.split(b','):
        if field.startswith(b'"') and field.endswith(b'"') and len(field) >= 2:
            if b'"' in field[1:-1]:
                return False
        elif b'"' in field:
            return False
    return True


def verified_event_files():
    """Stage well-formed event records."""
    sources = sorted(glob.glob(os.path.join(DATA, 'events', '*.csv')))
    if not sources:
        raise RuntimeError('No event files found')
    root = Path(SCRATCH) / 'verified_events'
    root.mkdir(parents=True, exist_ok=True)
    paths = []
    discarded = 0
    retained = 0
    header = b'event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents\n'
    marker = '\ue000'.encode('utf-8')
    for index, source in enumerate(sources):
        dest = root / f'part-{index:06}.csv'
        paths.append(str(dest))
        with open(source, 'rb') as inp, open(dest, 'wb') as outf:
            next(inp, None)  
            outf.write(header)
            for line in inp:
                if not valid_event_csv_line(line):
                    discarded += 1
                    continue
                if b'\r' in line:
                    line = line.replace(b'\r', marker)
                outf.write(line if line.endswith(b'\n') else line + b'\n')
                retained += 1
    log(f'Strict event CSV records: {retained} retained; {discarded} malformed discarded')
    return paths


def build_link_map():
    parent = {}
    rank = {}
    def find(x):
        if x not in parent:
            parent[x] = x
            rank[x] = 0
            return x
        r = x
        while parent[r] != r:
            r = parent[r]
        while parent[x] != x:
            next_x = parent[x]
            parent[x] = r
            x = next_x
        return r

    for path in sorted(glob.glob(os.path.join(DATA, 'links', '*.csv'))):
        with open(path, 'rb') as stream:
            next(stream, None)  
            for line in stream:
                cells = dialect_fields(line, 2)
                if not cells or not cells[0] or not cells[1]:
                    continue
                a, b = cells
                ra, rb = find(a), find(b)
                if ra == rb:
                    continue
                if rank[ra] < rank[rb]:
                    ra, rb = rb, ra
                parent[rb] = ra
                if rank[ra] == rank[rb]:
                    rank[ra] += 1
    smallest = {}
    for x in parent:
        root = find(x)
        if root not in smallest or x.encode('utf-8') < smallest[root].encode('utf-8'):
            smallest[root] = x
    path = os.path.join(SCRATCH, 'identity_map.csv')
    with open(path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f, lineterminator='\n')
        writer.writerow(['raw_id', 'person_id'])
        for x in parent:
            writer.writerow((x.replace('\r', '\ue000'), smallest[find(x)].replace('\r', '\ue000')))
    return path



def csv_paths_preserving_cr(folder, label):
    """Preserve carriage returns in CSV input."""
    paths = []
    destination = Path(SCRATCH) / f'csv_cr_{label}'
    for index, source in enumerate(sorted(glob.glob(os.path.join(DATA, folder, '*.csv')))):
        with open(source, 'rb') as f:
            data = f.read()
        if b'\r' not in data:
            paths.append(source)
            continue
        destination.mkdir(parents=True, exist_ok=True)
        target = destination / f'part-{index:06d}.csv'
        with open(target, 'wb') as outf:
            outf.write(data.replace(b'\r', '\ue000'.encode('utf-8')))
        paths.append(str(target))
    return paths


def sql_string(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    start = time.monotonic()
    Path(SCRATCH).mkdir(parents=True, exist_ok=True)
    Path(OUT, 'user_daily').mkdir(parents=True, exist_ok=True)
    Path(OUT, 'campaign_daily').mkdir(parents=True, exist_ok=True)
    with open(os.path.join(DATA, 'manifest.json')) as stream:
        manifest = json.load(stream)
    window_start = int(datetime.fromisoformat(manifest['window_start'].replace('Z', '+00:00')).timestamp() * 1000)
    window_end = int(datetime.fromisoformat(manifest['window_end'].replace('Z', '+00:00')).timestamp() * 1000)
    log('Resolving identity components')
    mapping = build_link_map()
    log(f'Identity map ready in {time.monotonic()-start:.2f}s')

    con = duckdb.connect()
    con.execute(f'SET threads={NCPU}')
    con.execute(f"SET memory_limit='{max(1, int(MEM_GB * .75))}GB'")
    con.execute(f"SET temp_directory={sql_string(os.path.join(SCRATCH, 'duckdb_tmp'))}")
    con.execute('SET preserve_insertion_order=false')
    profile = os.environ.get('PROFILE_SQL', '') == '1'
    def run(statement):
        started = time.monotonic()
        con.execute(statement)
        if profile:
            elapsed = time.monotonic() - started
            match = re.search(r'CREATE\s+TEMP\s+TABLE\s+(\w+)', statement, re.I)
            if match:
                label = match.group(1)
            elif re.search(r'COPY\s*\(', statement, re.I):
                label = 'output_user_daily' if 'user_daily' in statement else 'output_campaign_daily'
            else:
                label = 'sql_other'
            profile_stage(label, elapsed)

    def read_csv(path_glob, names, disable_quotes=False):
        
        
        cols = ', '.join(f'{sql_string(name)}: \'VARCHAR\'' for name in names)
        col_object = '{' + cols + '}'
        return (f'read_csv({('[' + ', '.join(sql_string(p) for p in path_glob) + ']') if isinstance(path_glob, list) else sql_string(path_glob)}, header=true, auto_detect=false, '
                f"columns={col_object}, delim={sql_string(chr(44))}, quote={sql_string('' if disable_quotes else chr(34))}, escape={sql_string('' if disable_quotes else chr(34))}, "
                f'ignore_errors=true, strict_mode=false, null_padding=false, '
                f"nullstr='\\0', parallel=true)")

    links = read_csv(mapping, ['raw_id', 'person_id'])
    def strict_simple_csv(folder, label, names):
        """Read restricted CSV records."""
        paths = csv_paths_preserving_cr(folder, label)
        if not paths:
            raise RuntimeError(f'No input CSV files found in {folder}')
        raw = read_csv(paths, names, disable_quotes=True)
        quote_pattern = sql_string(r'^[^"]*$|^"[^"]*"$')
        valid = ' AND '.join(
            f'({name} IS NOT NULL AND regexp_full_match({name}, {quote_pattern}))'
            for name in names
        )
        cols = ', '.join(
            f"CASE WHEN starts_with({name}, chr(34)) THEN substr({name}, 2, length({name})-2) ELSE {name} END AS {name}"
            for name in names
        )
        return f'(SELECT {cols} FROM {raw} WHERE {valid})'

    
    
    
    
    # Use the native reader when available.
    fast_parser = Path(__file__).resolve().parent / 'fast_csv'
    if fast_parser.is_file() and os.access(fast_parser, os.X_OK):
        verify_start = time.monotonic()
        subprocess.run([str(fast_parser), DATA, SCRATCH,
                        str(window_start), str(window_end)], check=True)
        event_inputs = sorted(glob.glob(os.path.join(SCRATCH, 'verified_events', '*.csv')))
        click_input = sorted(glob.glob(os.path.join(SCRATCH, 'verified_clicks', '*.csv')))
        if profile:
            profile_stage('native_csv_preprocessing', time.monotonic() - verify_start)
    else:
        verify_start = time.monotonic()
        event_inputs = verified_event_files()
        if profile:
            profile_stage('verify_event_csv', time.monotonic() - verify_start)
        verify_start = time.monotonic()
        click_input = verified_click_file(window_start, window_end)
        if profile:
            profile_stage('verify_click_csv', time.monotonic() - verify_start)
    events = read_csv(event_inputs,
                      ['event_id', 'user_id', 'ts', 'ingest_ts',
                       'event_type', 'product_id', 'price_cents'])
    clicks = read_csv(click_input,
                      ['click_id', 'user_id', 'campaign_id', 'tms'])
    products = read_csv(os.path.join(DATA, 'products.csv'), ['product_id', 'category'])
    campaigns = read_csv(os.path.join(DATA, 'campaigns.csv'), ['campaign_id', 'channel'])

    run(f'CREATE TEMP TABLE identities AS SELECT * FROM {links}')
    run(f'CREATE TEMP TABLE products AS SELECT * FROM {products}')
    run(f'CREATE TEMP TABLE campaigns AS SELECT * FROM {campaigns}')

    
    
    def parse_ts(field):
        iso = (r'^[0-9]{4}-[0-9]{2}-[0-9]{2}T(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]'
               r'(Z|[+-](0[0-9]|1[0-9]|2[0-3]):[0-5][0-9])$')
        return (f'CASE WHEN regexp_full_match({field}, \'[0-9]+\') '
                f"THEN TRY_CAST(CASE WHEN ltrim({field}, '0') = '' THEN '0' ELSE ltrim({field}, '0') END AS BIGINT) "
                f'WHEN regexp_full_match({field}, {sql_string(iso)}) '
                f'THEN epoch_ms(TRY_CAST({field} AS TIMESTAMPTZ)) '
                f'ELSE NULL END')

    log('Parsing and deduplicating events')
    run(f"""
        CREATE TEMP TABLE event_valid AS
        WITH dedup AS (
            SELECT * EXCLUDE (rn) FROM (
                SELECT *, row_number() OVER (
                    PARTITION BY event_id ORDER BY ingest_ts
                ) rn
                FROM {events} WHERE event_id IS NOT NULL AND event_id != ''
            ) WHERE rn=1
        ), parsed AS (
            SELECT *, {parse_ts('ts')} AS tms,
                   epoch_ms(TRY_CAST(ingest_ts AS TIMESTAMPTZ)) AS ims
            FROM dedup
        )
        SELECT COALESCE(i.person_id, e.user_id) AS person,
               e.tms, e.event_type, e.product_id,
               CASE WHEN e.event_type='purchase' THEN TRY_CAST(e.price_cents AS BIGINT)
                    ELSE 0::BIGINT END AS revenue
        FROM parsed e
        LEFT JOIN identities i ON e.user_id=i.raw_id
        WHERE e.user_id IS NOT NULL AND e.user_id != ''
          AND e.product_id IS NOT NULL AND e.product_id != ''
          AND e.event_type IN ('view', 'cart', 'purchase')
          AND e.tms >= {window_start} AND e.tms < {window_end}
          AND e.ims - e.tms <= 86400000
          AND (e.event_type != 'purchase' OR (
               regexp_full_match(e.price_cents, '[0-9]+')
               AND TRY_CAST(e.price_cents AS BIGINT) BETWEEN 1 AND 10000000
          ))
    """)

    log('Building per-person sessions')
    run("""
        CREATE TEMP TABLE numbered AS
        WITH lagged AS (
            SELECT *, lag(tms) OVER (PARTITION BY person ORDER BY tms) AS prior
            FROM event_valid
        ), marked AS (
            SELECT *, CASE WHEN prior IS NULL OR tms - prior > 1800000 THEN 1 ELSE 0 END AS new_session
            FROM lagged
        )
        SELECT person, tms, event_type, product_id, revenue,
               -- Keep equal timestamps in one session.
               sum(new_session) OVER (PARTITION BY person ORDER BY tms
                  RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS session_number
        FROM marked
    """)
    
    
    run("""
        CREATE TEMP TABLE sessions AS
        SELECT person, session_number, min(tms) AS session_start,
               CAST(epoch_ms(min(tms)) AS DATE) AS day,
               count(*) AS events, sum(revenue) AS revenue_cents,
               CAST(max(CASE WHEN event_type='purchase' THEN 1 ELSE 0 END) AS BIGINT) AS purchasing
        FROM numbered
        GROUP BY person, session_number
    """)
    run("""
        CREATE TEMP TABLE category_revenues AS
        SELECT n.person, s.day, COALESCE(p.category, 'unknown') AS category,
               sum(n.revenue) AS revenue
        FROM numbered n
        JOIN sessions s ON s.person=n.person AND s.session_number=n.session_number
        LEFT JOIN products p ON p.product_id=n.product_id
        WHERE n.event_type='purchase'
        GROUP BY n.person, s.day, COALESCE(p.category, 'unknown')
    """)
    run(f"""
        COPY (
          WITH user_agg AS (
            SELECT person AS user_id, day, count(*)::BIGINT AS sessions,
                   sum(events)::BIGINT AS events, sum(revenue_cents)::BIGINT AS revenue_cents
            FROM sessions GROUP BY person, day
          ), best_category AS (
            SELECT person AS user_id, day, category,
                   row_number() OVER (PARTITION BY person, day
                         ORDER BY revenue DESC, category ASC) rn
            FROM category_revenues
          )
          SELECT replace(u.user_id, '\ue000', chr(13)) AS user_id, u.day, u.sessions, u.events, u.revenue_cents,
                 c.category AS top_category
          FROM user_agg u LEFT JOIN best_category c
            ON u.user_id=c.user_id AND u.day=c.day AND c.rn=1
        ) TO {sql_string(os.path.join(OUT,'user_daily','part-0.parquet'))} (FORMAT PARQUET)
    """)

    log('Deduplicating clicks and assigning campaign credit')
    run(f"""
        CREATE TEMP TABLE click_valid AS
        WITH dedup AS (
            SELECT * EXCLUDE(rn) FROM (
                SELECT *, row_number() OVER (PARTITION BY click_id) AS rn
                FROM {clicks} WHERE click_id IS NOT NULL AND click_id != ''
            ) WHERE rn=1
        )
        SELECT COALESCE(i.person_id, c.user_id) AS person,
               TRY_CAST(c.tms AS BIGINT) AS tms,
               c.click_id, c.campaign_id
        FROM dedup c
        LEFT JOIN identities i ON c.user_id=i.raw_id
        INNER JOIN campaigns a ON a.campaign_id=c.campaign_id
        WHERE c.user_id IS NOT NULL AND c.user_id != ''
          AND TRY_CAST(c.tms AS BIGINT) >= {window_start - WEEK}
          AND TRY_CAST(c.tms AS BIGINT) < {window_end}
    """)
    
    
    # Attribute clicks from the preceding seven days.
    run("""
        CREATE TEMP TABLE qualified AS
        SELECT s.person, s.session_number, c.campaign_id, c.click_id, c.tms,
               row_number() OVER (PARTITION BY s.person, s.session_number
                 ORDER BY c.tms DESC, c.click_id ASC) AS attribution_rank
        FROM sessions s
        JOIN click_valid c ON c.person=s.person
            AND c.tms BETWEEN s.session_start - 604800000 AND s.session_start
    """)
    run("""
        CREATE TEMP TABLE attributed AS
        SELECT s.person, s.session_number, s.day, s.purchasing, s.revenue_cents,
               COALESCE(q.campaign_id, 'organic') AS campaign_id
        FROM sessions s
        LEFT JOIN qualified q ON q.person=s.person
          AND q.session_number=s.session_number AND q.attribution_rank=1
    """)
    run("""
        CREATE TEMP TABLE assisted AS
        SELECT DISTINCT q.campaign_id, a.day, q.person, q.session_number
        FROM qualified q
        JOIN attributed a ON a.person=q.person AND a.session_number=q.session_number
    """)
    run(f"""
        COPY (
          WITH credited AS (
            SELECT campaign_id, day, count(*)::BIGINT AS sessions,
                   sum(purchasing)::BIGINT AS purchasing_sessions,
                   sum(revenue_cents)::BIGINT AS revenue_cents
            FROM attributed GROUP BY campaign_id, day
          ), assistance AS (
            SELECT campaign_id, day, count(*)::BIGINT AS assisted_sessions
            FROM assisted GROUP BY campaign_id, day
          ), keys AS (
            SELECT campaign_id, day FROM credited
            UNION SELECT campaign_id, day FROM assistance
          )
          SELECT k.campaign_id, CASE WHEN k.campaign_id='organic' THEN 'organic'
               ELSE c.channel END AS channel, k.day,
               COALESCE(t.sessions,0)::BIGINT AS sessions,
               COALESCE(t.purchasing_sessions,0)::BIGINT AS purchasing_sessions,
               COALESCE(t.revenue_cents,0)::BIGINT AS revenue_cents,
               COALESCE(a.assisted_sessions,0)::BIGINT AS assisted_sessions
          FROM keys k
          LEFT JOIN credited t ON t.campaign_id=k.campaign_id AND t.day=k.day
          LEFT JOIN assistance a ON a.campaign_id=k.campaign_id AND a.day=k.day
          LEFT JOIN campaigns c ON k.campaign_id=c.campaign_id
        ) TO {sql_string(os.path.join(OUT,'campaign_daily','part-0.parquet'))} (FORMAT PARQUET)
    """)
    log(f'Finished in {time.monotonic()-start:.2f}s')


if __name__ == '__main__':
    main()
