# Lobby service protocol

The game's lobby list (`src/directory.*`) talks to a small HTTP service that lists open dedicated servers and player rooms and carries the
hand-shake that lets two players behind routers reach each other. **The service itself is not part of this repository**: the project's
maintainer runs a private one, and its address is built into the maintainer's executable (`local/directory.txt`, see `CMakeLists.txt`).
A build without that file has no lobby service and only finds players on the same PC / network and through a relay server you run
(`src/server.cpp`). Anyone can run their own service that speaks the protocol below (a Cloudflare Worker, a small web app, anything that
answers HTTP) and put its address in their own `local/directory.txt`.

The service only ever carries the list and the introductions, never game data: that is UDP between the players (`docs/ONLINE.md`).

## Protocol (plain text, so the game needs no JSON parser)

All answers are `text/plain`, one record per line, fields separated by a tab. `ip:port` is an IPv4 address and UDP port.

```
POST /announce  {"kind":"member","room","uid","name","port","ip","version","region","locked","rendezvous"}   every ~5 s while a player is in a room
                -> OK, then  J<TAB>ip:port  for every player who asked to join and has not been answered yet
POST /announce  {"kind":"server","name","port","players","version"}                        every 15 s from a dedicated server
POST /join      {"room","port","ip","version","region"}                                    a player who wants into a room
                -> OK, then  E<TAB>ip:port  for every member of the room
GET  /rooms?version=3&region=p
                -> R<TAB>room<TAB>players<TAB>ip:port,ip:port<TAB>version<TAB>rendezvous<TAB>locked    one per room (locked: 0 or 1)
                   S<TAB>name<TAB>players<TAB>ip:port<TAB>version                            one per dedicated server
GET  /health    -> OK
```

- `region` is `u` (the American release) or `p` (the European one; anything else counts as `u`). A room is identified by its region and name: the two releases cannot play together, so a service
  keeps them apart (`/join` answers with the members of the joiner's region, `/rooms` lists the asker's region). Dedicated servers serve both regions and are listed to everybody.
- `locked` is `true` when the room asks for a password. It is only a hint for the list: the password never reaches the service.
- `ip` in a request is the sender's public IPv4 address as STUN saw it; a service should prefer the address it observes on the connection
  and only use the reported one when the request came over IPv6. A service must only accept public addresses.
- Entries are soft state: forget a member or server about 45 s after its last announcement, a pending joiner after 30 s. A member is identified by its room (and region) and its address, so a player who leaves and comes back replaces
  its old entry instead of being counted twice; `POST /leave {"room","port","ip","region"}` removes the caller's own membership at once (only the caller's own address can be removed).
- Order servers by how long they have been announcing (not by the player count they claim), limit entries per caller (an IPv6 /64 is one caller) and rate-limit per caller.
- Validate everything (names are 1-15 of `A-Za-z0-9 _.-`, ports 1024-65535), strip control characters from anything echoed back,
  rate-limit per address, and cap the number of entries.

## The hand-shake

1. The game asks a STUN server (`stun.cloudflare.com:3478`) for its public UDP address and port.
2. Members of a room announce that address; the lobby list shows the room with its members' addresses.
3. A player picking the room tells the service their own address (`/join`) and gets the members' addresses back. The members learn about
   the joiner from their next announcement.
4. Both sides send packets to each other's address at the same time. That opens the path through both routers (NAT hole punching), after
   which the normal direct connection takes over. If a relay server is configured it relays pairs that cannot connect directly.
