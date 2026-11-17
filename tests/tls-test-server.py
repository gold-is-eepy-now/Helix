#!/usr/bin/env python3
"""tests/tls-test-server.py PORT CERT KEY — a TLS 1.3 HTTPS server for the QEMU check.

Binds to loopback only (QEMU's guest reaches it at 10.0.2.2). Answers each connection with
one fixed body and closes it, as an HTTP/1.0 server does, so the kernel's client sees the
end of the response. Used by tests/shell-test.py; not part of the kernel."""
import http.server
import ssl
import sys

TLS_TEST_BODY = b"hello from the Helix TLS test server\n"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(TLS_TEST_BODY)))
        self.end_headers()
        self.wfile.write(TLS_TEST_BODY)

    def log_message(self, *args):
        pass


def main():
    port, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(cert, key)
    server = http.server.HTTPServer(("127.0.0.1", port), Handler)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
