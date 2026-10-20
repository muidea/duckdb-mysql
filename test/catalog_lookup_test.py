"""Test extension catalog binding with a local synthetic MySQL wire fixture."""
import json
import re
import socketserver
import struct
import subprocess
import sys
import threading


def encoded(value):
    data = str(value).encode()
    if len(data) < 251:
        return bytes([len(data)]) + data
    assert len(data) < 1 << 24
    return b'\xfd' + len(data).to_bytes(3, 'little') + data


def column(name):
    kind = 8 if name in ('numeric_precision', 'numeric_scale', 'one') else 253
    return b''.join(encoded(s) for s in ('def', 'fixture', '', '', name, name)) + b'\x0c' + struct.pack('<HIBHB', 33, 1024, kind, 0, 0) + b'\0\0'


EOF = b'\xfe\0\0\x02\0'
OK = b'\0\0\0\x02\0\0\0'


class Fixture(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.queries = []
        self.errors = []
        self.tables = {'Mixed': ['canonical_id', 'label'], 'other': ['other_id']}

    def handle_error(self, request, client_address):
        self.errors.append(repr(sys.exc_info()[1]))

    def query(self, sql):
        q = ' '.join(sql.split())
        self.queries.append(q)
        if q.startswith(('SET ', 'START ', 'BEGIN', 'COMMIT', 'ROLLBACK')):
            return [], []
        if 'information_schema.schemata' in q:
            return ['schema_name'], [['fixture']]
        if 'information_schema.columns' in q:
            match = re.search(r"table_name='([^']+)'", q)
            names = [n for n in sorted(self.tables) if not match or n.lower() == match[1].lower()]
            rows = []
            for name in names:
                for col in self.tables[name]:
                    row = [col, 'int', 'int(11)', None, 'NO', '10', '0']
                    rows.append(row + [name] if match else [name] + row)
            names = ['column_name', 'data_type', 'column_type', 'column_default', 'is_nullable', 'numeric_precision', 'numeric_scale']
            return names + ['table_name'] if match else ['table_name'] + names, rows
        if q.upper() == 'SELECT 1':
            return ['one'], [[1]]
        if q == "SELECT REPEAT('x', 65536)":
            return ['payload'], [['x'*65536]]
        if 'VERSION()' in q.upper():
            return ['version'], [['8.0.36']]
        if '@@version_comment' in q:
            return ['comment'], [['MySQL Community Server']]
        if 'current_version()' in q:
            return ['version'], [['']]
        raise AssertionError(q)


class Handler(socketserver.BaseRequestHandler):
    def exact(self, count):
        data = b''
        while len(data) < count:
            part = self.request.recv(count-len(data))
            if not part:
                raise EOFError()
            data += part
        return data

    def read(self):
        header = self.exact(4)
        return self.exact(int.from_bytes(header[:3], 'little'))

    def send(self, payload, seq):
        self.request.sendall(len(payload).to_bytes(3, 'little') + bytes([seq]) + payload)

    def rows(self, names, rows, binary=False):
        if not names:
            self.send(OK, 1)
            return
        seq = 1
        for payload in [bytes([len(names)])] + [column(n) for n in names] + [EOF]:
            self.send(payload, seq)
            seq += 1
        for row in rows:
            if binary:
                bitmap = bytearray((len(names)+9)//8)
                payload = b''
                for i, value in enumerate(row):
                    if value is None:
                        bitmap[(i+2)//8] |= 1 << ((i+2)%8)
                    else:
                        payload += struct.pack('<q', int(value)) if names[i] in ('numeric_precision', 'numeric_scale', 'one') else encoded(value)
                payload = b'\0' + bytes(bitmap) + payload
            else:
                payload = b''.join(b'\xfb' if v is None else encoded(v) for v in row)
            self.send(payload, seq)
            seq += 1
        self.send(EOF, seq)

    def handle(self):
        capabilities = 1 | 4 | 8 | 512 | 8192 | 32768 | 131072 | 524288
        handshake = (b'\x0a8.0.36\0' + struct.pack('<I', 1) + b'12345678\0' +
                     struct.pack('<H', capabilities & 65535) + b'\x21' + struct.pack('<HHB', 2, capabilities >> 16, 21) +
                     b'\0'*10 + b'abcdefghijkl\0mysql_native_password\0')
        statements = {}
        try:
            self.send(handshake, 0)
            self.read()
            self.send(OK, 2)
            while True:
                payload = self.read()
                command = payload[0]
                if command == 1:
                    return
                if command == 3:
                    if payload[1:] == b'SHOW FRONTENDS':
                        self.send(b'\xff' + struct.pack('<H', 1064) + b'#42000synthetic unsupported command', 1)
                    else:
                        self.rows(*self.server.query(payload[1:].decode()))
                elif command == 22:
                    names, rows = self.server.query(payload[1:].decode())
                    sid = len(statements)+1
                    statements[sid] = names, rows
                    self.send(b'\0' + struct.pack('<IHHBH', sid, len(names), 0, 0, 0), 1)
                    for i, name in enumerate(names):
                        self.send(column(name), i+2)
                    if names:
                        self.send(EOF, len(names)+2)
                elif command == 23:
                    sid = struct.unpack('<I', payload[1:5])[0]
                    self.rows(*statements[sid], binary=True)
                elif command == 25:
                    pass
                elif command in (14, 31):
                    self.send(OK, 1)
                else:
                    raise AssertionError(command)
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass


def main():
    fixture = Fixture()
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    sql = "LOAD '%s'; ATTACH 'host=127.0.0.1 port=%d user=fixture database=fixture' AS src (TYPE mysql, READ_ONLY);" % (sys.argv[2], fixture.server_address[1])
    sql += '''
DESCRIBE SELECT * FROM src.fixture.mIXED;
DESCRIBE SELECT * FROM src.fixture.Mixed;
DESCRIBE SELECT * FROM src.fixture.other;
SELECT count(*) FROM duckdb_tables() WHERE database_name='src' AND schema_name='fixture';
DESCRIBE SELECT * FROM src.fixture.missing;
'''
    try:
        run = subprocess.run([sys.argv[1], '-unsigned', '-csv', '-noheader'], input=sql, capture_output=True, text=True, timeout=60)
        assert 'canonical_id,INTEGER' in run.stdout and 'other_id,INTEGER' in run.stdout, (run.returncode, run.stdout, run.stderr)
        assert '\n2\n' in run.stdout, run.stdout
        assert 'missing' in run.stderr and 'does not exist' in run.stderr, run.stderr
        assert not fixture.errors, fixture.errors
        queries = [q for q in fixture.queries if 'information_schema.columns' in q]
        assert len(queries) >= 4 and "table_name='mIXED'" in queries[0], queries
        assert "table_name='other'" in queries[1], queries
        assert "table_name=" not in queries[2], queries
        assert "table_name='missing'" in queries[3], queries
        print(json.dumps({'point_lookup': True, 'canonical_case': True, 'cached_lookup': True,
                          'enumeration_tables': 2, 'missing_table': True}))
    finally:
        fixture.shutdown()
        fixture.server_close()


if __name__ == '__main__':
    main()
