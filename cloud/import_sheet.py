"""One-time move of a Google Sheet weight history into a Smart Mirror account.

Reads the sheet through its Apps Script (CLOUD_URL / CLOUD_TOKEN in
esp32-mirror/src/secrets.h), signs in to the account (you type the email and
password; nothing is stored), and writes every reading to
users/<uid>/readings/<epoch> in Firestore — the same place the devices write.
Re-running is safe: a reading's id is its timestamp, so it is overwritten,
never duplicated.

  py -3 cloud/import_sheet.py            show what would be imported
  py -3 cloud/import_sheet.py --write    sign in and import
  py -3 cloud/import_sheet.py --remove-under 30
                                         sign in, list the account's readings under
                                         30 kg (e.g. test weights), delete on "yes"
"""
import getpass
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "esp32-mirror", "src")


def define(path, name):
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, open(path, encoding="utf-8").read())
    return m.group(1) if m else ""


def utc(ts):
    """Epoch seconds -> Firestore timestampValue ("at", readable in the console)."""
    import datetime as dt
    return dt.datetime.fromtimestamp(ts, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def http_json(url, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method="POST" if data else "GET")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        msg = json.loads(e.read() or b"{}").get("error", {}).get("message", e.reason)
        sys.exit(f"HTTP {e.code}: {msg}")


def sheet_rows():
    secrets = os.path.join(SRC, "secrets.h")
    url, tok = define(secrets, "CLOUD_URL"), define(secrets, "CLOUD_TOKEN")
    if not url:
        sys.exit("CLOUD_URL not found in secrets.h")
    with urllib.request.urlopen(url + "?token=" + urllib.parse.quote(tok), timeout=60) as r:
        text = r.read().decode()
    rows = {}
    for line in text.splitlines():
        parts = line.strip().split(",")
        if len(parts) != 2:
            continue
        try:
            ts, kg = int(parts[0]), float(parts[1])
        except ValueError:
            continue
        if ts > 1_000_000_000 and 1 < kg < 400:
            rows[ts] = kg                      # same timestamp twice: last one wins
    return sorted(rows.items())


def sign_in(key):
    email = input("account email: ").strip().lower()
    password = getpass.getpass("password (not shown): ")
    auth = http_json("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key="
                     + key, {"email": email, "password": password, "returnSecureToken": True})
    return email, auth["localId"], auth["idToken"]


def remove_under(key, project, limit):
    """List the account's readings below `limit` kg; delete them after a "yes"."""
    import datetime as dt
    email, uid, token = sign_in(key)
    base = f"projects/{project}/databases/(default)/documents"
    found, page = [], ""
    while True:
        url = (f"https://firestore.googleapis.com/v1/{base}/users/{uid}/readings"
               f"?pageSize=300&mask.fieldPaths=kg" + (f"&pageToken={page}" if page else ""))
        r = http_json(url, token=token)
        for doc in r.get("documents", []):
            f = doc.get("fields", {}).get("kg", {})
            kg = float(f.get("doubleValue", f.get("integerValue", 0)))
            if kg < limit:
                found.append((doc["name"], int(doc["name"].rsplit("/", 1)[1]), kg))
        page = r.get("nextPageToken", "")
        if not page:
            break
    if not found:
        print(f"no readings under {limit} kg in {email}'s account")
        return
    for _, ts, kg in found:
        print(f"  {dt.datetime.fromtimestamp(ts):%Y-%m-%d %H:%M}  {kg:6.2f} kg")
    if input(f"permanently delete these {len(found)} readings? type yes: ").strip() != "yes":
        print("nothing deleted")
        return
    http_json(f"https://firestore.googleapis.com/v1/{base}:commit",
              {"writes": [{"delete": name} for name, _, _ in found]}, token)
    print(f"deleted {len(found)} readings")


def main():
    key = define(os.path.join(SRC, "cloud_config.h"), "FIREBASE_API_KEY")
    project = define(os.path.join(SRC, "cloud_config.h"), "FIREBASE_PROJECT_ID")
    if "--remove-under" in sys.argv:
        remove_under(key, project, float(sys.argv[sys.argv.index("--remove-under") + 1]))
        return
    rows = sheet_rows()
    print(f"sheet: {len(rows)} readings", end="")
    if rows:
        import datetime as dt
        d = lambda t: dt.datetime.fromtimestamp(t).strftime("%Y-%m-%d")
        print(f", {d(rows[0][0])} .. {d(rows[-1][0])}")
    else:
        print()
    if "--write" not in sys.argv:
        print("dry run - add --write to sign in and import")
        return
    if not key or not project:
        sys.exit("FIREBASE_API_KEY / FIREBASE_PROJECT_ID are empty in cloud_config.h")

    email, uid, token = sign_in(key)
    base = f"projects/{project}/databases/(default)/documents"
    url = f"https://firestore.googleapis.com/v1/{base}:commit"
    done = 0
    for i in range(0, len(rows), 400):          # a commit takes up to 500 writes
        writes = [{"update": {"name": f"{base}/users/{uid}/readings/{ts}", "fields": {
            "ts": {"integerValue": str(ts)}, "at": {"timestampValue": utc(ts)},
            "kg": {"doubleValue": round(kg, 2)}, "src": {"stringValue": "sheet"}}}}
            for ts, kg in rows[i:i + 400]]
        http_json(url, {"writes": writes}, token)
        done += len(writes)
        print(f"imported {done}/{len(rows)}")
    print(f"done: {done} readings are in {email}'s account")


if __name__ == "__main__":
    main()
