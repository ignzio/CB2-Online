# Code signing policy

Free code signing for the Windows programs of CB2-Online (`Crash2.exe`, `Crash2 Server.exe`) is provided by [SignPath.io](https://signpath.io), with a certificate from the [SignPath Foundation](https://signpath.org).

**Status:** the application to the SignPath Foundation is pending. Until it is approved and the signing pipeline is running, releases are not signed (see the Windows Defender notes in the FAQ).

## Roles

| Role | Who |
| --- | --- |
| Committers and reviewers | The contributors of [ignzio/CB2-Online](https://github.com/ignzio/CB2-Online/graphs/contributors); changes by others are merged only after review by the maintainer |
| Approver | Ignazio Gandolfo ([@ignzio](https://github.com/ignzio)) |

## What is signed

Only binaries built from the public source in this repository, by the project's GitHub Actions workflow, from a tagged release. Nothing else is signed: no third-party programs, no game data. The SHA-256 of each signed file is what the game's updater checks.

## Privacy

See the [privacy policy](PRIVACY.md). The programs transfer information only to the online services described there, and only when you use the online lobby.

## Reporting a problem

A signed file that behaves suspiciously: open an [issue](https://github.com/ignzio/CB2-Online/issues) or contact the maintainer.
