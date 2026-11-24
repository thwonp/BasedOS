# What the card's programs need that the system layer lacks (tools/needs-check.py).
#   needs.sh <card root: a spruce checkout or card>   (BASEDOS_WORK as tools/wsl/build.sh)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
W=${BASEDOS_WORK:-$HOME/basedos-work}
python3 "$REPO/tools/needs-check.py" "$W/rootfs" "${1:?usage: needs.sh <card root>}" "$W/stock-root"
