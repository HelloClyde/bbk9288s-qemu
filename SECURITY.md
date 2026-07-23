# Security Policy

## Supported versions

Only the latest tagged release is supported.

## Reporting

Do not publish vulnerabilities that expose host files, allow unauthenticated
remote writes, or escape the emulator process before a fix is available.
Use GitHub private vulnerability reporting when it is enabled for the
repository.

The Web server is designed for a trusted LAN. Its HTTP, WebSocket, and NAND
file-management endpoints do not provide authentication or TLS. QMP is bound
to localhost and must remain inaccessible from other hosts.

Firmware and NAND images should be treated as untrusted input. Keep backups of
NAND images and run the emulator as an unprivileged user.
