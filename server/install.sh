#!/usr/bin/env bash
# Installs the Crash 2 rendezvous server on a Linux machine (tested layout: Ubuntu on an Oracle Cloud VM).
#   scp -r server src/server.cpp src/netproto.hpp ubuntu@YOUR_VM:~/cb2/   (see docs/ONLINE.md for the exact layout)
#   ssh ubuntu@YOUR_VM 'cd ~/cb2 && sudo bash server/install.sh'
# Expects, relative to the parent of this folder:  src/server.cpp  src/netproto.hpp  server/cb2-server.service
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${PORT:-7777}"
# DIRECTORY=https://your-lobby.workers.dev lists the server in the game's server list (the same address the game was built with); NAME="My server" is its title there.
DIRECTORY="${DIRECTORY:-}"
NAME="${NAME:-Crash 2 server}"
ANNOUNCE="--no-announce"
[ -n "$DIRECTORY" ] && ANNOUNCE="--announce ${DIRECTORY} --name \"${NAME}\""

command -v g++ >/dev/null || { apt-get update -y && apt-get install -y g++; }
g++ -O2 -std=c++17 -Wall src/server.cpp -o /usr/local/bin/cb2_server
id cb2 >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin cb2
sed -e "s/@PORT@/${PORT}/" -e "s|@ANNOUNCE@|${ANNOUNCE}|" server/cb2-server.service > /etc/systemd/system/cb2-server.service
systemctl daemon-reload
systemctl enable cb2-server
systemctl restart cb2-server

# Ubuntu images on Oracle Cloud ship an iptables rule set that rejects everything not listed: allow the UDP port.
if command -v iptables >/dev/null && ! iptables -C INPUT -p udp --dport "${PORT}" -j ACCEPT 2>/dev/null; then
    # inserted near the top: a plain append would land behind the final REJECT rule and never be reached
    iptables -I INPUT 1 -p udp --dport "${PORT}" -j ACCEPT
    command -v netfilter-persistent >/dev/null && netfilter-persistent save || true
fi

echo
echo "Installed. Status:  systemctl status cb2-server      Log:  journalctl -u cb2-server -f"
echo "Still to do in the Oracle console: VCN > Security List (or Network Security Group) > add an INGRESS rule:"
echo "    Source CIDR 0.0.0.0/0, protocol UDP, destination port ${PORT}"
if [ -n "$DIRECTORY" ]; then echo "The server lists itself in $DIRECTORY: it shows up in the game's server list within a minute once the port above is open."; fi
echo "Otherwise on each player's PC set   rendezvous = <the VM's public IP>   in cb2.cfg (or type it in the lobby)."
