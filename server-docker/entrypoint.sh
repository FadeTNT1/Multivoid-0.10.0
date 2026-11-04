#!/bin/bash
# server-docker/entrypoint.sh -- the container's entry. Under Pterodactyl the panel hands the
# startup line in STARTUP ({{VAR}} placeholders); standalone, the server starts directly.
set -e
cd /home/container || exit 1

if [ -n "${STARTUP}" ]; then
    # Pterodactyl's yolk convention: {{VAR}} becomes ${VAR}, then the line runs.
    MODIFIED_STARTUP=$(echo -e "${STARTUP}" | sed -e 's/{{/${/g' -e 's/}}/}/g')
    echo "container: ${MODIFIED_STARTUP}"
    eval exec ${MODIFIED_STARTUP}
fi
exec /usr/local/bin/multivoid-start
