# Privacy policy

CB2-Online is a free, open-source fan project. It has no accounts, no analytics, no advertising and no tracking. This page says exactly what leaves your computer, and when.

Maintainer and contact: Ignazio Gandolfo, via [GitHub issues](https://github.com/ignzio/CB2-Online/issues) or the e-mail on the maintainer's GitHub profile.

## The game (Crash2.exe)

**Offline play collects and sends nothing.** Your saves, records, settings (`cb2.cfg`) and logs (`crash.log`, `cb2-session.log`, ...) stay in the game folder. Nothing is uploaded automatically, ever.

When you use the **online lobby** (you can turn it off with `lobby = off` or `--no-lobby`):

| Sent to | What | Why | How long it is kept |
| --- | --- | --- | --- |
| The lobby service (a Cloudflare Worker run by the maintainer) | The room name, your player name, game version, release (PAL or NTSC-U), whether the room has a password (never the password itself, only a one-way hash of a key derived from it), your port and your IP address (the one the service sees, and the one learned from STUN) | To list rooms and to tell the other players of a room where to connect | About 45 seconds after your last announcement; held in the service's memory only, not written to a database or to logs by the project, not sold or shared |
| `stun.cloudflare.com` | A standard STUN request | To learn your public address, so players can connect through routers | Not kept by the project |
| The other players of your room | Your IP address, your player name, your gameplay (position, animation, level) | Peer-to-peer play: this is how a multiplayer game works | Not stored |
| A relay server (`Crash2 Server.exe`), only when two players cannot connect directly | The encrypted game packets | To forward them | Not stored |

Other players can therefore see your IP address, as in any peer-to-peer game. If that is a problem, play offline or use a VPN.

**Updates:** the game asks `api.github.com` for the latest release and downloads it from `github.com`. GitHub receives the request like for any download (see GitHub's privacy statement). The downloaded file is checked against a SHA-256 hash.

**Crash reports:** the files stay on your computer. If you choose to *open a bug report*, your browser opens a prefilled GitHub issue (version, system, graphics card, settings, last events). Nothing is sent until you press *Submit new issue*, and you can read and edit the text first. The crash files contain the game folder path (and so possibly your Windows user name), the list of loaded programs and your graphics card: look at them before you attach them to a public issue.

## The website (ignzio.github.io/CB2-Online)

It is a static page on GitHub Pages. It loads the latest release information from the GitHub API and a small `sponsors.json` file from the same site. It sets no cookies and has no analytics. GitHub, as host, may log visits according to its own privacy statement. The *Sponsor* button is an embedded GitHub page; GitHub handles any data when you use it.

## Your rights

There is no personal profile to access or delete: the lobby service forgets your entry after about 45 seconds. For anything else, contact the maintainer.

## Changes

Changes to this policy appear here, in the repository history.

## Code signing

Windows releases of CB2-Online are, once the application is approved, signed with a certificate from the SignPath Foundation, using SignPath.io's service (see [CODE_SIGNING.md](CODE_SIGNING.md)). Signing happens in the build pipeline; no data about players is involved.
