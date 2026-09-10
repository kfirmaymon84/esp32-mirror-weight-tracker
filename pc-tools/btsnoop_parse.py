"""
Parse an Android btsnoop_hci.log and dump the ATT (GATT) layer.

Goal: see exactly how the Mi Home app talks to the S200 -- the auth handshake
and the notification/read that carries weight. We reassemble L2CAP over ACL and
decode ATT PDUs (writes, notifications, indications, read responses), printing
timestamp, direction, opcode, handle and value bytes.

It also HIGHLIGHTS any value whose bytes contain the expected weight encoding so
the weight packet is easy to find.

Usage:
    python btsnoop_parse.py PATH\\to\\btsnoop_hci.log
    python btsnoop_parse.py file.log --find b4 28 68 51 12 04
        (--find takes hex byte patterns to flag; defaults cover 104.20 kg)
"""

import argparse
import struct
import sys

ATT_CID = 0x0004

ATT_OPCODES = {
    0x01: "ERROR_RSP", 0x02: "MTU_REQ", 0x03: "MTU_RSP",
    0x04: "FIND_INFO_REQ", 0x05: "FIND_INFO_RSP",
    0x06: "FIND_BY_TYPE_REQ", 0x07: "FIND_BY_TYPE_RSP",
    0x08: "READ_BY_TYPE_REQ", 0x09: "READ_BY_TYPE_RSP",
    0x0A: "READ_REQ", 0x0B: "READ_RSP",
    0x0C: "READ_BLOB_REQ", 0x0D: "READ_BLOB_RSP",
    0x10: "READ_BY_GROUP_REQ", 0x11: "READ_BY_GROUP_RSP",
    0x12: "WRITE_REQ", 0x13: "WRITE_RSP",
    0x16: "PREPARE_WRITE_REQ", 0x17: "PREPARE_WRITE_RSP",
    0x18: "EXEC_WRITE_REQ", 0x19: "EXEC_WRITE_RSP",
    0x1B: "NOTIFY", 0x1D: "INDICATE", 0x1E: "CONFIRM",
    0x52: "WRITE_CMD", 0xD2: "SIGNED_WRITE_CMD",
}

# ATT PDUs that carry a (handle, value) we care about.
HANDLE_VALUE_OPS = {0x0B, 0x0D, 0x12, 0x13, 0x1B, 0x1D, 0x52, 0xD2, 0x0A, 0x0C}


def read_btsnoop(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"btsnoop\x00":
        raise ValueError("not a btsnoop file (bad magic)")
    version, datalink = struct.unpack(">II", data[8:16])
    off = 16
    records = []
    while off + 24 <= len(data):
        orig_len, incl_len, flags, drops, ts = struct.unpack(">IIIIq", data[off:off + 24])
        off += 24
        pkt = data[off:off + incl_len]
        off += incl_len
        records.append((flags, ts, pkt))
    return datalink, records


def parse(path, find_patterns, only_conn=None):
    datalink, records = read_btsnoop(path)
    print(f"btsnoop datalink={datalink}, records={len(records)}\n")

    # Per ACL connection-handle L2CAP reassembly buffers.
    reassembly = {}  # conn_handle -> {"buf": bytes, "l2len": int, "cid": int, "dir": int, "ts": int}

    def flush_att(conn_handle, buf, direction, ts):
        # buf is a complete L2CAP payload for CID 0x0004 (ATT)
        if len(buf) < 1:
            return
        if only_conn is not None and conn_handle != only_conn:
            return
        opcode = buf[0]
        name = ATT_OPCODES.get(opcode, f"0x{opcode:02X}")
        arrow = "RX(dev->phone)" if direction == 1 else "TX(phone->dev)"
        line = f"[c=0x{conn_handle:03x}] {arrow} {name}"
        detail = ""
        if opcode in (0x1B, 0x1D, 0x12, 0x52, 0xD2):  # handle + value
            if len(buf) >= 3:
                handle = struct.unpack("<H", buf[1:3])[0]
                value = buf[3:]
                detail = f" handle=0x{handle:04x} value={value.hex(' ')}"
        elif opcode in (0x0B, 0x0D):  # read rsp: value only
            value = buf[1:]
            detail = f" value={value.hex(' ')}"
        elif opcode in (0x0A, 0x0C):  # read req: handle
            if len(buf) >= 3:
                handle = struct.unpack("<H", buf[1:3])[0]
                detail = f" handle=0x{handle:04x}"
        else:
            detail = f" raw={buf.hex(' ')}"

        flag = ""
        hexstr = buf.hex()
        for pat in find_patterns:
            if pat and pat in hexstr:
                flag = "   <<<<< MATCH weight-pattern " + pat
                break
        print(line + detail + flag)

    for flags, ts, pkt in records:
        if not pkt:
            continue
        direction = flags & 0x01  # 1 = received (controller->host)
        # H4: first byte is HCI packet type
        if datalink in (1002,):
            h4 = pkt[0]
            payload = pkt[1:]
        else:
            # 1001 = unencapsulated HCI; assume ACL framing differently. Try H4-less:
            h4 = None
            payload = pkt
        # We only care about ACL data (0x02)
        if datalink == 1002 and h4 != 0x02:
            continue
        if len(payload) < 4:
            continue
        handle_flags, acl_len = struct.unpack("<HH", payload[:4])
        conn_handle = handle_flags & 0x0FFF
        pb = (handle_flags >> 12) & 0x3  # 0b10 first, 0b01 continuation
        acl_payload = payload[4:4 + acl_len]

        if pb == 0x1:  # continuation fragment
            st = reassembly.get(conn_handle)
            if st is None:
                continue
            st["buf"] += acl_payload
        else:  # first fragment (0b10 == 2) or complete
            if len(acl_payload) < 4:
                continue
            l2len, cid = struct.unpack("<HH", acl_payload[:4])
            reassembly[conn_handle] = {
                "buf": acl_payload[4:], "l2len": l2len, "cid": cid,
                "dir": direction, "ts": ts,
            }

        st = reassembly.get(conn_handle)
        if st and len(st["buf"]) >= st["l2len"]:
            if st["cid"] == ATT_CID:
                flush_att(conn_handle, st["buf"][:st["l2len"]], st["dir"], st["ts"])
            reassembly.pop(conn_handle, None)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--find", nargs="*", default=["b428", "6851", "1204", "b4 28"])
    ap.add_argument("--conn", default="", help="only show this conn handle, e.g. 0x003")
    args = ap.parse_args()
    pats = [p.replace(" ", "").lower() for p in args.find]
    conn = int(args.conn, 16) if args.conn else None
    try:
        parse(args.path, pats, conn)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)
