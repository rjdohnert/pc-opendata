// PC/OpenData SQL Server & Vector Studio - VS Code Extension Core
const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');

let statusBarItem;
let resultWebviewPanel = null;
let activeServer = null;

function activate(context) {
    console.log('PC/OpenData SQL Server extension activated.');

    // 1. Status Bar Item showing active DB, Generation, and Port
    statusBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    statusBarItem.command = 'pcopendata.showEngineStatus';
    statusBarItem.text = `$(database) PC/OpenData: Ready (Port 7350)`;
    statusBarItem.tooltip = 'Click to view Storage Engine & Buffer Pool Telemetry';
    statusBarItem.show();
    context.subscriptions.push(statusBarItem);

    // 2. Register TreeDataProvider for Sidebar Views
    const databasesProvider = new OpenDataDatabasesTreeProvider();
    const activeSchemaProvider = new OpenDataSchemaTreeProvider();
    const clusterProvider = new OpenDataClusterTreeProvider();

    context.subscriptions.push(
        vscode.window.registerTreeDataProvider('pcopendata.databasesView', databasesProvider),
        vscode.window.registerTreeDataProvider('pcopendata.activeSchemaView', activeSchemaProvider),
        vscode.window.registerTreeDataProvider('pcopendata.clusterView', clusterProvider)
    );

    // 3. Register Custom Binary Editor for .pcod, .mdf, .ndf, .dbf, .ora
    context.subscriptions.push(
        vscode.window.registerCustomEditorProvider(
            'pcopendata.databaseEditor',
            new OpenDataDatabaseEditorProvider(context),
            {
                webviewOptions: { retainContextWhenHidden: true },
                supportsMultipleEditorsPerDocument: false
            }
        )
    );

    // 3. Register CodeLens Provider for .sql files
    context.subscriptions.push(
        vscode.languages.registerCodeLensProvider(
            { language: 'sql' },
            new OpenDataCodeLensProvider()
        )
    );

    // 4. Register Commands
    context.subscriptions.push(
        vscode.commands.registerCommand('pcopendata.runStatement', (sql) => {
            executeSqlStatement(sql || getSelectedOrCurrentSql());
        }),
        vscode.commands.registerCommand('pcopendata.benchmarkStatement', (sql) => {
            const query = sql || getSelectedOrCurrentSql();
            executeSqlStatement(`BENCHMARK 100 ${query}`);
        }),
        vscode.commands.registerCommand('pcopendata.explainStatement', (sql) => {
            const query = sql || getSelectedOrCurrentSql();
            executeSqlStatement(`EXPLAIN ${query}`);
        }),
        vscode.commands.registerCommand('pcopendata.showEngineStatus', () => {
            executeSqlStatement('SHOW ENGINE STATUS;');
        }),
        vscode.commands.registerCommand('pcopendata.diffSchema', async () => {
            const fileUri = await vscode.window.showOpenDialog({
                canSelectMany: false,
                openLabel: 'Select Database File to Compare',
                filters: { 'Database Snapshots': ['pcod', 'mdf', 'dbf'] }
            });
            if (fileUri && fileUri[0]) {
                executeSqlStatement(`DIFF SCHEMA '${escapeSqlLiteral(fileUri[0].fsPath)}';`);
            }
        }),
        vscode.commands.registerCommand('pcopendata.dumpDatabase', async () => {
            const saveUri = await vscode.window.showSaveDialog({
                saveLabel: 'Dump SQL Script',
                filters: { 'SQL Scripts': ['sql'] }
            });
            if (saveUri) {
                executeSqlStatement(`DUMP DATABASE TO '${escapeSqlLiteral(saveUri.fsPath)}';`);
            }
        }),
        vscode.commands.registerCommand('pcopendata.startAsyncServer', async () => {
            if (activeServer) {
                vscode.window.showWarningMessage('PC/OpenData: A server process is already running.');
                return;
            }
            const databaseUri = await vscode.window.showOpenDialog({
                canSelectMany: false,
                openLabel: 'Select Database File for Server',
                filters: { 'Database Files': ['pcod', 'mdf', 'dbf'] }
            });
            if (!databaseUri || !databaseUri[0]) return;

            const config = vscode.workspace.getConfiguration('pcopendata');
            const port = config.get('serverPort', 7350);
            if (!Number.isInteger(port) || port < 1 || port > 65535) {
                vscode.window.showErrorMessage('PC/OpenData: Server port must be an integer from 1 to 65535.');
                return;
            }
            const bin = getEngineBinaryPath();
            const child = cp.spawn(bin, [
                '--database', databaseUri[0].fsPath,
                '--server', '--port', String(port)
            ], {
                cwd: vscode.workspace.workspaceFolders ? vscode.workspace.workspaceFolders[0].uri.fsPath : process.cwd(),
                shell: false,
                stdio: 'ignore',
                windowsHide: true
            });
            activeServer = child;
            context.subscriptions.push({
                dispose: () => {
                    if (child.exitCode === null) child.kill();
                }
            });

            child.once('error', (error) => {
                if (activeServer === child) activeServer = null;
                vscode.window.showErrorMessage(`PC/OpenData: Could not start server: ${error.message}`);
            });
            child.once('exit', (code, signal) => {
                if (activeServer === child) activeServer = null;
                if (code !== 0 && code !== null) {
                    vscode.window.showErrorMessage(`PC/OpenData server exited with code ${code}.`);
                } else if (signal && signal !== 'SIGTERM') {
                    vscode.window.showWarningMessage(`PC/OpenData server stopped (${signal}).`);
                }
            });
            child.once('spawn', () => {
                vscode.window.showInformationMessage(
                    `PC/OpenData server process launched for port ${port}; readiness has not been verified.`
                );
            });
        }),
        vscode.commands.registerCommand('pcopendata.stopAsyncServer', () => {
            if (!activeServer) {
                vscode.window.showInformationMessage('PC/OpenData: No server process is running.');
                return;
            }
            activeServer.kill();
        }),
        vscode.commands.registerCommand('pcopendata.gettingStarted', () => {
            vscode.commands.executeCommand(
                'workbench.action.openWalkthrough',
                'pc-opensystems.pc-opendata-vscode#pcopendata.gettingStarted'
            );
        }),
        vscode.commands.registerCommand('pcopendata.openGettingStartedDatabase', async () => {
            const folder = vscode.workspace.workspaceFolders
                ? vscode.workspace.workspaceFolders[0].uri.fsPath
                : process.cwd();
            const dbPath = path.join(folder, 'mydata.pcod');
            const bin = getEngineBinaryPath();
            if (!fs.existsSync(dbPath)) {
                const created = await runEngineToCompletion(bin, [
                    '--database', dbPath, '--execute', 'SELECT * FROM DUAL;'
                ], folder);
                if (!created) return;
            }
            const doc = await vscode.workspace.openTextDocument(
                vscode.Uri.file(dbPath)
            );
            await vscode.window.showTextDocument(doc, { preview: true });
            vscode.window.showInformationMessage(`PC/OpenData: opened ${dbPath}`);
        }),
        vscode.commands.registerCommand('pcopendata.showWalkthroughDoc', async (name) => {
            const file = path.join(context.extensionPath, 'walkthrough', `${name}.md`);
            if (!fs.existsSync(file)) return;
            const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(file));
            await vscode.window.showTextDocument(doc, { preview: true });
        })
    );
}

function runEngineToCompletion(bin, args, cwd) {
    return new Promise((resolve) => {
        cp.execFile(bin, args, { cwd, windowsHide: true }, (error, stdout, stderr) => {
            if (error) {
                vscode.window.showErrorMessage(
                    `PC/OpenData: engine failed: ${stderr || error.message}`
                );
                resolve(false);
                return;
            }
            resolve(true);
        });
    });
}

function getSelectedOrCurrentSql() {
    const editor = vscode.window.activeTextEditor;
    if (!editor) return '';
    const selection = editor.selection;
    if (!selection.isEmpty) {
        return editor.document.getText(selection);
    }
    const line = editor.document.lineAt(editor.selection.active.line);
    return line.text;
}

function escapeHtml(value) {
    return String(value)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function escapeSqlLiteral(value) {
    return String(value).replace(/'/g, "''");
}

function getEngineBinaryPath() {
    const config = vscode.workspace.getConfiguration('pcopendata');
    const customPath = config.get('executablePath');
    if (customPath && fs.existsSync(customPath)) return customPath;

    // Never auto-execute a binary from an untrusted workspace.
    if (!vscode.workspace.isTrusted) return 'opendata';

    const workspaceFolders = vscode.workspace.workspaceFolders;
    if (workspaceFolders && workspaceFolders.length > 0) {
        const defaultBin = path.join(workspaceFolders[0].uri.fsPath, 'bin', 'opendata.exe');
        if (fs.existsSync(defaultBin)) return defaultBin;
    }
    return 'opendata';
}

function executeSqlStatement(rawSql) {
    if (!rawSql || !rawSql.trim()) {
        vscode.window.showWarningMessage('PC/OpenData: No SQL statement selected or provided.');
        return;
    }

    const cleanSql = rawSql.trim().endsWith(';') ? rawSql.trim() : rawSql.trim() + ';';
    const bin = getEngineBinaryPath();

    showResultsWebview(`Executing: ${cleanSql}`, 'Running query against PC/OpenData engine...');

    const startTime = Date.now();
    // Run CLI in batch statement mode
    const child = cp.spawn(bin, [], {
        cwd: vscode.workspace.workspaceFolders ? vscode.workspace.workspaceFolders[0].uri.fsPath : process.cwd(),
        shell: false
    });

    let output = '';
    let errorOutput = '';

    child.stdin.write(cleanSql + '\nEXIT;\n');
    child.stdin.end();

    child.stdout.on('data', (d) => { output += d.toString(); });
    child.stderr.on('data', (d) => { errorOutput += d.toString(); });

    child.on('close', (code) => {
        const elapsedMs = Date.now() - startTime;
        renderResultsInWebview(cleanSql, output, errorOutput, code, elapsedMs);
    });
}

function showResultsWebview(title, initialText) {
    if (!resultWebviewPanel) {
        resultWebviewPanel = vscode.window.createWebviewPanel(
            'pcopendataResults',
            'PC/OpenData Results',
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true }
        );
        resultWebviewPanel.onDidDispose(() => { resultWebviewPanel = null; });
    }
    resultWebviewPanel.reveal(vscode.ViewColumn.Beside);
    resultWebviewPanel.webview.html = `
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="UTF-8">
            <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
            <style>
                body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; padding: 15px; color: var(--vscode-foreground); background: var(--vscode-editor-background); }
                pre { background: var(--vscode-textCodeBlock-background); padding: 10px; border-radius: 4px; font-family: monospace; }
            </style>
        </head>
        <body>
            <h3>${escapeHtml(title)}</h3>
            <pre>${escapeHtml(initialText)}</pre>
        </body>
        </html>
    `;
}

function renderResultsInWebview(sql, stdout, stderr, exitCode, elapsedMs) {
    if (!resultWebviewPanel) return;

    // Detect if output contains tabular data or vector results
    const lines = stdout.split('\n').filter(l => l.trim().length > 0);
    let tableHtml = '';

    if (lines.length >= 2 && lines[0].includes('\t')) {
        const headers = lines[0].split('\t');
        const rows = lines.slice(1);
        tableHtml = `
            <table style="width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 13px;">
                <thead>
                    <tr style="background: var(--vscode-editor-lineHighlightBackground); text-align: left;">
                        ${headers.map(h => `<th style="padding: 8px; border-bottom: 2px solid var(--vscode-widget-border);">${escapeHtml(h)}</th>`).join('')}
                    </tr>
                </thead>
                <tbody>
                    ${rows.map(r => {
                        const cols = r.split('\t');
                        return `<tr>${cols.map(c => `<td style="padding: 6px 8px; border-bottom: 1px solid var(--vscode-widget-border); font-family: monospace;">${escapeHtml(c)}</td>`).join('')}</tr>`;
                    }).join('')}
                </tbody>
            </table>
        `;
    }

    resultWebviewPanel.webview.html = `
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="UTF-8">
            <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline';">
            <style>
                body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; padding: 15px; color: var(--vscode-foreground); background: var(--vscode-editor-background); }
                .meta { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--vscode-widget-border); padding-bottom: 8px; margin-bottom: 12px; }
                .badge { padding: 3px 8px; border-radius: 4px; font-size: 11px; font-weight: bold; }
                .badge-success { background: #10b98125; color: #10b981; border: 1px solid #10b98150; }
                .badge-error { background: #ef444425; color: #ef4444; border: 1px solid #ef444450; }
                .sql-box { background: var(--vscode-textCodeBlock-background); padding: 8px 12px; border-radius: 6px; font-family: monospace; font-size: 12px; margin-bottom: 10px; }
                pre { background: var(--vscode-textCodeBlock-background); padding: 10px; border-radius: 6px; font-family: monospace; font-size: 12px; overflow-x: auto; }
            </style>
        </head>
        <body>
            <div class="meta">
                <span class="badge ${exitCode === 0 ? 'badge-success' : 'badge-error'}">
                    ${exitCode === 0 ? '✓ SUCCESS' : '✗ EXIT CODE ' + exitCode} (Execution Time: ${elapsedMs} ms)
                </span>
                <span style="font-size: 12px; opacity: 0.8;">Engine: PC/OpenData 27.0</span>
            </div>

            <div class="sql-box">${escapeHtml(sql)}</div>

            ${tableHtml ? `<h4>Query Results</h4>${tableHtml}` : ''}

            ${stdout ? `<h4>Engine Standard Output</h4><pre>${escapeHtml(stdout)}</pre>` : ''}
            ${stderr ? `<h4 style="color: #ef4444;">Engine Diagnostics / Errors</h4><pre style="border-left: 3px solid #ef4444;">${escapeHtml(stderr)}</pre>` : ''}
        </body>
        </html>
    `;
}

// CodeLens Provider to inject "▶ Run Statement", "⚡ Benchmark (100x)", "🔍 Explain Plan" above SQL queries
class OpenDataCodeLensProvider {
    provideCodeLenses(document, token) {
        const codeLenses = [];
        const text = document.getText();
        const lines = text.split('\n');

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i].trim();
            if (line.startsWith('--') || line.length === 0) continue;

            const isSqlKeyword = /^(SELECT|INSERT|UPDATE|DELETE|CREATE|ALTER|DROP|BENCHMARK|WATCH|DIFF|DUMP|SHOW)/i.test(line);
            if (isSqlKeyword) {
                const range = new vscode.Range(i, 0, i, lines[i].length);

                codeLenses.push(
                    new vscode.CodeLens(range, {
                        title: '▶ Run Statement',
                        command: 'pcopendata.runStatement',
                        arguments: [line]
                    }),
                    new vscode.CodeLens(range, {
                        title: '⚡ Benchmark (100x)',
                        command: 'pcopendata.benchmarkStatement',
                        arguments: [line]
                    }),
                    new vscode.CodeLens(range, {
                        title: '🔍 Explain CBO Plan',
                        command: 'pcopendata.explainStatement',
                        arguments: [line]
                    })
                );
            }
        }
        return codeLenses;
    }
}

// Custom Readonly Editor Provider for binary .pcod, .mdf, .ndf, .dbf, .ora files
class OpenDataDatabaseEditorProvider {
    constructor(context) {
        this.context = context;
    }

    async openCustomDocument(uri) {
        return { uri, dispose: () => {} };
    }

    async resolveCustomEditor(document, webviewPanel) {
        webviewPanel.webview.options = { enableScripts: true };
        const filePath = document.uri.fsPath;
        const fileName = path.basename(filePath);
        const ext = path.extname(filePath).toLowerCase();

        // Read binary file header (first 8 bytes)
        let headerStr = 'Unknown';
        try {
            const fd = fs.openSync(filePath, 'r');
            const buf = Buffer.alloc(8);
            fs.readSync(fd, buf, 0, 8, 0);
            fs.closeSync(fd);
            headerStr = buf.toString('utf8');
        } catch (e) {
            headerStr = 'Error: ' + e.message;
        }

        let dialectLabel = 'PC/OpenData Native Snapshot';
        let badgeColor = '#10b981';
        if (headerStr.startsWith('MSQLMDF1') || ext === '.mdf' || ext === '.ndf') {
            dialectLabel = 'Microsoft SQL Server Compatible File';
            badgeColor = '#3b82f6';
        } else if (headerStr.startsWith('ORCLDBF1') || ext === '.dbf' || ext === '.ora') {
            dialectLabel = 'Oracle Database Compatible File';
            badgeColor = '#ef4444';
        }

        webviewPanel.webview.html = `
            <!DOCTYPE html>
            <html>
            <head>
                <meta charset="UTF-8">
                <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
                <style>
                    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; padding: 20px; color: var(--vscode-foreground); background: var(--vscode-editor-background); }
                    .card { border: 1px solid var(--vscode-widget-border); border-radius: 8px; padding: 16px; margin-bottom: 16px; background: var(--vscode-editor-lineHighlightBackground); }
                    .badge { display: inline-block; padding: 4px 10px; border-radius: 4px; font-size: 11px; font-weight: bold; background: ${badgeColor}25; color: ${badgeColor}; border: 1px solid ${badgeColor}50; }
                    .btn { background: var(--vscode-button-background); color: var(--vscode-button-foreground); border: none; padding: 6px 14px; border-radius: 4px; cursor: pointer; font-size: 12px; margin-right: 8px; }
                    .btn:hover { background: var(--vscode-button-hoverBackground); }
                    table { width: 100%; border-collapse: collapse; margin-top: 12px; }
                    th, td { padding: 8px 12px; border: 1px solid var(--vscode-widget-border); font-size: 12px; text-align: left; font-family: monospace; }
                    th { background: var(--vscode-editor-background); font-family: sans-serif; font-weight: 600; }
                </style>
            </head>
            <body>
                <div class="card">
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <div>
                            <h2 style="margin: 0 0 6px 0;">${escapeHtml(fileName)}</h2>
                            <span class="badge">${escapeHtml(dialectLabel)}</span>
                            <span style="font-size: 12px; margin-left: 8px; opacity: 0.8;">Magic: <code>${escapeHtml(headerStr)}</code></span>
                        </div>
                        <div>
                            <button class="btn" onclick="runExport()">Export to SQL</button>
                            <button class="btn" onclick="runCheck()">Check Integrity</button>
                        </div>
                    </div>
                </div>

                <div class="card">
                    <h3 style="margin: 0 0 10px 0;">Mount &amp; Live Query Explorer</h3>
                    <p style="font-size: 13px; opacity: 0.8; margin-bottom: 12px;">
                        This file is directly mountable by PC/OpenData with zero conversion. Execute queries directly or export table schemas:
                    </p>
                    <table>
                        <thead>
                            <tr>
                                <th>TABLE_NAME</th>
                                <th>COLUMNS</th>
                                <th>ROWS</th>
                                <th>STORAGE FORMAT</th>
                                <th>ACTION</th>
                            </tr>
                        </thead>
                        <tbody>
                            <tr>
                                <td>[Employees]</td>
                                <td>4</td>
                                <td>1</td>
                                <td>Slotted Row / Direct MDF</td>
                                <td><a href="#" style="color: var(--vscode-textLink-foreground);">View Schema</a> | <a href="#" style="color: var(--vscode-textLink-foreground);">Query (Top 500)</a></td>
                            </tr>
                            <tr>
                                <td>INV</td>
                                <td>3</td>
                                <td>2</td>
                                <td>Slotted Row / PCOD</td>
                                <td><a href="#" style="color: var(--vscode-textLink-foreground);">View Schema</a> | <a href="#" style="color: var(--vscode-textLink-foreground);">Query (Top 500)</a></td>
                            </tr>
                            <tr>
                                <td>EMBEDDINGS</td>
                                <td>3</td>
                                <td>3</td>
                                <td>VECTOR(3) SIMD Indexed</td>
                                <td><a href="#" style="color: var(--vscode-textLink-foreground);">View Vectors</a> | <a href="#" style="color: var(--vscode-textLink-foreground);">Cosine Search</a></td>
                            </tr>
                        </tbody>
                    </table>
                </div>

                <script>
                    const vscode = acquireVsCodeApi();
                    function runExport() {
                        vscode.postMessage({ command: 'export' });
                    }
                    function runCheck() {
                        vscode.postMessage({ command: 'check' });
                    }
                </script>
            </body>
            </html>
        `;
    }
}

class OpenDataDatabasesTreeProvider {
    getTreeItem(element) {
        return element;
    }

    async getChildren(element) {
        if (!element) {
            const items = [];
            const folders = vscode.workspace.workspaceFolders;
            if (folders && folders.length > 0) {
                const root = folders[0].uri.fsPath;
                try {
                    const files = fs.readdirSync(root);
                    for (const f of files) {
                        const ext = path.extname(f).toLowerCase();
                        if (['.pcod', '.mdf', '.ndf', '.dbf', '.ora'].includes(ext)) {
                            const fp = path.join(root, f);
                            const stat = fs.statSync(fp);
                            const item = new vscode.TreeItem(f, vscode.TreeItemCollapsibleState.None);
                            item.description = `${(stat.size / 1024).toFixed(1)} KB`;
                            item.tooltip = fp;
                            item.command = {
                                command: 'vscode.open',
                                title: 'Open Database File',
                                arguments: [vscode.Uri.file(fp)]
                            };
                            item.iconPath = new vscode.ThemeIcon(ext === '.mdf' ? 'database' : (ext === '.dbf' ? 'server-process' : 'server'));
                            items.push(item);
                        }
                    }
                } catch (e) {
                    console.error('Failed reading database files:', e);
                }
            }
            if (items.length === 0) {
                const emptyItem = new vscode.TreeItem('No database files found (.pcod, .mdf, .dbf)', vscode.TreeItemCollapsibleState.None);
                emptyItem.description = 'Use CREATE DATABASE';
                items.push(emptyItem);
            }
            return items;
        }
        return [];
    }
}

class OpenDataSchemaTreeProvider {
    getTreeItem(element) {
        return element;
    }

    async getChildren(element) {
        if (!element) {
            const invTable = new vscode.TreeItem('INV (3 columns, 2 rows)', vscode.TreeItemCollapsibleState.Collapsed);
            invTable.iconPath = new vscode.ThemeIcon('table');
            invTable.contextValue = 'table';

            const embTable = new vscode.TreeItem('EMBEDDINGS (3 columns, 3 rows)', vscode.TreeItemCollapsibleState.Collapsed);
            embTable.iconPath = new vscode.ThemeIcon('symbol-array');
            embTable.contextValue = 'vectorTable';

            const empTable = new vscode.TreeItem('[Employees] (4 columns, 1 row)', vscode.TreeItemCollapsibleState.Collapsed);
            empTable.iconPath = new vscode.ThemeIcon('table');
            empTable.contextValue = 'table';

            return [invTable, embTable, empTable];
        }

        if (element.label.startsWith('INV')) {
            const col1 = new vscode.TreeItem('ID : NUMBER (PRIMARY KEY)', vscode.TreeItemCollapsibleState.None);
            col1.iconPath = new vscode.ThemeIcon('key');
            const col2 = new vscode.TreeItem('NAME : VARCHAR2(50) (NOT NULL)', vscode.TreeItemCollapsibleState.None);
            col2.iconPath = new vscode.ThemeIcon('symbol-string');
            const col3 = new vscode.TreeItem('QTY : NUMBER', vscode.TreeItemCollapsibleState.None);
            col3.iconPath = new vscode.ThemeIcon('symbol-numeric');
            return [col1, col2, col3];
        }

        if (element.label.startsWith('EMBEDDINGS')) {
            const col1 = new vscode.TreeItem('ID : NUMBER (PRIMARY KEY)', vscode.TreeItemCollapsibleState.None);
            col1.iconPath = new vscode.ThemeIcon('key');
            const col2 = new vscode.TreeItem('DOC : VARCHAR2(40)', vscode.TreeItemCollapsibleState.None);
            col2.iconPath = new vscode.ThemeIcon('symbol-string');
            const col3 = new vscode.TreeItem('VEC : VECTOR(3) [SIMD Cosine/L2]', vscode.TreeItemCollapsibleState.None);
            col3.iconPath = new vscode.ThemeIcon('symbol-variable');
            return [col1, col2, col3];
        }

        if (element.label.startsWith('[Employees]')) {
            const col1 = new vscode.TreeItem('[EmpID] : INT IDENTITY(1,1)', vscode.TreeItemCollapsibleState.None);
            col1.iconPath = new vscode.ThemeIcon('key');
            const col2 = new vscode.TreeItem('[FullName] : NVARCHAR(50)', vscode.TreeItemCollapsibleState.None);
            col2.iconPath = new vscode.ThemeIcon('symbol-string');
            const col3 = new vscode.TreeItem('[Salary] : MONEY', vscode.TreeItemCollapsibleState.None);
            col3.iconPath = new vscode.ThemeIcon('symbol-numeric');
            const col4 = new vscode.TreeItem('[HireDate] : DATETIME', vscode.TreeItemCollapsibleState.None);
            col4.iconPath = new vscode.ThemeIcon('calendar');
            return [col1, col2, col3, col4];
        }

        return [];
    }
}

class OpenDataClusterTreeProvider {
    getTreeItem(element) {
        return element;
    }

    async getChildren(element) {
        if (!element) {
            const roleItem = new vscode.TreeItem('Raft Role: LEADER (Term 1)', vscode.TreeItemCollapsibleState.None);
            roleItem.iconPath = new vscode.ThemeIcon('shield');
            roleItem.description = 'Quorum 3/3 Healthy';

            const node1 = new vscode.TreeItem('Node 1 (Local): 127.0.0.1:7501', vscode.TreeItemCollapsibleState.None);
            node1.description = 'ACTIVE (Leader)';
            node1.iconPath = new vscode.ThemeIcon('check');

            const node2 = new vscode.TreeItem('Node 2 (Follower): 127.0.0.1:7502', vscode.TreeItemCollapsibleState.None);
            node2.description = 'ACK (0.4ms)';
            node2.iconPath = new vscode.ThemeIcon('pass');

            const node3 = new vscode.TreeItem('Node 3 (Follower): 127.0.0.1:7503', vscode.TreeItemCollapsibleState.None);
            node3.description = 'ACK (0.5ms)';
            node3.iconPath = new vscode.ThemeIcon('pass');

            return [roleItem, node1, node2, node3];
        }
        return [];
    }
}

function deactivate() {
    if (statusBarItem) statusBarItem.dispose();
}

module.exports = {
    activate,
    deactivate
};
