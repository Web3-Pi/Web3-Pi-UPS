#!/usr/bin/env python3
"""
Byte-agnostic model of the HTTP backend's command bookkeeping — mirrors the
"Receiver limits" block of firmware-ESP32-LTE-M/main/http_backend.c
(esp32:0.8.10): ack list cap 8 (oldest evicted), exec dedup ring 16, <= 8
commands applied per response, id = string of 1..47 chars (else bad_id),
rejected list cap 4 with reasons, resp_dropped for a 2xx body > 2047 B,
carried acks/rejects drained BEFORE the response is handled, overflow →
nothing applied. Commands are dicts, a response is (commands, bytes);
HMAC/JSON/HTTP are out of scope. Device(new=False) models the pre-0.8.10
firmware where it matters: every command applied, ids STORED cut to 31 chars
but COMPARED uncut, no rejected list, handle-then-drain ack order, oversize
body → parse fails.

Scenarios (each asserts): 1) 20 queued → applied 8/8/4 over three cycles, no
duplicate; 2) 17 in ONE response → OLD re-executes the oldest (the bug), NEW
applies 8, acks 8, the other 9 arrive next cycle, nothing re-executed;
3) 48-char id → bad_id, never executed, server drops it by prefix (OLD
re-executed it on every poll: the cut ring entry never matched the full id);
4) unsupported cmd → rejected, never acked, server drops it; 5) 2200 B
response → nothing applied, next body carries resp_dropped {2200, 2047},
cleared after the following 2xx, uplink stays up; 6) executed id re-sent →
re-acked, never re-executed, even with a malformed cmd; 7) 8 carried acks +
8 new in one 2xx → NEW acks all 16 over two POSTs, OLD lost the new acks;
8) non-object / id-less entries → one bad_id "" reject, the rest applied.

Mirror any bookkeeping change in http_backend.c here. Run: python3 tools/test_http_cmd_loop.py
"""
import json

ACK_PENDING_MAX, EXEC_RING_MAX, REJ_MAX = 8, 16, 4
CMDS_PER_RESP_MAX = ACK_PENDING_MAX
HTTP_RESP_MAX = 2048            # capture buffer; usable = 2047 B
ACK_ID_MAX_NEW, ACK_ID_MAX_OLD = 48, 32     # incl. NUL: ids of 1..47 chars; pre-0.8.10 cut to 31
KNOWN = {"host.shutdown", "host.reset", "power.cycle", "ui.beep", "ui.display_msg"}


class Device:
    def __init__(self, new=True):
        self.new, self.id_max = new, (ACK_ID_MAX_NEW if new else ACK_ID_MAX_OLD)
        self.pending, self.rejected = [], []    # s_pending acks / s_rejected [(id, reason)]
        self.executed, self.exec_idx = [""] * EXEC_RING_MAX, 0
        self.resp_dropped = None            # {"bytes", "max"} while pending
        self.executions, self.warnings, self.successes = [], [], 0  # probes: dispatches / logs / 2xx count

    def _cut(self, s):                      # snprintf(dst, ACK_ID_MAX, "%s", id)
        return s[:self.id_max - 1]

    # Lookups compare the id AS RECEIVED with STORED (cut) entries, like the C's strcmp.
    def exec_seen(self, cid):
        return any(e and e == cid for e in self.executed)

    def ack_add(self, cid):
        if cid in self.pending:
            return
        if len(self.pending) >= ACK_PENDING_MAX:
            self.pending.pop(0)             # "ack queue full — dropping oldest"
        self.pending.append(self._cut(cid))

    def _apply(self, cid):                  # dispatch ok → exec_add + ack_add
        self.executions.append(cid)
        self.executed[self.exec_idx] = self._cut(cid)
        self.exec_idx = (self.exec_idx + 1) % EXEC_RING_MAX
        self.ack_add(cid)

    def rej_add(self, cid, reason):
        cid = cid[:ACK_ID_MAX_NEW - 1]      # bad_id keeps the first 47 chars
        for i, (rid, _) in enumerate(self.rejected):
            if rid == cid:
                self.rejected[i] = (cid, reason)    # dedupe: refresh the reason
                return
        if len(self.rejected) >= REJ_MAX:
            self.rejected.pop(0)
        self.rejected.append((cid, reason))

    def build_body(self):
        body = {"acks": list(self.pending)}
        if self.new and self.rejected:
            body["rejected"] = [{"id": i, "reason": r} for i, r in self.rejected]
        if self.new and self.resp_dropped:
            body["resp_dropped"] = dict(self.resp_dropped)
        return body

    def handle_response(self, cmds):
        """`cmds` = the parsed, signature-verified "commands" array."""
        if not self.new:                    # OLD: every entry, no limits
            for c in cmds:
                c = c if isinstance(c, dict) else {}   # non-object → items NULL
                cid, cmd = c.get("id"), c.get("cmd")
                if not isinstance(cid, str) or not isinstance(cmd, str):
                    continue
                if self.exec_seen(cid):     # full id vs the cut ring entry
                    self.ack_add(cid)
                elif cmd in KNOWN:          # unknown cmd: silently ignored
                    self._apply(cid)
            return
        if len(cmds) > CMDS_PER_RESP_MAX:
            self.warnings.append(f"carried {len(cmds)}, processing {CMDS_PER_RESP_MAX}")
        for c in cmds[:CMDS_PER_RESP_MAX]:
            c = c if isinstance(c, dict) else {}       # non-object → items NULL
            cid = c.get("id") if isinstance(c.get("id"), str) else ""
            cmd = c.get("cmd")
            if len(cid) == 0 or len(cid) >= ACK_ID_MAX_NEW:
                self.rej_add(cid, "bad_id")
            elif self.exec_seen(cid):       # re-ack, never re-execute — checked
                self.ack_add(cid)           # before the payload shape
            elif not isinstance(cmd, str):
                self.rej_add(cid, "bad_args")
            elif cmd not in KNOWN:
                self.rej_add(cid, "unsupported_cmd")
            else:                           # dispatch_command() cannot fail for a known name
                self._apply(cid)

    def post(self, server):
        """One POST cycle (post_once + http_task loop). Returns the body sent."""
        body = self.build_body()
        acks_in, rej_in, dropped_in = (len(self.pending), len(self.rejected),
                                       self.resp_dropped is not None)
        status, cmds, nbytes = server(body)
        if not 200 <= status < 300:
            return body
        self.successes += 1                 # s_last_success_s = now
        if self.new:
            del self.pending[:acks_in]      # drain BEFORE handling
            del self.rejected[:rej_in]
            if dropped_in:
                self.resp_dropped = None
            if nbytes > HTTP_RESP_MAX - 1:
                self.resp_dropped = {"bytes": nbytes, "max": HTTP_RESP_MAX - 1}
            else:
                self.handle_response(cmds)
        else:
            if nbytes <= HTTP_RESP_MAX - 1: # truncated JSON → cJSON fails → nothing
                self.handle_response(cmds)
            del self.pending[:acks_in]      # handle THEN drain (old http_task)
        return body


class Server:
    """Queue like examples/http-control-server/server.py. limit 0 = send all."""

    def __init__(self, limit=CMDS_PER_RESP_MAX):
        self.q, self.limit, self.n, self.last_sent = [], limit, 0, []
        self.force_bytes, self.ignore_acks_once = None, False    # pretend size / lose acks

    def enqueue(self, cmd="ui.beep", cid=None, n=1):
        for _ in range(n):
            self.n += 1
            self.q.append({"id": cid or f"c-{self.n:04d}", "cmd": cmd, "args": {}})

    def __call__(self, body):
        acked = set() if self.ignore_acks_once else set(body.get("acks", []))
        self.ignore_acks_once = False
        self.q = [c for c in self.q if c["id"] not in acked]
        for r in body.get("rejected", []):
            rid, reason = r["id"], r["reason"]   # prefix match only for a cut bad_id
            self.q = [c for c in self.q if not (c["id"] == rid or
                      (reason == "bad_id" and len(rid) == ACK_ID_MAX_NEW - 1 and
                       c["id"].startswith(rid)))]
        self.last_sent = list(self.q) if self.limit == 0 else self.q[:self.limit]
        nbytes = self.force_bytes or len(json.dumps({"commands": self.last_sent}))
        return 200, self.last_sent, nbytes


def ids(a, b):
    return [f"c-{i:04d}" for i in range(a, b + 1)]


def s1_twenty_commands():
    for limit in (0, CMDS_PER_RESP_MAX):    # unlimited server and a capped one
        dev, srv = Device(), Server(limit)
        srv.enqueue(n=20)
        carried = [len(dev.post(srv)["acks"]) for _ in range(5)]
        assert carried == [0, 8, 8, 4, 0], carried
        assert dev.executions == ids(1, 20) and not srv.q and not dev.pending


def s2_seventeen_in_one_response():
    old, srv = Device(new=False), Server(limit=0)
    srv.enqueue(n=17)
    old.post(srv)                           # 17 applied; ring wrapped past c-0001
    assert len(old.executions) == 17 and old.pending == ids(10, 17)
    old.post(srv)                           # acks 10..17; 1..9 re-sent
    assert old.executions.count("c-0001") == 2, "OLD re-executes the evicted id"

    dev, srv = Device(), Server(limit=0)
    srv.enqueue(n=17)
    dev.post(srv)
    assert dev.executions == ids(1, 8) and dev.warnings and dev.pending == ids(1, 8)
    body = dev.post(srv)                    # acks 1..8, gets the other 9
    assert body["acks"] == ids(1, 8) and [c["id"] for c in srv.last_sent] == ids(9, 17)
    assert dev.executions == ids(1, 16)     # 8 of the 9 applied, none twice
    for _ in range(2):
        dev.post(srv)
    assert dev.executions == ids(1, 17) and not srv.q and not dev.pending


def s3_long_id():
    long_id = "x" * 48
    dev, srv = Device(), Server()
    srv.enqueue(cid=long_id)
    srv.enqueue()                           # c-0002 behind it
    dev.post(srv)
    assert dev.rejected == [(long_id[:47], "bad_id")] and dev.executions == ["c-0002"]
    body = dev.post(srv)
    assert body["rejected"] == [{"id": "x" * 47, "reason": "bad_id"}]
    assert body["acks"] == ["c-0002"] and not srv.q, "server matched the 47-char prefix"
    dev.post(srv)
    assert not dev.rejected and dev.executions == ["c-0002"]

    old, srv = Device(new=False), Server()  # OLD: exec_seen(full id) never matched
    srv.enqueue(cid=long_id)                # the 31-char ring entry, and the cut
    for _ in range(3):                      # ack never matched the server's id
        old.post(srv)
    assert old.executions == [long_id] * 3, "OLD: re-executed on every poll"
    assert old.pending == ["x" * 31] and srv.q, "OLD: re-sent forever"


def s4_unsupported_cmd():
    dev, srv = Device(), Server()
    srv.enqueue(cmd="ui.dance")
    srv.enqueue(cmd="ui.beep")
    dev.post(srv)
    assert dev.rejected == [("c-0001", "unsupported_cmd")]
    assert dev.pending == ["c-0002"] and dev.executions == ["c-0002"]
    body = dev.post(srv)
    assert body["acks"] == ["c-0002"] and body["rejected"][0]["reason"] == "unsupported_cmd"
    assert not srv.q and not dev.rejected and "c-0001" not in dev.pending


def s5_oversize_response():
    dev, srv = Device(), Server()
    srv.enqueue()
    srv.force_bytes = 2200
    dev.post(srv)
    assert dev.executions == [] and dev.successes == 1, "2xx still refreshes uplink"
    assert dev.resp_dropped == {"bytes": 2200, "max": 2047}
    srv.force_bytes = None
    body = dev.post(srv)
    assert body["resp_dropped"] == {"bytes": 2200, "max": 2047} and body["acks"] == []
    assert dev.resp_dropped is None and dev.executions == ["c-0001"]
    body = dev.post(srv)
    assert "resp_dropped" not in body and body["acks"] == ["c-0001"] and not srv.q

    old, srv = Device(new=False), Server()  # OLD: parse fails, nothing reported
    srv.enqueue()
    srv.force_bytes = 2200
    for _ in range(3):
        old.post(srv)
    assert old.executions == [] and srv.q, "OLD: re-sent forever, no signal"


def s6_reexecuted_id_reacked():
    dev, srv = Device(), Server()
    srv.enqueue()
    dev.post(srv)
    srv.ignore_acks_once = True             # server "loses" our ack once
    body = dev.post(srv)
    assert body["acks"] == ["c-0001"] and srv.last_sent[0]["id"] == "c-0001"
    assert dev.executions == ["c-0001"] and dev.pending == ["c-0001"], "re-acked only"
    dev.post(srv)
    assert not srv.q and not dev.pending
    # Re-sent with a malformed cmd: still re-acked (exec_seen precedes the shape checks).
    dev.handle_response([{"id": "c-0001", "cmd": 7}])
    assert dev.pending == ["c-0001"] and not dev.rejected and dev.executions == ["c-0001"]


def s7_carried_plus_new():
    def run(new):
        dev, srv = Device(new=new), Server()
        srv.enqueue(n=8)
        dev.post(srv)                       # applies 1..8, pending = 8
        srv.enqueue(n=8)                    # 9..16 wait on the server
        second = dev.post(srv)              # carries 1..8, receives 9..16
        return dev, srv, second, dev.post(srv)
    dev, srv, second, third = run(True)
    assert second["acks"] == ids(1, 8) and third["acks"] == ids(9, 16)
    assert dev.executions == ids(1, 16) and not srv.q, "all 16 acked over two POSTs"
    dev, srv, second, third = run(False)
    assert second["acks"] == ids(1, 8) and third["acks"] == [], "OLD lost the new acks"
    assert [c["id"] for c in srv.q] == ids(9, 16) and dev.executions == ids(1, 16)


def s8_non_object_entries():
    dev = Device()
    dev.handle_response([42, {"id": "c-0001", "cmd": "ui.beep"}, {"cmd": "ui.beep"}, "x"])
    assert dev.rejected == [("", "bad_id")], "id-less entries share one bad_id reject"
    assert dev.executions == ["c-0001"] and dev.pending == ["c-0001"] and \
        dev.build_body()["rejected"] == [{"id": "", "reason": "bad_id"}]


if __name__ == "__main__":
    for fn in (s1_twenty_commands, s2_seventeen_in_one_response, s3_long_id,
               s4_unsupported_cmd, s5_oversize_response, s6_reexecuted_id_reacked,
               s7_carried_plus_new, s8_non_object_entries):
        fn()
        print(f"ok  {fn.__name__}")
    print("ALL PASSED")
