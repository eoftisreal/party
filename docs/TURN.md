# Connect devices on different networks

Same-Wi-Fi success proves only the local path. Different routers, carrier-grade
NAT and firewalls can prevent direct WebRTC connections. STUN discovers public
candidates; TURN supplies a relay when direct connectivity fails. Appwrite
coordinates rooms and signaling; it does not relay the movie.

## Direct connections by default

The backend returns `stun:stun.cloudflare.com:3478` when `PARTY_STUN_URLS` is
unset. This is Cloudflare's public STUN service: no account or TURN credentials
are needed. STUN exchanges small address-discovery packets; it never receives
the movie stream. The players' ICE implementation already checks direct paths,
including available IPv4/IPv6 host candidates and UDP hole punching. This change
supplies the missing default public-address discovery rather than replacing ICE.

To try the direct path:

1. Deploy this backend and update both players to versions that fetch `/ice`.
2. Leave `PARTY_STUN_URLS` unset. Leave `PARTY_TURN_URLS` and `PARTY_TURN_SECRET`
   unset if you want no relay configured. Unset local `PARTY_ICE_SERVERS` overrides
   and leave `PARTY_RELAY_ONLY` unset or `0`.
3. Start the host and viewer on different networks. No video upload or TURN
   subscription is required.
4. Inspect `network.connectionPath`: `direct` means neither selected candidate is
   relayed; `relay` means at least one is; `unselected` means no classified path yet.
   Check `connected` too, since a selected pair alone does not prove active playback.

`network.localCandidates.srflx` reports discovered server-reflexive candidates.
A positive count confirms public-mapping discovery, **not** that the other network
can reach that mapping. `iceGatheringComplete`, `stunServersConfigured`,
`turnServersConfigured` and `remoteTrickleCandidatesApplied` help distinguish
configuration, gathering and signaling issues without logging IPs or credentials.

Override `PARTY_STUN_URLS` with your own STUN URL if desired. Set it to `[]` (or an
empty string) to opt out of public discovery. An explicit local
`PARTY_ICE_SERVERS=[]` also keeps testing local and bypasses backend settings.

This removes the need to provision a relay for networks where direct ICE succeeds.
It cannot make every pair of restrictive NATs/firewalls accept direct traffic.
If both devices gather candidates and exchange them but cannot connect, use a
reachable network path or the optional TURN fallback below. Do not disable the
firewall as a workaround. With TURN configured, normal ICE can use direct and
relay candidates; `PARTY_RELAY_ONLY=1` is only for relay diagnostics.

## Optional relay fallback

Run coturn on an Internet-reachable Linux server, or use a service compatible with
coturn's shared-secret TURN REST credentials. The server must have a reachable
public address, sufficient video bandwidth, and UDP ports open for both its
listener and its relay port range. An Appwrite HTTP Function cannot host coturn.

Example coturn configuration (replace both placeholders):

```ini
listening-port=3478
fingerprint
realm=turn.example.com
use-auth-secret
static-auth-secret=REPLACE_WITH_A_RANDOM_SECRET_OF_AT_LEAST_32_CHARACTERS
min-port=49160
max-port=49260
no-tcp-relay
no-multicast-peers
no-cli
```

Allow inbound UDP 3478 and UDP 49160–49260 in the server firewall and cloud
security group. If the server is behind NAT, configure coturn's `external-ip`
mapping and forward those ports. Use coturn's peer IP deny rules to prevent
relaying to private infrastructure; configure allocation and bandwidth quotas
for your deployment. Keep the shared secret out of Git and player builds.

Set these environment variables on the **Party Appwrite Function**:

```dotenv
PARTY_STUN_URLS=["stun:turn.example.com:3478"]
PARTY_TURN_URLS=["turn:turn.example.com:3478?transport=udp"]
PARTY_TURN_SECRET=<same-secret-as-coturn>
PARTY_TURN_TTL_SECONDS=3600
```

Redeploy the backend and rebuild/update the native player from the same revision.
Unset `PARTY_ICE_SERVERS` on both devices to enable automatic configuration. A
nonempty value overrides backend settings, including the explicit value `[]`.

The bundled libjuice build accepts one STUN and two UDP TURN URLs. TCP/TLS TURN
URLs are rejected explicitly: networks that block UDP require a transport build
with TCP/TLS TURN support, which this change does not add. TURN relays traffic;
it does not inherently change the encoded colors or resolution. Throughput and
latency still depend on the relay and both Internet connections.

## Player integration contract

After creating/joining a room, request:

```http
GET /v1/rooms/{roomId}/ice
Authorization: Bearer <guest-token-or-Appwrite-JWT>
x-party-session: <current-session-id>
```

The response is marked `Cache-Control: no-store` and contains `iceServers`
(entries with `url`, plus `username`/`credential` for TURN), `ttlSeconds`,
`expiresAt` (Unix seconds), and `turnConfigured`. It never contains the shared
secret. Default configuration returns the public STUN entry and `turnConfigured:false`;
explicitly disabling STUN with no TURN configured returns an empty list;
invalid configuration returns 503 `ICE_CONFIGURATION_ERROR`. Membership, room
status, session replacement and lease checks apply before and after issuance.

The native HTTP worker fetches settings before publishing room authority and
incoming offers, refreshes them one minute before TTL expiry for subsequent peer
connections, and refetches after session recovery. HTTPS work stays off the
media/render thread. Existing peers keep their configuration; a recreated peer
uses the refreshed settings. Credential TTL should cover your expected session
length (300–86400 seconds supported). Other Windows players embedding this repo
must implement the same request before creating their own peer connection.

## Verify using two networks

1. Host on Wi-Fi; join from mobile data or a different home network.
2. Check startup output: `TURN relay: configured`. If it says `not configured`, confirm
   the deployed Function variables and unset any local ICE override.
3. Set `PARTY_RELAY_ONLY=1` on both devices, restart, then verify playback. This
   forces the relay path and prevents a direct connection masking a broken relay.
4. Remove relay-only mode to allow direct connections when available.
5. If forced relay fails, check coturn allocation/auth logs, DNS, the shared
   secret, server clock, UDP listener and relay firewall range. Do not paste
   credentials into diagnostics.

A passing unit test or local loopback test cannot prove this Internet relay path.
It needs a running TURN server and the real two-network test above.

References: [Cloudflare public STUN](https://developers.cloudflare.com/realtime/turn/faq/),
[WebRTC TURN](https://webrtc.org/getting-started/turn-server),
[coturn authentication](https://github.com/coturn/coturn/blob/master/README.turnserver).
