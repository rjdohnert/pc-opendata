PC/OpenData SQL Server Enterprise - Windows Setup
=========================================================

Included Files:
- opendata.exe         : SQL Server engine, interactive shell, and built-in self-test suite
- install.bat          : Double-click to install system-wide (requires Administrator)
- install.ps1          : PowerShell installation script (auto-elevates via UAC)
- install-elevate.ps1  : Helper script for UAC elevation

Installation Instructions:
1. Right-click 'install.bat' and select "Run as administrator" (or run 'install.bat' / 'install.ps1').
2. The installer automatically installs:
     opendata.exe
   to:
     C:\Program Files\OpenDataSQL\bin\
3. It configures the Machine/System PATH so the command is available system-wide for all users.
   (Strictly NO local user install).
4. Open a new Command Prompt or PowerShell and verify:
     opendata -v
     opendata self-test
