# Security policy

Use GitHub private vulnerability reporting when available. Otherwise contact the repository owner privately before opening a public issue.

Never include passwords, cookies, tickets, private certificate authorities, Wi-Fi credentials, flash dumps, or private Hermes conversation data in a report.

Security fixes target the current `main` branch until versioned releases are published.

- Use HTTPS/WSS for remote dashboards.
- Treat plain LAN HTTP as an explicit, temporary exception.
- Prefer a dedicated least-privilege Hermes account.
- Keep configuration, certificates, recordings, and backups outside Git.
- Restrict the local admin panel to a trusted network.
- Rotate any credential exposed in logs, screenshots, issues, or commits.
