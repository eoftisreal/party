# Party — watch-party networking

Host-to-viewer **live streaming** for a custom C++ video player. The movie stays on the host PC; Appwrite coordinates rooms and future WebRTC signaling, not movie transport.

## Development sections

| Section | State | Scope |
| --- | --- | --- |
| 1. Room API | Implemented in source; local tests passed | Private rooms, verified Appwrite JWT, invite join, leave, close, expiry |
| 2. WebRTC signaling | Not started | Authenticated offers/answers/ICE, peer authorization |
| 3. C++ media transport | Not started | Peer connections, STUN/TURN, RTP, reconnect |
| 4. FFmpeg stream sender | Not started | Live video/audio encode and host viewer synchronization |
| 5. Player integration | Not started | Watch-party controls, late join, recovery, multi-viewer |

See [Section 1 deployment and API](docs/SECTION_1.md).

### Local tests

```sh
cd backend
npm install
npm test
npm run check
```

**Verification:** Initial room API implementation passed 11 local tests before the GitHub push. The repository version contains a reduced 5-test suite. Remote npm dependencies could not be fetched in this environment, and the Appwrite live deployment has **not** been tested. The SDK and live environment must be verified before relying on the service.
