# Dev Tools
This directory contains various useful tools for development.

`test_crucible_client.sh` launches the installed Crucible 7.31 client with
Proton 7 and WineD3D using a separate compatibility prefix. It checks that
`start.ini` points at `192.168.1.158:26000` before launch and refuses a mismatch.
Use `--dry-run` to inspect the selected paths without starting the client. The
script does not accept the EULA; that must be done by the account owner.
