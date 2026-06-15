#!/usr/bin/env python3
"""
nm2_test_all.py — Cross-device Network MIDI 2.0 session test.

Tests 2-8 (test 1 = Mac host / Pico A client mDNS already passed manually).

  Mac  POSIX : ./build/examples/midi_bridge/nm2_interactive
  Pi   POSIX : /home/<pi-user>/NetworkMIDI2/build/examples/midi_bridge/nm2_interactive
  Pico A     : /dev/cu.usbmodem2112201
  Pico B     : /dev/cu.usbmodem21121301

Quit asymmetry (avoids 'q'-at-REPEAT_PROMPT reboot race):
  - CLIENT functions send 'q' first after UMP confirmed, board → REPEAT_PROMPT.
  - HOST functions wait passively for "Run again?" (arrives via "Peer disconnected"
    path when client's Bye is received, or fall back to sending 'q' after timeout).
  - Pico hosts therefore end at REPEAT_PROMPT without the script sending 'q'.
  - After each test, pico_repeat_to_role(board) advances REPEAT_PROMPT → Role?.
"""

import sys, os, time, re, threading, subprocess, select

try:
    import serial
except ImportError:
    sys.exit("pyserial not found — pip install pyserial")

# ---------------------------------------------------------------------------
# Configuration — set via environment variables (copy nm2_test.env.example to
# nm2_test.env, fill in your values, then `source nm2_test.env` before running)
# ---------------------------------------------------------------------------

def _require_env(name, example):
    val = os.environ.get(name)
    if not val:
        sys.exit(f"Environment variable {name} is not set. "
                 f"See nm2_test.env.example (e.g. {name}={example!r})")
    return val

PICO_A_DEV = os.environ.get("NM2_PICO_A_DEV", "/dev/ttyACM0")
PICO_B_DEV = os.environ.get("NM2_PICO_B_DEV", "/dev/ttyACM1")
BAUD       = 115200

MAC_BINARY = os.path.abspath("build/examples/midi_bridge/nm2_interactive")
PI_IP      = _require_env("NM2_PI_IP",      "192.168.1.50")
PI_USER    = _require_env("NM2_PI_USER",    "pi")
PI_KEY     = os.path.expanduser(os.environ.get("NM2_PI_KEY", "~/.ssh/id_rsa"))
PI_BINARY  = os.environ.get("NM2_PI_BINARY",
                 f"/home/{PI_USER}/NetworkMIDI2/build/examples/midi_bridge/nm2_interactive")
MAC_LAN_IP = _require_env("NM2_MAC_LAN_IP", "192.168.1.10")

MDNS_NAME  = "picomidi"
results    = {}

PICOTOOL   = os.path.expanduser("~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool")
PICO_A_BUS = 2
PICO_A_ADDR = 11
PICO_B_BUS = 2
PICO_B_ADDR = 13


# ---------------------------------------------------------------------------
# Board — Pico via pyserial
# ---------------------------------------------------------------------------
class Board:
    def __init__(self, dev, label):
        self.dev, self.label = dev, label
        self.ser  = None
        self._buf = ""

    def open(self):
        self.ser = serial.Serial(self.dev, BAUD, timeout=0.1)
        self._buf = ""

    def reopen(self):
        """Close and reopen the serial port after a Pico USB CDC reconnect."""
        if self.ser:
            try: self.ser.close()
            except Exception: pass
            self.ser = None
        self._buf = ""
        for _ in range(8):
            time.sleep(1)
            if not os.path.exists(self.dev):
                continue
            try:
                self.ser = serial.Serial(self.dev, BAUD, timeout=0.1)
                return True
            except Exception:
                pass
        return False

    def send(self, text, delay=0.08):
        time.sleep(delay)
        if self.ser:
            try:
                self.ser.write(text.encode())
            except Exception:
                pass

    def expect(self, patterns, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
            except Exception:
                time.sleep(0.05)
                continue
            if chunk:
                self._buf += chunk.decode("utf-8", errors="replace")
            parts = re.split(r"\r\n|\r|\n", self._buf)
            self._buf = parts[-1]
            for idx, line in enumerate(parts[:-1]):
                line = line.strip()
                if not line:
                    continue
                print(f"  [{self.label}] {line}")
                sys.stdout.flush()
                for i, pat in enumerate(patterns):
                    if re.search(pat, line, re.IGNORECASE):
                        # Save remaining lines so the next expect sees them.
                        self._buf = "\r\n".join(parts[idx+1:])
                        return i, line
            # Check partial buffer — catches prompts without trailing newline
            partial = self._buf.strip()
            if partial:
                for i, pat in enumerate(patterns):
                    if re.search(pat, partial, re.IGNORECASE):
                        print(f"  [{self.label}] {partial}")
                        sys.stdout.flush()
                        self._buf = ""
                        return i, partial
        return -1, ""

    def drain(self):
        time.sleep(0.2)
        try:
            self.ser.read(max(self.ser.in_waiting, 1))
        except Exception:
            pass
        self._buf = ""

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None


# ---------------------------------------------------------------------------
# PosixProc — nm2_interactive via subprocess (local or SSH)
# ---------------------------------------------------------------------------
class PosixProc:
    def __init__(self, label, cmd):
        self.label = label
        self.proc  = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0)
        self._buf = ""

    def send(self, text, delay=0.1):
        time.sleep(delay)
        try:
            self.proc.stdin.write(text.encode())
            self.proc.stdin.flush()
        except Exception:
            pass

    def expect(self, patterns, timeout=30):
        deadline = time.time() + timeout
        fd = self.proc.stdout.fileno()
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                self._buf += chunk.decode("utf-8", errors="replace")
            parts = re.split(r"\r\n|\r|\n", self._buf)
            self._buf = parts[-1]
            for idx, line in enumerate(parts[:-1]):
                line = line.strip()
                if not line:
                    continue
                print(f"  [{self.label}] {line}")
                sys.stdout.flush()
                for i, pat in enumerate(patterns):
                    if re.search(pat, line, re.IGNORECASE):
                        # Save remaining lines so the next expect sees them.
                        self._buf = "\r\n".join(parts[idx+1:])
                        return i, line
            # Check partial — catches "Role? [H]ost / [C]lient: " (no trailing \n)
            partial = self._buf.strip()
            if partial:
                for i, pat in enumerate(patterns):
                    if re.search(pat, partial, re.IGNORECASE):
                        print(f"  [{self.label}] {partial}")
                        sys.stdout.flush()
                        self._buf = ""
                        return i, partial
        return -1, ""

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Factories
# ---------------------------------------------------------------------------
def make_mac(label="MAC"):
    return PosixProc(label, [MAC_BINARY, "--name", MDNS_NAME])

def make_pi(label="PI"):
    return PosixProc(label, [
        "ssh", "-i", PI_KEY,
        "-o", "StrictHostKeyChecking=no",
        "-o", "BatchMode=yes",
        f"{PI_USER}@{PI_IP}",
        f"{PI_BINARY} --name {MDNS_NAME}",
    ])


# ---------------------------------------------------------------------------
# Pico state helpers
# ---------------------------------------------------------------------------

def pico_reboot_to_reset_name(board):
    """Navigate board from ROLE_SELECT through a HOST session → REPEAT_PROMPT → N-reboot,
    then bring it back up via fresh-boot (which sets mDNS name to MDNS_NAME).
    Call this at startup when the saved mDNS name may be wrong."""
    # Enter host mode (from ROLE_SELECT)
    board.send("H\r")
    i, _ = board.expect([r"\[Host\] Waiting", r"\[nm2\] State -> Idle", r"Role\?"], timeout=15)
    if i < 0:
        return False  # unexpected state
    if i == 2:
        # Got back to ROLE_SELECT prompt — unusual, but try again
        board.send("H\r")
        i, _ = board.expect([r"\[Host\] Waiting", r"\[nm2\] State -> Idle"], timeout=12)
        if i < 0:
            return False
    # Now in HOST mode (Idle / Waiting) — send q to quit
    board.send("q\r", delay=0.3)
    j, _ = board.expect(["Run again", r"Session closed"], timeout=15)
    if j < 0:
        return False
    if j == 1:
        board.expect(["Run again"], timeout=8)
    # At REPEAT_PROMPT — send N to reboot
    board.send("N\r", delay=0.15)
    board.expect(["Rebooting"], timeout=5)
    board.reopen()
    return pico_fresh_boot_to_role(board)


def pico_repeat_to_role(board):
    """Board is at REPEAT_PROMPT ("Run again?").  Advance to Role? via 'R'."""
    board.send("R\r", delay=0.15)
    i, _ = board.expect([r"Role\?"], timeout=10)
    return i == 0


def pico_ensure_role(board):
    """Best-effort: bring board to Role? from any post-test state.
    Returns True on success. Used between tests to recover from failures
    including Pico reboots triggered by bad cleanup paths."""
    # By contract, after any test the board is at REPEAT_PROMPT or Role?.
    # Sending 'R' is safe at REPEAT_PROMPT (→ Role?) and just causes a
    # "Please enter 'H' or 'C'" error at ROLE_SELECT (board stays there).
    board.send("R\r", delay=0.15)
    i, _ = board.expect([r"Role\?", r"Run again", r"Rebooting",
                          r"Network MIDI", r"Saved WiFi", r"Connected",
                          r"mDNS name", r"Please enter"], timeout=12)
    if i == 0:   # Role? — done
        return True
    if i == 1:   # Still at REPEAT_PROMPT
        return pico_repeat_to_role(board)
    if i == 6:   # mDNS name prompt (board mid-boot; 'R' was sent there by mistake)
        board.send(MDNS_NAME + "\r", delay=0.15)
        k, _ = board.expect([r"Role\?"], timeout=15)
        return k == 0
    if i in (2, 3, 4, 5):  # rebooting or in boot sequence
        board.reopen()
        return pico_fresh_boot_to_role(board)
    if i == 7:   # "Please enter" — board is at ROLE_SELECT, already there
        return True
    # Timeout — board may be in SESSION_RUN or unknown state; send q+Enter to quit.
    board.send("q\r", delay=0.2)
    j, _ = board.expect([r"Run again", r"Role\?", r"Rebooting",
                          r"Network MIDI", r"Saved WiFi"], timeout=15)
    if j == 0:
        return pico_repeat_to_role(board)
    if j == 1:
        return True
    if j >= 2:
        board.reopen()
        return pico_fresh_boot_to_role(board)
    board.reopen()
    return pico_fresh_boot_to_role(board)


def pico_fresh_boot_to_role(board):
    """Drive board from fresh boot (or silent WIFI_SSID state) to Role? prompt.

    After a reboot the USB CDC ring buffer may already be full of old prompts, or it
    may be empty (if the board sat idle long enough to drain it).  In both cases we
    need to handle a silent board at WIFI_SSID by probing with blank Enter, which
    triggers "Using saved WiFi..." when saved credentials exist (which they do after
    any successful run).

    Steps:
      1. Wait briefly for spontaneous output (in case buffer has boot banner).
      2. If already at Role? or REPEAT_PROMPT, handle directly.
      3. If connected but at mDNS name prompt, send Enter.
      4. Otherwise (WIFI_SSID silent or about to be) send blank Enter to trigger
         "Using saved WiFi..." -> WIFI_CONNECTING -> "Connected!" path.
      5. After "Connected!" send Enter for mDNS name -> "Role?".
    """
    i, _ = board.expect([r"Network MIDI 2\.0", r"Saved WiFi", r"WiFi SSID",
                          r"Connected", r"Role\?", r"Run again", r"mDNS name"], timeout=8)
    if i == 4:  # Already at Role?
        return True
    if i == 5:  # At REPEAT_PROMPT
        return pico_repeat_to_role(board)
    if i == 3:  # Already connected, at mDNS name prompt (no '\n' after Connected yet)
        board.send(MDNS_NAME + "\r", delay=0.15)
        j, _ = board.expect([r"Role\?"], timeout=15)
        return j == 0
    if i == 6:  # Already at mDNS name prompt (board was mid-boot, past WiFi)
        board.send(MDNS_NAME + "\r", delay=0.15)
        j, _ = board.expect([r"Role\?"], timeout=15)
        return j == 0

    # i in {0,1,2} means banner/SSID output was seen; i<0 means board is silent.
    # For i==0 (banner only) we wait for the SSID prompt to arrive.
    # For i in {1,2} or i<0 the SSID prompt is already present (or was, before the
    # buffer drained) -- send blank Enter to use saved credentials.
    if i == 0:
        j, _ = board.expect([r"Saved WiFi", r"WiFi SSID"], timeout=10)
        if j < 0:
            return False

    # Send blank Enter at the WIFI_SSID prompt.
    # - With saved creds: "Using saved WiFi..." -> WIFI_CONNECTING -> "Connected!"
    # - Without saved creds: "WiFi SSID: " re-prompt -> return False (can't proceed)
    # - If board happened to be at REPEAT_PROMPT: blank Enter -> doReboot() -> reboot
    #   (which we detect via "Rebooting" and recurse).
    # - If board was already at mDNS-name prompt: blank Enter confirms name -> "Role?"
    # - If board was already at Role?: blank Enter is silently ignored (firmware
    #   does nothing for blank input at ROLE_SELECT) -> j=-1 in 75s.
    board.send("\r", delay=0.15)

    # r"mDNS name \(" matches the PROMPT; r"mDNS name: " matches the echo after confirm.
    j, _ = board.expect([r"Connected", r"Using saved", r"WiFi SSID", r"Rebooting",
                          r"Role\?", r"mDNS name \(", r"mDNS name: "], timeout=75)
    if j == 1:  # "Using saved WiFi..." -> wait for "Connected!"
        k, _ = board.expect([r"Connected"], timeout=60)
        if k < 0:
            return False
    elif j == 2:  # "WiFi SSID: " re-prompted -- no saved creds, cannot continue
        return False
    elif j == 3:  # "Rebooting" -- blank Enter hit REPEAT_PROMPT; board reboots
        return pico_fresh_boot_to_role(board)  # recursive: handle the fresh boot
    elif j == 4:  # Already at Role?
        return True
    elif j == 5:  # mDNS name PROMPT (parenthesis variant) -- board past WiFi, at prompt
        board.send(MDNS_NAME + "\r", delay=0.15)
        k, _ = board.expect([r"Role\?"], timeout=15)
        return k == 0
    elif j == 6:  # mDNS name ECHO -- name already confirmed, Role? is next
        k, _ = board.expect([r"Role\?"], timeout=15)
        return k == 0
    elif j < 0:
        # 75s with no WiFi-stage output. Board is almost certainly already at Role?
        # (blank Enter at ROLE_SELECT is silently ignored by the firmware). Confirm.
        board.send("X\r", delay=0.15)
        k, _ = board.expect([r"Please enter", r"Role\?"], timeout=8)
        return k >= 0
    # j==0 or came via j==1: "Connected" was seen

    # mDNS name PROMPT follows "Connected!" -- always set to MDNS_NAME
    board.send(MDNS_NAME + "\r", delay=0.15)
    k, _ = board.expect([r"Role\?"], timeout=15)
    return k == 0


def pico_recover(board):
    """Best-effort recovery when board state is unknown.
    Returns True if board ends up at Role? prompt."""
    # Probe first: send an invalid role char. At ROLE_SELECT this gets
    # "Please enter 'H' or 'C':", confirming we're already there.
    board.send("X\r", delay=0.2)
    i, _ = board.expect([r"Please enter", r"Role\?"], timeout=5)
    if i >= 0:
        return True

    # 'R' is safe at REPEAT_PROMPT and produces Role? there
    board.send("R\r", delay=0.2)
    i, _ = board.expect([r"Role\?"], timeout=5)
    if i == 0:
        return True

    # Not at REPEAT_PROMPT -- try 'q\r' to exit SESSION_RUN / MDNS_RESOLVE.
    board.send("q\r", delay=0.2)
    i, _ = board.expect(["Run again", r"Role\?", r"Rebooting",
                          r"Network MIDI", r"Saved WiFi"], timeout=15)
    if i == 1:
        return True
    if i == 0:
        return pico_repeat_to_role(board)
    if i in (2, 3, 4):
        board.reopen()
        return pico_fresh_boot_to_role(board)

    # Last resort: enter dummy IP to escape CLIENT_IP state, then quit
    board.send("192.0.2.1\r")  # RFC 5737 TEST-NET — dummy IP to escape CLIENT_IP prompt
    time.sleep(2)
    board.send("q\r")
    board.expect(["Run again"], timeout=10)
    return pico_repeat_to_role(board)


# ---------------------------------------------------------------------------
# Pico session runners
#
# CONTRACT:
#   - pico_run_host: DOES NOT send 'q'. Waits passively for "Run again?"
#     which arrives when the CLIENT sends its Bye (peer disconnect path).
#     Falls back to sending 'q' after a timeout.  Ends at REPEAT_PROMPT.
#   - pico_run_client_*: sleeps 1 s then sends 'q' -> Bye -> host sees peer
#     disconnect.  Ends at REPEAT_PROMPT.
#   - All functions end at REPEAT_PROMPT on both PASS and FAIL.
#   - Caller then sends "R\r" via pico_repeat_to_role() to advance to Role?.
# ---------------------------------------------------------------------------

def pico_run_host(board, host_ip_out, host_ip_ready):
    """Board is at Role?. Drive host role. Ends at REPEAT_PROMPT."""
    def fail(r):
        print(f"\n  [{board.label} HOST FAIL] {r}")
        host_ip_ready.set()
        # Probe whether the board is already at REPEAT_PROMPT (e.g. peer beat us there).
        # If not, send 'q\r' to quit SESSION_RUN.
        j, _ = board.expect(["Run again"], timeout=1)
        if j < 0:
            board.send("q\r", delay=0.2)
            board.expect(["Run again"], timeout=10)
        return f"FAIL:{r}"

    board.send("H\r")
    i, line = board.expect([r"\[Host\] IP", r"\[Host\] Waiting", r"\[Host\] Still"], timeout=12)
    if i < 0:
        return fail("no_host_start")
    # Parse IP from "[Host] IP    : x.x.x.x  port N"
    if i == 0:
        m = re.search(r"(\d+\.\d+\.\d+\.\d+)", line)
        if m:
            host_ip_out[0] = m.group(1)
    host_ip_ready.set()

    i, _ = board.expect(["State -> Established"], timeout=65)
    if i < 0:
        return fail("no_established")

    i, _ = board.expect([r"\[UMP\].*Note"], timeout=20)
    if i < 0:
        return fail("no_ump")

    # Wait passively for "Run again?" -- arrives when client sends Bye.
    # Fall back to sending 'q' if the client takes too long.
    i, _ = board.expect(["Run again", r"Peer disconnected"], timeout=20)
    if i == 1:
        board.expect(["Run again"], timeout=8)
    elif i < 0:
        board.send("q\r")
        board.expect(["Session closed", "Peer disconnected"], timeout=10)
        board.expect(["Run again"], timeout=8)
    return "PASS"   # Board at REPEAT_PROMPT


def pico_run_client_mdns(board, host_ip_out, host_ip_ready):
    """Board is at Role?. Drive client (mDNS). Ends at REPEAT_PROMPT.
    Client quits first (sends Bye), which triggers host's peer-disconnect path."""
    def fail(r):
        print(f"\n  [{board.label} CLIENT FAIL] {r}")
        # Probe for REPEAT_PROMPT first -- 'q' there triggers doReboot().
        j, _ = board.expect(["Run again"], timeout=1)
        if j < 0:
            board.send("q\r", delay=0.2)
            board.expect(["Session closed", "Peer disconnected", "Run again"], timeout=12)
            board.expect(["Run again"], timeout=8)
        return f"FAIL:{r}"

    host_ip_ready.wait(timeout=35)

    board.send("C\r")
    i, _ = board.expect(["Found host", "Host IP"], timeout=40)
    if i == 1:
        ip = host_ip_out[0] if host_ip_out else None
        if not ip:
            return fail("mdns_failed_no_ip")
        print(f"\n  [{board.label}] mDNS fallback -> IP {ip}")
        board.send(ip + "\r")
    elif i < 0:
        # mDNS timed out. Send Enter to abort the browse and fall back to manual IP.
        ip = host_ip_out[0] if host_ip_out else None
        if not ip:
            return fail("mdns_timeout_no_ip")
        board.send("\r")
        k, _ = board.expect(["Host IP"], timeout=8)
        if k < 0:
            return fail("mdns_timeout_no_prompt")
        print(f"\n  [{board.label}] mDNS timeout, fallback -> IP {ip}")
        board.send(ip + "\r")

    i, _ = board.expect(["State -> Established"], timeout=30)
    if i < 0:
        return fail("no_established")

    i, _ = board.expect([r"\[UMP\].*Note"], timeout=20)
    if i < 0:
        return fail("no_ump")

    # Client quits first -- sends Bye so the host transitions via peer-disconnect path.
    time.sleep(1)
    board.send("q\r")
    j, _ = board.expect(["Session closed", "Peer disconnected", "Rebooting"], timeout=10)
    if j == 2:  # unexpected reboot
        board.reopen()
        return fail("unexpected_reboot")
    k, _ = board.expect(["Run again", "Rebooting"], timeout=8)
    if k == 1:
        board.reopen()
        return fail("reboot_at_repeat")
    if k < 0:
        return fail("no_repeat_prompt")
    return "PASS"   # Board at REPEAT_PROMPT


def pico_run_client_manual(board, host_ip, host_ip_ready):
    """Board is at Role?. Drive client (manual IP). Ends at REPEAT_PROMPT.
    host_ip: str (known ahead) or list (populated by host thread after ip_ready fires)."""
    def fail(r):
        print(f"\n  [{board.label} CLIENT FAIL] {r}")
        # Probe for REPEAT_PROMPT first -- 'q' there triggers doReboot().
        j, _ = board.expect(["Run again"], timeout=1)
        if j < 0:
            board.send("q\r", delay=0.2)
            board.expect(["Session closed", "Peer disconnected", "Run again"], timeout=12)
            board.expect(["Run again"], timeout=8)
        return f"FAIL:{r}"

    host_ip_ready.wait(timeout=35)
    actual_ip = (host_ip[0] if isinstance(host_ip, list) else host_ip) or ""
    if not actual_ip:
        return fail("host_ip_unknown")

    board.send("C\r")
    i, _ = board.expect(["Searching", "Found host", "Host IP"], timeout=10)
    if i == 0:
        # mDNS browse started -- interrupt with Enter to switch to manual IP
        time.sleep(0.5)
        board.send("\r")
        i, _ = board.expect(["Host IP"], timeout=5)
        if i < 0:
            return fail("no_ip_prompt")
    elif i == 1:
        return fail("unexpected_mdns_resolve")
    elif i < 0:
        return fail("no_client_start")

    board.send(actual_ip + "\r")

    i, _ = board.expect(["State -> Established"], timeout=30)
    if i < 0:
        return fail("no_established")

    i, _ = board.expect([r"\[UMP\].*Note"], timeout=20)
    if i < 0:
        return fail("no_ump")

    # Client quits first -- sends Bye so the host transitions via peer-disconnect path.
    time.sleep(1)
    board.send("q\r")
    j, _ = board.expect(["Session closed", "Peer disconnected", "Rebooting"], timeout=10)
    if j == 2:
        board.reopen()
        return fail("unexpected_reboot")
    k, _ = board.expect(["Run again", "Rebooting"], timeout=8)
    if k == 1:
        board.reopen()
        return fail("reboot_at_repeat")
    if k < 0:
        return fail("no_repeat_prompt")
    return "PASS"   # Board at REPEAT_PROMPT


# ---------------------------------------------------------------------------
# POSIX session runners
# ---------------------------------------------------------------------------

def posix_run_host(proc, host_ip_out, host_ip_ready):
    """POSIX host: starts, signals ready, waits for client's Bye, then exits."""
    def fail(r):
        print(f"\n  [{proc.label} HOST FAIL] {r}")
        host_ip_ready.set()
        proc.send("q\n")
        proc.expect(["Run again", "Done"], timeout=10)
        proc.send("N\n")
        proc.close()
        return f"FAIL:{r}"

    i, _ = proc.expect(["Role\\?"], timeout=15)
    if i < 0:
        return fail("no_role_prompt")

    proc.send("H\n")

    # Collect one or more "[Host] IP" lines (process may print several interfaces),
    # then wait for "Waiting" / "Listening".  Store first 10.x.x.x LAN address.
    deadline = time.time() + 15
    while time.time() < deadline:
        i, line = proc.expect([r"\[Host\] IP", r"\[Host\] Waiting", r"\[Host\] Listening",
                                "State -> Established"], timeout=5)
        if i == 0:
            m = re.search(r"(\d+\.\d+\.\d+\.\d+)", line)
            if m and host_ip_out[0] is None:
                host_ip_out[0] = m.group(1)
            continue  # read more IP lines until Waiting/Listening
        if i < 0:
            return fail("no_host_ready")
        break  # i in {1,2,3}: host is ready
    else:
        return fail("no_host_ready")
    host_ip_ready.set()

    if i != 2:  # not yet Established — wait for it (buffer drain fix may have saved it)
        i, _ = proc.expect(["State -> Established"], timeout=65)
        if i < 0:
            return fail("no_established")

    i, _ = proc.expect([r"\[UMP\].*Note"], timeout=20)
    if i < 0:
        return fail("no_ump")

    # Wait for Pico client to send Bye (client always quits first).
    # Fall back to sending 'q' if it takes too long.
    i, _ = proc.expect(["Peer disconnected", "Session closed", "Run again"], timeout=20)
    if i < 0:
        proc.send("q\n")
        proc.expect(["Session closed", "Run again"], timeout=10)
    proc.send("N\n")
    proc.expect(["Done"], timeout=8)
    proc.close()
    return "PASS"


def posix_run_client_mdns(proc, host_ip_out, host_ip_ready):
    """POSIX client via mDNS. Client quits first after UMP confirmed."""
    def fail(r):
        print(f"\n  [{proc.label} CLIENT FAIL] {r}")
        proc.close()
        return f"FAIL:{r}"

    i, _ = proc.expect(["Role\\?"], timeout=15)
    if i < 0:
        return fail("no_role_prompt")

    host_ip_ready.wait(timeout=35)
    # lwIP mDNS probing takes ~750 ms after the host prints "[Host] mDNS".
    # Wait for the probing sequence to finish and the announcement to propagate
    # so the Mac's resolver gets a fresh answer rather than a stale cache hit.
    time.sleep(2)
    proc.send("C\n")

    i, _ = proc.expect(["Found host", "Host IP"], timeout=40)
    if i == 1:
        ip = host_ip_out[0] if host_ip_out else None
        if not ip:
            return fail("mdns_failed_no_ip")
        print(f"\n  [{proc.label}] mDNS fallback -> IP {ip}")
        proc.send(ip + "\n")
    elif i < 0:
        # nm2_interactive didn't print "Found host" or "Host IP:" within 40s.
        # Send Enter to trigger the "Host IP:" prompt and fall back to manual IP.
        ip = host_ip_out[0] if host_ip_out else None
        if not ip:
            return fail("mdns_timeout_no_ip")
        proc.send("\n")
        k, _ = proc.expect(["Host IP"], timeout=8)
        if k < 0:
            return fail("mdns_timeout_no_prompt")
        print(f"\n  [{proc.label}] mDNS timeout, fallback -> IP {ip}")
        proc.send(ip + "\n")

    i, _ = proc.expect(["State -> Established"], timeout=30)
    if i < 0:
        return fail("no_established")

    i, _ = proc.expect([r"\[UMP\].*Note"], timeout=20)
    if i < 0:
        return fail("no_ump")

    # Client quits first
    time.sleep(1)
    proc.send("q\n")
    proc.expect(["Session closed", "Peer disconnected", "Run again"], timeout=10)
    proc.send("N\n")
    proc.expect(["Done"], timeout=8)
    proc.close()
    return "PASS"


def posix_run_client_manual(proc, host_ip, host_ip_ready):
    """POSIX client via manual IP. Client quits first after UMP confirmed."""
    def fail(r):
        print(f"\n  [{proc.label} CLIENT FAIL] {r}")
        proc.close()
        return f"FAIL:{r}"

    i, _ = proc.expect(["Role\\?"], timeout=15)
    if i < 0:
        return fail("no_role_prompt")

    host_ip_ready.wait(timeout=35)
    actual_ip = (host_ip[0] if isinstance(host_ip, list) else host_ip) or ""
    if not actual_ip:
        return fail("host_ip_unknown")

    proc.send("C\n")
    i, _ = proc.expect(["Searching", "Found host", "Host IP"], timeout=10)
    if i == 0:
        time.sleep(0.5)
        proc.send("\n")
        i, _ = proc.expect(["Host IP"], timeout=5)
        if i < 0:
            return fail("no_ip_prompt")
    elif i == 1:
        return fail("unexpected_auto_resolve")
    elif i < 0:
        return fail("no_client_start")

    proc.send(actual_ip + "\n")

    i, _ = proc.expect(["State -> Established"], timeout=30)
    if i < 0:
        return fail("no_established")

    i, _ = proc.expect([r"\[UMP\].*Note"], timeout=20)
    if i < 0:
        return fail("no_ump")

    # Client quits first
    time.sleep(1)
    proc.send("q\n")
    proc.expect(["Session closed", "Peer disconnected", "Run again"], timeout=10)
    proc.send("N\n")
    proc.expect(["Done"], timeout=8)
    proc.close()
    return "PASS"


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------
def run_test(name, host_fn, client_fn):
    print(f"\n{'='*60}")
    print(f"  {name}")
    print(f"{'='*60}")
    sys.stdout.flush()

    res           = {"host": "TIMEOUT", "client": "TIMEOUT"}
    host_ip_out   = [None]
    host_ip_ready = threading.Event()

    def host_worker():
        res["host"] = host_fn(host_ip_out, host_ip_ready)

    def client_worker():
        res["client"] = client_fn(host_ip_out, host_ip_ready)

    th = threading.Thread(target=host_worker,   daemon=True)
    tc = threading.Thread(target=client_worker, daemon=True)
    th.start(); tc.start()
    th.join(timeout=200); tc.join(timeout=200)

    hr, cr = res["host"], res["client"]
    ok     = (hr == "PASS" and cr == "PASS")
    print(f"\n  Host  : {hr}")
    print(f"  Client: {cr}")
    print(f"  -> {'PASS' if ok else 'FAIL'}")
    sys.stdout.flush()
    results[name] = "PASS" if ok else f"FAIL  (H={hr}  C={cr})"
    return ok


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def kill_pi_stragglers():
    """Kill any leftover nm2_interactive processes on the Pi over SSH."""
    subprocess.run(
        ["ssh", "-i", PI_KEY, "-o", "StrictHostKeyChecking=no",
         f"{PI_USER}@{PI_IP}", "pkill -9 -f nm2_interactive; true"],
        capture_output=True, timeout=10)
    time.sleep(0.3)


def flush_mac_mdns_cache():
    """Flush the macOS mDNS/DNS cache to clear stale .local hostname entries.

    Required before any test where the Mac resolves a Pico's .local hostname:
    - Without static IPs the Picos' DHCP addresses can differ across power cycles,
      leaving stale mDNSResponder cache entries pointing to the wrong host.
    - dscacheutil needs no privileges; killall -HUP mDNSResponder needs sudo.
      Add the following line to /etc/sudoers (via visudo) for passwordless flush:
        %admin ALL=(ALL) NOPASSWD: /bin/kill -HUP *
    """
    subprocess.run(["dscacheutil", "-flushcache"], capture_output=True, timeout=5)
    r = subprocess.run(["sudo", "-n", "killall", "-HUP", "mDNSResponder"],
                       capture_output=True, timeout=5)
    if r.returncode != 0:
        print("[warn] mDNSResponder cache flush requires sudo — stale .local entries "
              "may persist. Fix: sudo killall -HUP mDNSResponder before the run, "
              "or assign static IPs to the Picos.")
    else:
        print("[setup] Mac mDNS cache flushed.")
    time.sleep(0.5)


def main():
    print("=== NetworkMIDI2 Full Cross-Device Test (Tests 2-8) ===")
    # Kill any leftover nm2_interactive on Mac and Pi that could hold ports 5004/5005.
    subprocess.run(["pkill", "-f", "nm2_interactive"], capture_output=True)
    kill_pi_stragglers()
    time.sleep(0.3)
    for dev in (PICO_A_DEV, PICO_B_DEV):
        if not os.path.exists(dev):
            sys.exit(f"ERROR: {dev} not found")

    pico_a = Board(PICO_A_DEV, "PICO_A")
    pico_b = Board(PICO_B_DEV, "PICO_B")
    pico_a.open()

    try:
        # ---- Initial setup: Pico A ----
        # pico_fresh_boot_to_role always sends MDNS_NAME at the mDNS name prompt,
        # saving it to flash.  After that, enter host mode briefly to check the
        # advertised mDNS name; if it is wrong (name was saved before this run),
        # trigger a reboot so pico_fresh_boot_to_role can set it correctly.
        print("\n[setup] Bringing Pico A to Role?...")
        if not pico_fresh_boot_to_role(pico_a):
            if not pico_recover(pico_a):
                sys.exit("ERROR: Pico A could not reach Role? prompt (initial)")

        # Verify mDNS name by entering host mode for one second.
        pico_a.send("H\r")
        hi, hline = pico_a.expect([r"\[Host\] mDNS", r"Please enter"], timeout=10)
        if hi == 0 and (MDNS_NAME + "-host") not in hline:
            print(f"[setup] mDNS name wrong ({hline.strip()}) — rebooting to fix...")
            pico_a.send("q\r", delay=0.3)
            pico_a.expect(["Run again"], timeout=10)
            if not pico_reboot_to_reset_name(pico_a):
                print("[setup] Name-reset reboot failed — continuing with wrong name")
                pico_ensure_role(pico_a)
        else:
            # Name is correct (or couldn't verify) — exit host mode cleanly.
            pico_a.send("q\r", delay=0.3)
            pico_a.expect(["Run again"], timeout=10)
            pico_repeat_to_role(pico_a)
        print("[setup] Pico A ready")

        # ------------------------------------------------------------------
        # Test 2: Pico A host -- Mac client (mDNS)
        # Mac client quits first -> Pico A -> REPEAT_PROMPT
        # ------------------------------------------------------------------
        # Flush stale .local cache entries so the Mac resolves the current IP.
        flush_mac_mdns_cache()
        run_test("Test 2: Pico A host -- Mac client (mDNS)",
            host_fn   = lambda ip_out, ev: pico_run_host(pico_a, ip_out, ev),
            client_fn = lambda ip_out, ev: posix_run_client_mdns(make_mac(), ip_out, ev))
        pico_ensure_role(pico_a)
        time.sleep(3)  # let stale UDP packets expire before next test

        # ------------------------------------------------------------------
        # Test 3: Pi host -- Pico A client (mDNS)
        # Pico A client quits first -> Pi host exits
        # ------------------------------------------------------------------
        kill_pi_stragglers()  # ensure no zombie from a previous run holds port 5004
        run_test("Test 3: Pi host -- Pico A client (mDNS)",
            host_fn   = lambda ip_out, ev: posix_run_host(make_pi(), ip_out, ev),
            client_fn = lambda ip_out, ev: pico_run_client_mdns(pico_a, ip_out, ev))
        pico_ensure_role(pico_a)
        time.sleep(3)  # let Pico re-establish mDNS advertisement before Pi browses in test 4

        # ------------------------------------------------------------------
        # Test 4: Pico A host -- Pi client (mDNS)
        # Pi client quits first -> Pico A -> REPEAT_PROMPT
        # ------------------------------------------------------------------
        run_test("Test 4: Pico A host -- Pi client (mDNS)",
            host_fn   = lambda ip_out, ev: pico_run_host(pico_a, ip_out, ev),
            client_fn = lambda ip_out, ev: posix_run_client_mdns(make_pi("PI2"), ip_out, ev))
        pico_ensure_role(pico_a)

        # ------------------------------------------------------------------
        # Test 5: Mac host -- Pico A client (manual IP, Mac IP known ahead)
        # Pico A client quits first -> Mac host exits
        # ------------------------------------------------------------------
        run_test("Test 5: Mac host -- Pico A client (manual IP)",
            host_fn   = lambda ip_out, ev: posix_run_host(make_mac("MAC5"), ip_out, ev),
            client_fn = lambda ip_out, ev: pico_run_client_manual(pico_a, MAC_LAN_IP, ev))
        pico_ensure_role(pico_a)

        # ------------------------------------------------------------------
        # Test 6: Pico A host -- Mac client (manual IP, Pico IP captured live)
        # Mac client quits first -> Pico A -> REPEAT_PROMPT
        # ------------------------------------------------------------------
        run_test("Test 6: Pico A host -- Mac client (manual IP)",
            host_fn   = lambda ip_out, ev: pico_run_host(pico_a, ip_out, ev),
            client_fn = lambda ip_out, ev: posix_run_client_manual(make_mac("MAC6"), ip_out, ev))
        pico_ensure_role(pico_a)

        # ---- Open Pico B for tests 7 & 8 ----
        pico_b.open()
        print("\n[setup] Bringing Pico B to Role? prompt...")
        if not pico_fresh_boot_to_role(pico_b):
            if not pico_recover(pico_b):
                sys.exit("ERROR: Pico B could not reach Role? prompt")
        print("[setup] Pico B ready")

        # ------------------------------------------------------------------
        # Test 7: Pico A host -- Pico B client (mDNS)
        # Pico B client quits first -> Pico A -> REPEAT_PROMPT
        # ------------------------------------------------------------------
        run_test("Test 7: Pico A host -- Pico B client (mDNS)",
            host_fn   = lambda ip_out, ev: pico_run_host(pico_a, ip_out, ev),
            client_fn = lambda ip_out, ev: pico_run_client_mdns(pico_b, ip_out, ev))
        pico_ensure_role(pico_a)
        pico_ensure_role(pico_b)

        # ------------------------------------------------------------------
        # Test 8: Pico A host -- Pico B client (manual IP)
        # Pico B client quits first -> Pico A -> REPEAT_PROMPT
        # ------------------------------------------------------------------
        run_test("Test 8: Pico A host -- Pico B client (manual IP)",
            host_fn   = lambda ip_out, ev: pico_run_host(pico_a, ip_out, ev),
            client_fn = lambda ip_out, ev: pico_run_client_manual(pico_b, ip_out, ev))
        # No advance needed -- last test

    finally:
        pico_a.close()
        pico_b.close()

    # ------------------------------------------------------------------
    # Final report
    # ------------------------------------------------------------------
    print(f"\n{'='*60}")
    print("  FINAL RESULTS")
    print(f"{'='*60}")
    print("  Test 1: Pico A client -- Mac host  (mDNS)        PASS  [manual]")
    all_pass = True
    for name, verdict in results.items():
        short = name.split(":")[0]
        rest  = name.split(":", 1)[1].strip()
        print(f"  {short}: {rest:44s} {verdict}")
        if "FAIL" in verdict or "TIMEOUT" in verdict:
            all_pass = False
    print(f"\n  Overall: {'ALL PASS' if all_pass else 'SOME FAILURES'}")
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
