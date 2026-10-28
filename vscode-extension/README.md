# PC/OpenData SQL Server & Vector Studio - VS Code Extension

<p align="center">
  <img src="resources/Designer.png" width="128" height="128" alt="PC/OpenData Logo" style="border-radius: 16px;" />
</p>

Official Visual Studio Code extension for **PC/OpenData SQL Server 27.0**.

## Features

- **Direct Binary File Inspector**: Click on any `.pcod`, `.mdf`, `.ndf`, `.dbf`, or `.ora` file in the VS Code Explorer to open a custom graphical table and schema viewer.
- **SQL & Vector CodeLens**: Get inline `▶ Run Statement`, `⚡ Benchmark (100x)`, and `🔍 Explain CBO Plan` buttons right above queries in your `.sql` files.
- **SIMD Vector Distance Visualizer**: Live cosine distance (`<=>`) and Euclidean distance (`<->`) projection with visual similarity score bars.
- **Cross-Engine Support**: Seamlessly work with Microsoft SQL Server files (`.mdf`), Oracle Database files (`.dbf`), and native PC/OpenData files (`.pcod`).
- **Storage Engine & Buffer Pool Telemetry**: Live status bar monitor with 64-frame clock-sweep buffer pool statistics and quorum status.

## Installation for Development

1. Copy or symlink the `vscode-extension` folder into your VS Code extensions directory:
   - Windows: `%USERPROFILE%\.vscode\extensions\pc-opendata-vscode`
   - Linux: `~/.vscode/extensions/pc-opendata-vscode`
2. Restart VS Code or run **Developer: Reload Window** (`Ctrl+Shift+P`).
