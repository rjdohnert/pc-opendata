/**
 * Node.js driver for PC/OpenData SQL Server.
 * Supports persistent high-performance TCP socket connections
 * and secure local embedded CLI fallback. Zero external dependencies.
 */

const net = require('net');
const tls = require('tls');
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

class DatabaseError extends Error {
    constructor(message, query) {
        super(message);
        this.name = 'DatabaseError';
        this.query = query;
    }
}

class Client {
    constructor(options = {}) {
        this.database = options.database || '';
        this.host = options.host || '127.0.0.1';
        this.port = Number(options.port) || 7350;
        this.user = options.user || options.username || '';
        this.password = options.password || '';
        this.tls = Boolean(options.tls || options.ssl);
        this.binPath = options.binPath || this._findBinary();
        
        // Use network mode if host/port is explicitly set or database file is not given
        this.useNetwork = Boolean(options.network || (!this.database && this.port));
        
        this._socket = null;
        this._authenticated = false;
        this._connected = false;
        this._connecting = null;
        this._closed = false;
        this._queue = [];
        this._rxBuffer = '';
    }

    _findBinary() {
        const candidates = [
            path.resolve(__dirname, '..', '..', 'bin', 'opendata.exe'),
            path.resolve(__dirname, '..', '..', 'bin', 'opendata'),
            'opendata.exe',
            'opendata'
        ];
        for (const p of candidates) {
            if (fs.existsSync(p)) return p;
        }
        return 'opendata';
    }

    /**
     * Connect to the PC/OpenData SQL Server daemon via TCP/TLS socket.
     */
    async connect() {
        if (!this.useNetwork) return;
        if (this._connected) return;
        if (this._connecting) return this._connecting;

        this._connecting = new Promise((resolve, reject) => {
            const socketFactory = this.tls ? tls.connect : net.connect;
            const sockOpts = this.tls 
                ? { host: this.host, port: this.port, rejectUnauthorized: false }
                : { host: this.host, port: this.port };

            const sock = socketFactory(sockOpts, () => {
                this._socket = sock;
                this._connected = true;

                // Handle initial greeting and setup
                const initHandler = (chunk) => {
                    this._rxBuffer += chunk.toString();
                    if (this._rxBuffer.includes('> ') || this._rxBuffer.includes('AUTH') || this._rxBuffer.includes('SQL')) {
                        sock.removeListener('data', initHandler);
                        sock.on('data', this._onSocketData.bind(this));
                        
                        this._performHandshake()
                            .then(() => resolve())
                            .catch(reject);
                    }
                };

                sock.on('data', initHandler);
            });

            sock.on('error', (err) => {
                if (!this._connected) {
                    this._connecting = null;
                    reject(new DatabaseError(`Failed to connect to ${this.host}:${this.port} - ${err.message}`));
                } else {
                    this._handleSocketError(err);
                }
            });

            sock.on('close', () => {
                this._connected = false;
                this._authenticated = false;
                this._socket = null;
            });
        });

        return this._connecting;
    }

    async _performHandshake() {
        // Authenticate if user provided
        if (this.user) {
            const authResp = await this._rawSend(`AUTH ${this.user} ${this.password}\n`);
            if (authResp.toLowerCase().includes('failed') || authResp.toLowerCase().includes('error')) {
                throw new DatabaseError(`Authentication failed for user '${this.user}': ${authResp.trim()}`);
            }
            this._authenticated = true;
        }
        // Set session output format to JSON for structured parsing
        await this._rawSend('SET FORMAT JSON\n');
    }

    _onSocketData(chunk) {
        this._rxBuffer += chunk.toString();
        this._processQueue();
    }

    _processQueue() {
        if (this._queue.length === 0) return;
        const current = this._queue[0];

        // Check if we received prompt or completion terminator
        const promptIdx = this._rxBuffer.indexOf('\n> ');
        const endPromptIdx = this._rxBuffer.endsWith('> ') ? this._rxBuffer.length - 2 : -1;
        const targetIdx = promptIdx !== -1 ? promptIdx : endPromptIdx;

        if (targetIdx !== -1) {
            const rawResponse = this._rxBuffer.substring(0, targetIdx);
            this._rxBuffer = this._rxBuffer.substring(targetIdx + 2).replace(/^[\r\n]+/, '');
            this._queue.shift();
            current.resolve(rawResponse);
            this._processQueue();
        }
    }

    _rawSend(data) {
        return new Promise((resolve, reject) => {
            if (!this._socket || this._socket.destroyed) {
                return reject(new DatabaseError('Socket is not connected'));
            }
            this._queue.push({ resolve, reject });
            this._socket.write(data);
        });
    }

    _formatSql(sql, params) {
        if (!params || params.length === 0) return sql;
        let pIdx = 0;
        return sql.replace(/\?/g, () => {
            if (pIdx >= params.length) return '?';
            const v = params[pIdx++];
            if (v === null || v === undefined) return 'NULL';
            if (typeof v === 'number') {
                if (!Number.isFinite(v)) return 'NULL';
                return String(v);
            }
            if (typeof v === 'boolean') return v ? '1' : '0';
            if (Array.isArray(v)) {
                return '[' + v.map(n => Number.isFinite(n) ? n : 0).join(', ') + ']';
            }
            const sanitized = String(v).replace(/\0/g, '').replace(/'/g, "''");
            return "'" + sanitized + "'";
        });
    }

    async query(sql, params = []) {
        if (this._closed) {
            throw new DatabaseError('Cannot execute query on closed client');
        }

        const formattedSql = this._formatSql(sql, params);

        if (this.useNetwork) {
            if (!this._connected) {
                await this.connect();
            }
            return this._queryNetwork(formattedSql);
        } else {
            return this._queryCli(formattedSql);
        }
    }

    async _queryNetwork(formattedSql) {
        // Send SQL terminated by semicolon and newline
        const cleanSql = formattedSql.trim().replace(/;+$/, '');
        const rawOutput = await this._rawSend(`${cleanSql};\n`);
        const trimmed = rawOutput.trim();

        if (trimmed.startsWith('ERROR:') || trimmed.toLowerCase().includes('syntax error')) {
            throw new DatabaseError(trimmed, formattedSql);
        }

        let rows = [];
        let fields = [];

        // Check if output is a JSON array
        const jsonStart = trimmed.indexOf('[');
        const jsonEnd = trimmed.lastIndexOf(']');
        if (jsonStart !== -1 && jsonEnd !== -1 && jsonEnd > jsonStart) {
            try {
                const jsonText = trimmed.substring(jsonStart, jsonEnd + 1);
                rows = JSON.parse(jsonText);
                if (rows.length > 0) {
                    fields = Object.keys(rows[0]).map(name => ({ name }));
                }
            } catch (e) {
                // Parsing fallback
            }
        }

        return {
            rows,
            fields,
            rowCount: rows.length,
            command: formattedSql.split(/\s+/)[0].toUpperCase(),
            raw: rawOutput
        };
    }

    async _queryCli(formattedSql) {
        const args = ['--format', 'json'];
        if (this.database) {
            args.push('--database', this.database);
        }
        if (this.user) {
            args.push('-u', this.user);
        }
        if (this.password) {
            // Note: In CLI fallback mode, prefer database path without passing sensitive credentials in argv if not required
            args.push('-p', this.password);
        }
        if (this.port && !this.database) {
            args.push('--port', String(this.port));
        }
        args.push('-e', formattedSql);

        return new Promise((resolve, reject) => {
            const proc = spawn(this.binPath, args);
            let stdout = '';
            let stderr = '';

            proc.stdout.on('data', chunk => { stdout += chunk.toString(); });
            proc.stderr.on('data', chunk => { stderr += chunk.toString(); });

            proc.on('close', code => {
                if (code !== 0) {
                    const errMsg = (stderr || stdout).trim() || `Exit code ${code}`;
                    return reject(new DatabaseError(errMsg, formattedSql));
                }

                const trimmed = stdout.trim();
                let rows = [];
                let fields = [];

                if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
                    try {
                        rows = JSON.parse(trimmed);
                        if (rows.length > 0) {
                            fields = Object.keys(rows[0]).map(name => ({ name }));
                        }
                    } catch (e) {
                        // Not JSON
                    }
                }

                resolve({
                    rows,
                    fields,
                    rowCount: rows.length,
                    command: formattedSql.split(/\s+/)[0].toUpperCase(),
                    raw: stdout
                });
            });

            proc.on('error', err => {
                reject(new DatabaseError(`Failed to spawn opendata binary: ${err.message}`, formattedSql));
            });
        });
    }

    _handleSocketError(err) {
        while (this._queue.length > 0) {
            const item = this._queue.shift();
            item.reject(new DatabaseError(`Socket communication error: ${err.message}`));
        }
    }

    async close() {
        this._closed = true;
        if (this._socket) {
            this._socket.end();
            this._socket.destroy();
            this._socket = null;
        }
        this._connected = false;
        this._authenticated = false;
    }
}

function connect(options = {}) {
    return new Client(options);
}

module.exports = {
    connect,
    Client,
    DatabaseError
};
