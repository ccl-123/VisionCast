#!/usr/bin/env bash
set -euo pipefail

CARD="${1:-1}"

echo "Main mic mixer setup is board-image dependent."
echo "Inspect controls with: amixer -c ${CARD} contents"
echo "Then update this script with the confirmed NAU8822 capture path."
