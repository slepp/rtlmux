#!/usr/bin/env python3

import socket
import subprocess
import sys
import threading
import time
import unittest


BINARY = sys.argv[1] if len(sys.argv) > 1 else "./rtlmux"
sys.argv = [sys.argv[0]]


def unused_port_pair():
    while True:
        first = socket.socket()
        first.bind(("127.0.0.1", 0))
        port = first.getsockname()[1]
        second = socket.socket()
        try:
            second.bind(("127.0.0.1", port + 1))
        except OSError:
            first.close()
            second.close()
            continue
        first.close()
        second.close()
        return port


def connect_with_retry(port):
    deadline = time.time() + 8
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            time.sleep(0.03)
    raise RuntimeError("rtlmux did not start listening")


class RtlmuxTest(unittest.TestCase):
    def start_rtlmux(self, upstream_port, listen_port, *args):
        process = subprocess.Popen(
            [
                BINARY,
                "-h", "127.0.0.1",
                "-p", str(upstream_port),
                "-l", str(listen_port),
                *args,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.addCleanup(self.stop_process, process)
        return process

    @staticmethod
    def stop_process(process):
        if process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    def test_delayed_client_waits_for_real_header(self):
        upstream = socket.socket()
        upstream.bind(("127.0.0.1", 0))
        upstream.listen()
        upstream_port = upstream.getsockname()[1]
        listen_port = unused_port_pair()
        accepted = threading.Event()
        release_header = threading.Event()

        def serve():
            connection, _ = upstream.accept()
            accepted.set()
            release_header.wait(2)
            connection.sendall(b"RTL0" + (5).to_bytes(4, "big") + (7).to_bytes(4, "big"))
            time.sleep(0.2)
            connection.close()
            upstream.close()

        thread = threading.Thread(target=serve)
        thread.start()
        self.addCleanup(thread.join, 2)
        self.start_rtlmux(upstream_port, listen_port, "-d")

        client = connect_with_retry(listen_port)
        self.addCleanup(client.close)
        self.assertTrue(accepted.wait(2))
        client.settimeout(0.15)
        with self.assertRaises(socket.timeout):
            client.recv(12)

        release_header.set()
        client.settimeout(2)
        self.assertEqual(client.recv(12), b"RTL0" + (5).to_bytes(4, "big") + (7).to_bytes(4, "big"))

    def test_fragmented_command_is_forwarded_when_complete(self):
        upstream = socket.socket()
        upstream.bind(("127.0.0.1", 0))
        upstream.listen()
        upstream_port = upstream.getsockname()[1]
        listen_port = unused_port_pair()
        received = bytearray()
        ready = threading.Event()
        partial_checked = threading.Event()
        finish = threading.Event()

        def serve():
            connection, _ = upstream.accept()
            connection.sendall(b"RTL0" + (1).to_bytes(4, "big") + (2).to_bytes(4, "big"))
            connection.settimeout(2)
            ready.set()
            try:
                received.extend(connection.recv(5))
            except socket.timeout:
                pass
            partial_checked.set()
            connection.settimeout(2)
            while len(received) < 5:
                try:
                    chunk = connection.recv(5 - len(received))
                except socket.timeout:
                    break
                if not chunk:
                    break
                received.extend(chunk)
            finish.set()
            connection.close()
            upstream.close()

        thread = threading.Thread(target=serve)
        thread.start()
        self.addCleanup(thread.join, 2)
        self.start_rtlmux(upstream_port, listen_port)
        self.assertTrue(ready.wait(2))

        client = connect_with_retry(listen_port)
        self.addCleanup(client.close)
        client.settimeout(2)
        self.assertEqual(client.recv(12), b"RTL0" + (1).to_bytes(4, "big") + (2).to_bytes(4, "big"))

        command = b"\x01" + (100_000_000).to_bytes(4, "big")
        client.sendall(command[:1])
        self.assertTrue(partial_checked.wait(4))
        self.assertEqual(received, b"")
        client.sendall(command[1:])

        self.assertTrue(finish.wait(2))
        self.assertEqual(received, command)

    def test_invalid_ports_are_rejected(self):
        upstream_result = subprocess.run(
            [BINARY, "-p", "-1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=2,
        )
        self.assertNotEqual(upstream_result.returncode, 0)
        self.assertIn("rtl_tcp port must be between", upstream_result.stderr)

        listen_result = subprocess.run(
            [BINARY, "-l", "70000"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=2,
        )
        self.assertNotEqual(listen_result.returncode, 0)
        self.assertIn("Listening port must be between", listen_result.stderr)

    def test_shutdown_during_reconnect_is_clean(self):
        upstream = socket.socket()
        upstream.bind(("127.0.0.1", 0))
        upstream.listen()
        upstream_port = upstream.getsockname()[1]
        listen_port = unused_port_pair()
        closed = threading.Event()

        def serve():
            connection, _ = upstream.accept()
            connection.sendall(b"RTL0" + (1).to_bytes(4, "big") + (2).to_bytes(4, "big"))
            connection.close()
            upstream.close()
            closed.set()

        thread = threading.Thread(target=serve)
        thread.start()
        self.addCleanup(thread.join, 2)
        process = self.start_rtlmux(upstream_port, listen_port)

        self.assertTrue(closed.wait(2))
        time.sleep(0.2)
        process.terminate()
        self.assertEqual(process.wait(timeout=3), 0)

    def test_delayed_restart_accepts_a_new_client(self):
        upstream = socket.socket()
        upstream.bind(("127.0.0.1", 0))
        upstream.listen()
        upstream_port = upstream.getsockname()[1]
        listen_port = unused_port_pair()
        disconnected = [threading.Event(), threading.Event()]

        def serve():
            for index in range(2):
                connection, _ = upstream.accept()
                connection.sendall(b"RTL0" + (index + 1).to_bytes(4, "big") + (2).to_bytes(4, "big"))
                connection.settimeout(3)
                try:
                    while connection.recv(1024):
                        pass
                except socket.timeout:
                    pass
                connection.close()
                disconnected[index].set()
            upstream.close()

        thread = threading.Thread(target=serve)
        thread.start()
        self.addCleanup(thread.join, 4)
        self.start_rtlmux(upstream_port, listen_port, "-d", "-r")

        first = connect_with_retry(listen_port)
        first.settimeout(2)
        self.assertEqual(first.recv(12), b"RTL0" + (1).to_bytes(4, "big") + (2).to_bytes(4, "big"))
        first.close()
        self.assertTrue(disconnected[0].wait(3))

        second = connect_with_retry(listen_port)
        second.settimeout(2)
        self.assertEqual(second.recv(12), b"RTL0" + (2).to_bytes(4, "big") + (2).to_bytes(4, "big"))
        second.close()
        self.assertTrue(disconnected[1].wait(3))


if __name__ == "__main__":
    unittest.main()
