# Direct-only network connections

TURN is intentionally disabled, not an optional fallback. No media relay or SFU is introduced. Backend and native ICE configuration reject TURN and relay-only mode. Remove old PARTY_TURN_URLS, PARTY_TURN_SECRET and PARTY_RELAY_ONLY=1 settings before migration.

Start/Join responses include one STUN discovery server by default. The Function can override PARTY_STUN_URLS server-side. STUN discovers a public address; it never carries movie media. Authenticated Function calls deliver SDP/trickled ICE. Media flows directly between participants over UDP/DTLS/SRTP.

Some CGNAT, symmetric NAT and restrictive firewall combinations cannot establish a direct path. Neither Appwrite nor more polling can fix those cases. Allow application UDP traffic and test two networks. Report failed connections honestly; the room remains available during its reconnect window. Never claim universal cross-network support.

Inspect per-viewer ICE state, candidate counts and connectionPath statistics. Account for each viewer's host upload bandwidth. See [capabilities](CAPABILITIES.md) and [streaming tuning](STREAMING_TUNING.md).
