#!/usr/bin/env python3
"""
nm2_test.py — Automated two-board Network MIDI 2.0 session test.

  Board A (host)  : /dev/cu.usbmodem2112201
  Board B (client): /dev/cu.usbmodem21121301

Usage:
  1. Run this script.
  2. When prompted, power on (or press RESET on) both boards.
  3. The script drives the full boot → WiFi → session → verify → reboot sequence.

Requires: pyserial  (pip install pyserial)
"""

import sys
import os
import time
import re
import threading

try:
    import serial
except ImportError:
    sys.exit("pyserial not found — run: pip install pyserial")

HOST_DEV   = "/dev/cu.usbmodem2112201"
CLIENT_DEV = "/dev/cu.usbmodem21121301"
BAUD       = 115200

# ---------------------------------------------------------------------------
# Board wrapper
# ---------------------------------------------------------------------------

class Board:
    def __init__(self, dev, label):
        self.dev   = dev
        self.label = label
        self.ser   = None
        self._buf  = ""

    def open(self):
        self.ser = serial.Serial(self.dev, BAUD, timeout=0.1)
        self._buf = ""

    def send(self, text, delay=0.08):
        time.sleep(delay)
        if self.ser:
            self.ser.write(text.encode())

    def expect(self, patterns, timeout=30):
        """
        Read output until a line matches one of the regex patterns.
        Returns (pattern_index, matched_line) or (-1, "") on timeout.
        Every non-empty line received is printed with the board label.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
            except Exception:
                time.sleep(0.05)
                continue
            if not chunk:
                continue
            self._buf += chunk.decode("utf-8", errors="replace")
            parts = re.split(r"\r\n|\r|\n", self._buf)
            self._buf = parts[-1]           # keep partial trailing fragment
            for line in parts[:-1]:
                line = line.strip()
                if not line:
                    continue
                print(f"  [{self.label}] {line}")
                for i, pat in enumerate(patterns):
                    if re.search(pat, line, re.IGNORECASE):
                        return i, line
        # Timeout — check partial buffer
        partial = self._buf.strip()
        if partial:
            print(f"  [{self.label}] (partial) {partial}")
            for i, pat in enumerate(patterns):
                if re.search(pat, partial, re.IGNORECASE):
                    return i, partial
        return -1, ""

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None


# ---------------------------------------------------------------------------
# Wait for serial device to appear (handles plug-in after script starts)
# ---------------------------------------------------------------------------

def wait_for_device(dev, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(dev):
            return True
        time.sleep(0.25)
    return False


# ---------------------------------------------------------------------------
# Board test routines
# ---------------------------------------------------------------------------

def boot_to_wifi_prompt(board, timeout=60):
    """
    Wait for board to reach the WiFi SSID prompt.
    Handles three cases:
      - Fresh boot: board prints banner then WiFi prompt automatically.
      - Already awake and silent: board is sitting at the prompt with nothing
        to send; we poke it with \\r after 5 s so it echoes or re-prompts.
      - Mid-session: board eventually produces recognisable output.
    Returns True when we are confident the board is at (or just past) the
    WiFi prompt and ready to receive credentials.
    """
    POKE_AFTER = 5   # seconds of silence before sending a \\r poke

    i, line = board.expect([
        r"Network MIDI 2\.0",   # 0: fresh boot banner
        r"WiFi SSID",           # 1: already at SSID prompt
        r"Saved WiFi",          # 2: already at saved-creds prompt
        r"Using saved WiFi",    # 3: already accepted (poke triggered it)
        r"Connected",           # 4: already connected (very stale state)
    ], timeout=POKE_AFTER)

    if i < 0:
        # No output in 5 s — board is awake and silent at a prompt.
        # Send a blank line to poke it; it will echo and re-display state.
        print(f"  [{board.label}] (no output — poking with Enter)")
        board.send("\r", delay=0)
        i, line = board.expect([
            r"Network MIDI 2\.0",
            r"WiFi SSID",
            r"Saved WiFi",
            r"Using saved WiFi",
            r"Connected",
        ], timeout=timeout - POKE_AFTER)
        if i < 0:
            return False

    if i == 0:
        # Got the boot banner — wait for the WiFi prompt that follows
        i, line = board.expect([r"WiFi SSID", r"Saved WiFi"], timeout=15)
        if i < 0:
            return False

    # i == 1/2 → at prompt; i == 3 → poke triggered saved-WiFi flow (handled
    # by run_host/client which will see "Connected" next); i == 4 → already
    # connected (unusual, but run_host/client will fail gracefully).
    return True


def run_host(board, host_ip_out, host_ip_ready):
    def fail(reason):
        print(f"\n  [HOST FAIL] {reason}")
        return f"FAIL:{reason}"

    if not boot_to_wifi_prompt(board):
        return fail("no_boot_banner")

    # Use saved WiFi (Enter); firmware retries up to 3× internally (45 s max)
    board.send("\r")
    i, _ = board.expect(["Connected!", r"Connect failed after"], timeout=65)
    if i != 0:
        return fail("wifi_connect")

    # Use saved mDNS name (Enter)
    board.send("\r")
    i, _ = board.expect([r"Role\?"], timeout=15)
    if i < 0:
        return fail("role_prompt")

    board.send("H\r")

    # Capture the host IP printed as "[Host] IP    : X.X.X.X  port 5004"
    i, line = board.expect([r"\[Host\] IP"], timeout=10)
    if i == 0:
        m = re.search(r"(\d+\.\d+\.\d+\.\d+)", line)
        if m:
            host_ip_out[0] = m.group(1)
            host_ip_ready.set()
            print(f"\n  [HOST] IP captured: {host_ip_out[0]}")

    # Wait for client to connect and session to reach Established
    i, _ = board.expect(["State -> Established"], timeout=60)
    if i < 0:
        return fail("no_established")

    # Verify MIDI note messages are flowing (host receives from client)
    i, _ = board.expect([r"\[UMP\].*Note"], timeout=15)
    if i < 0:
        return fail("no_ump_received")

    time.sleep(3)           # let a few notes exchange
    board.send("q\r")
    time.sleep(0.5)
    board.send("N\r")       # trigger reboot

    board.expect(["Rebooting", "Session closed"], timeout=8)
    return "PASS"


def run_client(board, host_ip_out, host_ip_ready):
    def fail(reason):
        print(f"\n  [CLIENT FAIL] {reason}")
        return f"FAIL:{reason}"

    if not boot_to_wifi_prompt(board):
        return fail("no_boot_banner")

    # Use saved WiFi
    board.send("\r")
    i, _ = board.expect(["Connected!", r"Connect failed after"], timeout=65)
    if i != 0:
        return fail("wifi_connect")

    # Use saved mDNS name
    board.send("\r")
    i, _ = board.expect([r"Role\?"], timeout=15)
    if i < 0:
        return fail("role_prompt")

    board.send("C\r")

    # mDNS resolution — watch for success or the in-progress "Resolving" message.
    i, line = board.expect([
        "Found host",    # 0: resolved before we could act — proceed directly
        "Connecting to", # 1: already connecting
        "Resolving",     # 2: in progress — interrupt with Enter then supply IP
    ], timeout=10)

    if i == 2:
        # mDNS is searching. Wait for the host IP captured from the host board,
        # then press Enter to interrupt mDNS and enter the IP manually.
        print(f"\n  [CLIENT] mDNS in progress — waiting for host IP then interrupting")
        got = host_ip_ready.wait(timeout=25)
        if not got or not host_ip_out[0]:
            return fail("host_ip_unavailable")
        board.send("\r")                    # Enter → interrupt mDNS
        board.expect(["Host IP"], timeout=5)
        board.send(host_ip_out[0] + "\r")  # supply the IP
    elif i < 0:
        return fail("mdns_no_response")

    # Wait for session Established
    i, _ = board.expect(["State -> Established"], timeout=60)
    if i < 0:
        return fail("no_established")

    # Verify MIDI notes
    i, _ = board.expect([r"\[UMP\].*Note"], timeout=15)
    if i < 0:
        return fail("no_ump_received")

    time.sleep(3)
    board.send("q\r")
    time.sleep(0.5)
    board.send("N\r")

    board.expect(["Rebooting", "Session closed"], timeout=8)
    return "PASS"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print("=== NetworkMIDI2 Two-Board Session Test ===")
    print(f"  Host  : {HOST_DEV}")
    print(f"  Client: {CLIENT_DEV}\n")

    # Wait for both USB serial devices to appear, polling every 250 ms.
    # The Pico 2 W USB CDC device file appears within ~1 s of power-on,
    # and the firmware waits 2 s before printing anything — so opening
    # the port the moment the device file appears will catch all output.
    print("Plug in BOTH boards now (USB power-on).")
    print("Waiting for serial devices to appear...\n")

    deadline = time.time() + 90
    while time.time() < deadline:
        h = os.path.exists(HOST_DEV)
        c = os.path.exists(CLIENT_DEV)
        missing = ([HOST_DEV]   if not h else []) + ([CLIENT_DEV] if not c else [])
        if not missing:
            break
        print(f"\r  Still waiting: {missing}   ", end="", flush=True)
        time.sleep(0.25)
    else:
        sys.exit("\nERROR: timed out waiting for USB devices (90 s)")

    print(f"\n  Both devices detected — opening ports and listening...\n")

    # Open ports immediately — do NOT flush the input buffer;
    # we want every byte from the moment the port opens.
    try:
        host_board   = Board(HOST_DEV,   "HOST  ")
        client_board = Board(CLIENT_DEV, "CLIENT")
        host_board.open()
        client_board.open()
    except serial.SerialException as e:
        sys.exit(f"Cannot open serial port: {e}")

    host_ip_out   = [None]
    host_ip_ready = threading.Event()
    results       = {}

    def host_thread():
        results["host"] = run_host(host_board, host_ip_out, host_ip_ready)

    def client_thread():
        results["client"] = run_client(client_board, host_ip_out, host_ip_ready)

    th = threading.Thread(target=host_thread,   daemon=True)
    tc = threading.Thread(target=client_thread, daemon=True)
    th.start()
    tc.start()
    th.join(timeout=180)
    tc.join(timeout=180)

    host_board.close()
    client_board.close()

    print("\n=== RESULTS ===")
    hr = results.get("host",   "TIMEOUT")
    cr = results.get("client", "TIMEOUT")
    print(f"  Host  : {hr}")
    print(f"  Client: {cr}")
    ok = (hr == "PASS" and cr == "PASS")
    print(f"  Overall: {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
