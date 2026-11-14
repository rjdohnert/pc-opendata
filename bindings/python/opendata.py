"""
Python DB-API 2.0 (PEP 249) driver for PC/OpenData SQL.
Zero external dependencies (uses standard library ctypes and subprocess).
"""

import os
import sys
import ctypes
from typing import Optional, List, Tuple, Any

apilevel = "2.0"
threadsafety = 1
paramstyle = "qmark"


class Error(Exception):
    pass


class DatabaseError(Error):
    pass


class OperationalError(DatabaseError):
    pass


class IntegrityError(DatabaseError):
    pass


class ProgrammingError(DatabaseError):
    pass


class _OpendataLib:
    _instance = None

    @classmethod
    def get(cls):
        if cls._instance is None:
            cls._instance = cls._load()
        return cls._instance

    @classmethod
    def _load(cls):
        search_paths = [
            os.path.join(os.path.dirname(__file__), "..", "..", "bin", "opendata.dll"),
            os.path.join(os.path.dirname(__file__), "..", "..", "bin", "opendata.exe"),
            "opendata.dll",
            "opendata.exe",
        ]
        for p in search_paths:
            p_abs = os.path.abspath(p)
            if os.path.exists(p_abs):
                try:
                    lib = ctypes.CDLL(p_abs)
                    if hasattr(lib, "opendata_open"):
                        cls._setup_signatures(lib)
                        return lib
                except Exception:
                    continue
        return None

    @classmethod
    def _setup_signatures(cls, lib):
        lib.opendata_open.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_uint,
            ctypes.c_char_p,
        ]
        lib.opendata_open.restype = ctypes.c_bool

        lib.opendata_close.argtypes = []
        lib.opendata_close.restype = None

        lib.opendata_exec.argtypes = [ctypes.c_char_p]
        lib.opendata_exec.restype = ctypes.c_bool

        lib.opendata_commit.argtypes = []
        lib.opendata_commit.restype = ctypes.c_bool

        lib.opendata_rollback.argtypes = []
        lib.opendata_rollback.restype = ctypes.c_bool

        if hasattr(lib, "opendata_errmsg"):
            lib.opendata_errmsg.argtypes = []
            lib.opendata_errmsg.restype = ctypes.c_char_p

        lib.opendata_prepare.argtypes = [ctypes.c_char_p]
        lib.opendata_prepare.restype = ctypes.c_void_p

        lib.opendata_step.argtypes = [ctypes.c_void_p]
        lib.opendata_step.restype = ctypes.c_bool

        lib.opendata_finalize.argtypes = [ctypes.c_void_p]
        lib.opendata_finalize.restype = None

        lib.opendata_column_count.argtypes = [ctypes.c_void_p]
        lib.opendata_column_count.restype = ctypes.c_int

        lib.opendata_column_name.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.opendata_column_name.restype = ctypes.c_char_p

        lib.opendata_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.opendata_column_text.restype = ctypes.c_char_p

        lib.opendata_column_is_null.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.opendata_column_is_null.restype = ctypes.c_bool

        if hasattr(lib, "opendata_bind_int"):
            lib.opendata_bind_int.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_int64,
            ]
            lib.opendata_bind_int.restype = ctypes.c_bool

        if hasattr(lib, "opendata_bind_text"):
            lib.opendata_bind_text.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_char_p,
            ]
            lib.opendata_bind_text.restype = ctypes.c_bool

        if hasattr(lib, "opendata_bind_double"):
            lib.opendata_bind_double.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_double,
            ]
            lib.opendata_bind_double.restype = ctypes.c_bool


class Cursor:
    def __init__(self, connection):
        self.connection = connection
        self.description = None
        self.rowcount = -1
        self._rows = []
        self._pos = 0

    def execute(self, operation: str, parameters: Optional[Tuple[Any, ...]] = None):
        if not self.connection or self.connection._closed:
            raise ProgrammingError("Cannot operate on closed connection")

        lib = _OpendataLib.get()
        if lib and hasattr(lib, "opendata_prepare"):
            sql_b = operation.encode("utf-8")
            stmt = lib.opendata_prepare(sql_b)
            if not stmt:
                err = ""
                if hasattr(lib, "opendata_errmsg"):
                    err = (lib.opendata_errmsg() or b"").decode(
                        "utf-8", errors="replace"
                    )
                raise DatabaseError(err or "Failed to prepare SQL statement")

            try:
                if parameters:
                    for idx, val in enumerate(parameters):
                        if val is None:
                            if hasattr(lib, "opendata_bind_null"):
                                lib.opendata_bind_null(stmt, idx + 1)
                        elif isinstance(val, int):
                            lib.opendata_bind_int(stmt, idx + 1, val)
                        elif isinstance(val, float):
                            if hasattr(lib, "opendata_bind_double"):
                                lib.opendata_bind_double(stmt, idx + 1, val)
                            else:
                                lib.opendata_bind_text(
                                    stmt, idx + 1, str(val).encode("utf-8")
                                )
                        else:
                            lib.opendata_bind_text(
                                stmt, idx + 1, str(val).encode("utf-8")
                            )

                cols = lib.opendata_column_count(stmt)
                if cols > 0:
                    self.description = [
                        (
                            lib.opendata_column_name(stmt, c).decode(
                                "utf-8", errors="replace"
                            ),
                            None,
                            None,
                            None,
                            None,
                            None,
                            None,
                        )
                        for c in range(cols)
                    ]
                else:
                    self.description = None

                self._rows = []
                while lib.opendata_step(stmt):
                    row = []
                    for c in range(cols):
                        if lib.opendata_column_is_null(stmt, c):
                            row.append(None)
                        else:
                            txt = lib.opendata_column_text(stmt, c).decode(
                                "utf-8", errors="replace"
                            )
                            row.append(txt)
                    self._rows.append(tuple(row))

                self.rowcount = len(self._rows)
                self._pos = 0
            finally:
                lib.opendata_finalize(stmt)
        else:
            # Fallback to CLI execution if lib not loaded
            import subprocess
            import json
            import re
            import math

            formatted_sql = operation
            if parameters:
                p_list = list(parameters)
                p_idx = 0

                def _param_sub(match):
                    nonlocal p_idx
                    if p_idx >= len(p_list):
                        return '?'
                    val = p_list[p_idx]
                    p_idx += 1
                    if val is None:
                        return 'NULL'
                    if isinstance(val, (int, float)):
                        if isinstance(val, float) and (math.isnan(val) or math.isinf(val)):
                            return 'NULL'
                        return str(val)
                    if isinstance(val, bool):
                        return '1' if val else '0'
                    if isinstance(val, (list, tuple)):
                        return '[' + ', '.join(str(float(x)) for x in val) + ']'
                    s = str(val).replace('\0', '').replace("'", "''")
                    return f"'{s}'"

                formatted_sql = re.sub(r'\?', _param_sub, operation)

            bin_cmd = "opendata"
            candidates = [
                os.path.join(os.path.dirname(__file__), "..", "..", "bin", "opendata.exe"),
                os.path.join(os.path.dirname(__file__), "..", "..", "bin", "opendata"),
                "opendata.exe",
                "opendata"
            ]
            for c in candidates:
                if os.path.exists(c):
                    bin_cmd = os.path.abspath(c)
                    break

            cmd = [bin_cmd]
            if self.connection.database:
                cmd.extend(["--database", self.connection.database])
            if getattr(self.connection, "user", None):
                cmd.extend(["-u", self.connection.user])
            if getattr(self.connection, "password", None):
                cmd.extend(["-p", self.connection.password])
            if getattr(self.connection, "port", None) and not self.connection.database:
                cmd.extend(["--port", str(self.connection.port)])
            cmd.extend(["--format", "json", "-e", formatted_sql])

            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode != 0:
                raise DatabaseError(res.stderr or res.stdout)
            self._rows = []
            if res.stdout.strip().startswith("["):
                try:
                    data = json.loads(res.stdout)
                    if data:
                        cols = list(data[0].keys())
                        self.description = [
                            (c, None, None, None, None, None, None) for c in cols
                        ]
                        for d in data:
                            self._rows.append(tuple(d.get(c) for c in cols))
                except Exception:
                    pass
            self.rowcount = len(self._rows)
            self._pos = 0
        return self

    def executemany(self, operation: str, seq_of_parameters):
        for params in seq_of_parameters:
            self.execute(operation, params)
        return self

    def fetchone(self) -> Optional[Tuple[Any, ...]]:
        if self._pos < len(self._rows):
            row = self._rows[self._pos]
            self._pos += 1
            return row
        return None

    def fetchmany(self, size: Optional[int] = None) -> List[Tuple[Any, ...]]:
        if size is None:
            size = 1
        res = self._rows[self._pos : self._pos + size]
        self._pos += len(res)
        return res

    def fetchall(self) -> List[Tuple[Any, ...]]:
        res = self._rows[self._pos :]
        self._pos = len(self._rows)
        return res

    def close(self):
        self._rows = []
        self._pos = 0


class Connection:
    def __init__(self, database: str = "", **kwargs):
        self.database = database
        self.user = kwargs.get("user") or kwargs.get("username")
        self.password = kwargs.get("password")
        self.port = kwargs.get("port")
        self._closed = False
        lib = _OpendataLib.get()
        if lib and database:
            db_b = database.encode("utf-8")
            user_b = self.user.encode("utf-8") if self.user else None
            port_u = int(self.port) if self.port else 0
            pwd_b = self.password.encode("utf-8") if self.password else None
            if not lib.opendata_open(db_b, user_b, port_u, pwd_b):
                err = ""
                if hasattr(lib, "opendata_errmsg"):
                    err = (lib.opendata_errmsg() or b"").decode(
                        "utf-8", errors="replace"
                    )
                raise OperationalError(
                    err or f"Could not open database {database}"
                )

    def cursor(self) -> Cursor:
        if self._closed:
            raise ProgrammingError("Cannot create cursor on closed connection")
        return Cursor(self)

    def commit(self):
        if self._closed:
            raise ProgrammingError("Cannot commit on closed connection")
        lib = _OpendataLib.get()
        if lib:
            if not lib.opendata_commit():
                raise DatabaseError("Commit failed")

    def rollback(self):
        if self._closed:
            raise ProgrammingError("Cannot rollback on closed connection")
        lib = _OpendataLib.get()
        if lib:
            if not lib.opendata_rollback():
                raise DatabaseError("Rollback failed")

    def close(self):
        if not self._closed:
            lib = _OpendataLib.get()
            if lib:
                lib.opendata_close()
            self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type is not None:
            self.rollback()
        else:
            self.commit()
        self.close()


def connect(database: str = "", **kwargs) -> Connection:
    return Connection(database, **kwargs)
