# Account-free sessions

The former separate guest-token bootstrap and optional Appwrite account flows have been removed. See [capability protocol and migration](CAPABILITIES.md). Start and Join each obtain a room credential in the same request as creation/join. The client configures only PARTY_FUNCTION_URL.

Credentials remain in memory. Network outages retain membership during the 30-minute host reconnect window; restarting the application is a new identity. Explicit host End/normal shutdown closes the room when reachable. Offline shutdown/crashes rely on expiry. No account, login screen, JWT refresh or public Realtime subscription is required or supported.
