import argparse
import json
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib


class Client:
    def __init__(self, path, segments=16):
        self.stderr_path = path.with_name(path.name + ".stderr")
        self.stderr = self.stderr_path.open("wb")
        self.proc = subprocess.Popen([CLI, "--wal", str(path), "--segments", str(segments),
                                      "--batch-wait-us", "0"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.stderr, text=True, encoding="utf-8")
        self.lines = queue.Queue()
        self.closed = False
        self.reader = threading.Thread(target=self.read_loop, daemon=True)
        self.reader.start()

    def read_loop(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def receive(self):
        line = self.lines.get(timeout=5)
        if line is None:
            raise RuntimeError("CLI je prekinut: " + self.stderr_path.read_text(errors="replace"))
        return line

    def send(self, command):
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()

    def request(self, command):
        self.send(command)
        return self.receive()

    def cleanup(self, kill=False):
        if self.closed:
            return
        if kill and self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=5)
        try:
            self.proc.stdin.close()
        except BrokenPipeError:
            pass
        self.reader.join(timeout=2)
        self.proc.stdout.close()
        self.stderr.close()
        self.closed = True


def put(key, value):
    return "put " + json.dumps(key) + " " + json.dumps(value)


def record(sequence, operation=1, key=b"k", value=b"v"):
    body = struct.pack("<QBII", sequence, operation, len(key), len(value)) + key + value
    body += struct.pack("<I", zlib.crc32(body))
    return struct.pack("<I", len(body)) + body


class Recovery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="storage-recovery-")
        self.path = Path(self.temp.name) / "data.wal"
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            client.cleanup(kill=True)
        self.temp.cleanup()

    def start(self, segments=16):
        client = Client(self.path, segments)
        self.clients.append(client)
        self.assertTrue(client.receive().startswith("READY"))
        return client

    def stop(self, client):
        self.assertEqual(client.request("quit"), "BYE")
        client.cleanup()
        self.assertEqual(client.proc.returncode, 0)

    def test_acknowledged_changes_survive_kill(self):
        client = self.start()
        expected = {}
        for i in range(40):
            key, value = "key" + str(i), "value" + str(i)
            self.assertEqual(client.request(put(key, value)), "OK")
            expected[key] = value
        for i in range(0, 40, 2):
            key = "key" + str(i)
            self.assertEqual(client.request(put(key, "overwritten")), "OK")
            expected[key] = "overwritten"
        for i in range(1, 40, 4):
            key = "key" + str(i)
            self.assertEqual(client.request("erase " + json.dumps(key)), "ERASED")
            expected.pop(key)
        client.send(put("unconfirmed", "maybe present"))
        client.cleanup(kill=True)

        restarted = self.start(segments=1)
        for i in range(40):
            key = "key" + str(i)
            answer = restarted.request("get " + json.dumps(key))
            if key in expected:
                self.assertEqual(answer, "VALUE " + json.dumps(expected[key]))
            else:
                self.assertEqual(answer, "NOT_FOUND")
        self.assertEqual(restarted.request(put("after-restart", "works")), "OK")
        self.stop(restarted)
        again = self.start(segments=4)
        self.assertEqual(again.request('get "after-restart"'), 'VALUE "works"')
        self.stop(again)

    def test_partial_header_and_body_are_truncated_before_append(self):
        for tail in [b"\x20\x00", struct.pack("<I", 50) + b"partial"]:
            with self.subTest(tail=tail):
                if self.path.exists():
                    self.path.unlink()
                original = record(1, key=b"saved", value=b"value")
                self.path.write_bytes(original + tail)
                client = self.start()
                self.assertEqual(self.path.stat().st_size, len(original))
                self.assertEqual(client.request('get "saved"'), 'VALUE "value"')
                self.assertEqual(client.request(put("new", "append")), "OK")
                self.stop(client)
                restarted = self.start()
                self.assertEqual(restarted.request('get "saved"'), 'VALUE "value"')
                self.assertEqual(restarted.request('get "new"'), 'VALUE "append"')
                self.stop(restarted)

    def test_corrupt_complete_record_is_refused_without_truncation(self):
        data = bytearray(record(1))
        data[-1] ^= 0x01
        self.path.write_bytes(data)
        result = subprocess.run([CLI, "--wal", str(self.path)], input="", text=True,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn("CRC32", result.stderr)
        self.assertNotIn("READY", result.stdout)
        self.assertEqual(self.path.read_bytes(), data)

    def test_invalid_sequence_operation_and_lengths_are_refused(self):
        cases = [record(2), record(1) + record(1), record(1, operation=9),
                 record(1, operation=2, value=b"not empty"),
                 record(1, key=b"k" * 257), struct.pack("<I", 0), struct.pack("<I", 5000)]
        for data in cases:
            with self.subTest(size=len(data)):
                self.path.write_bytes(data)
                result = subprocess.run([CLI, "--wal", str(self.path)], input="", text=True,
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertNotIn("READY", result.stdout)
                self.assertEqual(self.path.read_bytes(), data)

    def test_second_process_cannot_open_live_wal(self):
        client = self.start()
        result = subprocess.run([CLI, "--wal", str(self.path)], input="", text=True,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(client.request(put("still", "works")), "OK")
        self.stop(client)
        reopened = self.start()
        self.assertEqual(reopened.request('get "still"'), 'VALUE "works"')
        self.stop(reopened)

    def test_cli_quotes_and_errors(self):
        client = self.start()
        self.assertEqual(client.request(put("a b", 'value "with quotes"')), "OK")
        self.assertEqual(client.request('get "a b"'), 'VALUE "value \\"with quotes\\""')
        self.assertTrue(client.request('put "missing-value"').startswith("ERROR"))
        self.assertTrue(client.request('get "a b" extra').startswith("ERROR"))
        self.assertEqual(client.request('erase "missing"'), "NOT_FOUND")
        self.stop(client)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    args, rest = parser.parse_known_args()
    CLI = str(Path(args.cli).resolve())
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
