"""Check src/timezones.h against the IANA database.

For every zone and alias, evaluates its POSIX rule hour by hour over the next
three years and compares the UTC offset with Python's zoneinfo. Any mismatch
(a wrong offset, or a daylight-saving switch on the wrong day/hour) is listed.

  py -3 ota/check_timezones.py
"""
import datetime as dt
import os
import re
import sys
from zoneinfo import ZoneInfo

HDR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src",
                   "timezones.h")


def zones():
    text = open(HDR, encoding="utf-8").read()
    body = text[text.index("TZ_ZONES[] = {"):]
    body = re.sub(r"//[^\n]*", "", body)              # comments
    body = re.sub(r'"\s*\n\s*"', "", body)            # adjacent literals = one string
    entry = r'\{\s*' + r'\s*,\s*'.join([r'"([^"]*)"'] * 5) + r'\s*\}'
    for m in re.finditer(entry, body):
        yield list(m.groups())


def parse_offset(s):
    """POSIX offset '[+-]h[:mm]' (west positive) -> seconds EAST of UTC."""
    sign = -1 if s.startswith("-") else 1
    s = s.lstrip("+-")
    h, _, m = s.partition(":")
    return -sign * (int(h) * 3600 + int(m or 0) * 60)


def parse_rule(r):
    """'Mm.w.d[/time]' -> (month, week, weekday, seconds after local midnight)."""
    date, _, time = r.partition("/")
    mth, wk, wd = (int(x) for x in date[1:].split("."))
    secs = 7200
    if time:
        sign = -1 if time.startswith("-") else 1
        h, _, m = time.lstrip("+-").partition(":")
        secs = sign * (int(h) * 3600 + int(m or 0) * 60)
    return mth, wk, wd, secs


def rule_utc(year, rule, local_offset):
    mth, wk, wd, secs = rule
    first = dt.date(year, mth, 1)
    # POSIX weekday 0 = Sunday
    d = first + dt.timedelta(days=(wd - (first.isoweekday() % 7)) % 7)
    d += dt.timedelta(weeks=wk - 1)
    if wk == 5:
        while (d + dt.timedelta(weeks=1)).month == mth:
            d += dt.timedelta(weeks=1)
        while d.month != mth:
            d -= dt.timedelta(weeks=1)
    local = dt.datetime(d.year, d.month, d.day) + dt.timedelta(seconds=secs)
    return local - dt.timedelta(seconds=local_offset)       # naive UTC


def posix_offset(posix, utc):
    m = re.match(r"^(<[^>]+>|[A-Za-z]+)([+-]?\d+(?::\d+)?)(?:(<[^>]+>|[A-Za-z]+)"
                 r"([+-]?\d+(?::\d+)?)?(?:,(M[\d.]+(?:/[+-]?\d+(?::\d+)?)?),"
                 r"(M[\d.]+(?:/[+-]?\d+(?::\d+)?)?))?)?$", posix)
    if not m:
        raise ValueError(f"can't parse {posix}")
    std = parse_offset(m.group(2))
    if not m.group(3):
        return std
    dst = parse_offset(m.group(4)) if m.group(4) else std + 3600
    start, end = parse_rule(m.group(5)), parse_rule(m.group(6))
    s = rule_utc(utc.year, start, std)       # switch to DST happens in standard time
    e = rule_utc(utc.year, end, dst)         # switch back happens in DST
    in_dst = (s <= utc < e) if s < e else not (e <= utc < s)
    return dst if in_dst else std


def main():
    start = dt.datetime(2026, 10, 1)
    bad = 0
    n = 0
    for label, city, iana, posix, aliases in zones():
        for name in [iana] + aliases.split():
            n += 1
            zi = ZoneInfo(name)
            errs = []
            t = start
            while t < start + dt.timedelta(days=3 * 365):
                want = int(t.replace(tzinfo=dt.timezone.utc).astimezone(zi).utcoffset()
                           .total_seconds())
                got = posix_offset(posix, t)
                if want != got:
                    errs.append(f"{t:%Y-%m-%d %H:%M}Z want {want / 3600:+g} got {got / 3600:+g}")
                t += dt.timedelta(hours=1)
            if errs:
                bad += 1
                print(f"MISMATCH {name} ({label}): {len(errs)} h, e.g. {errs[0]}")
    print(f"{n} zone names checked, {bad} mismatching")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
