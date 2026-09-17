#!/bin/sh
# Create the Cumulocity Digital Twin Manager property definitions for the
# parameter sets this Router App exposes (Metrics, Relay, Container).
#
# Run this ONCE PER TENANT, from a workstation -- not on the router. Without the
# property definitions the device happily accepts c8y_ParameterUpdate operations
# and publishes its twin values, but Device Management > Parameters has no form
# to render, so nothing is visible in the UI.
#
# Usage:
#   scripts/c8y-parameter-definitions.sh [create|delete|list] [--dry] [set ...]
#
#   create   create the definitions (default)
#   delete   remove them again
#   list     show the definitions currently in the tenant
#
#   set ...  limit the action to the named parameter sets
#            (default: Metrics Relay Container)
#   --dry    show the requests without sending them
#
# Flow parameter sets (flow_params_<mapper>_<flow>) are per-flow, so their
# schema has to mirror that flow's params.toml. There is no generic definition
# for them; create one per flow, e.g. for mappers/c8y/flows/relay-auto-open:
#
#   c8y api POST /service/dtm/definitions/properties -n --template '{
#     "identifier": "flow_params_c8y_relay-auto-open",
#     "jsonSchema": {
#       "title": "Relay auto-open flow",
#       "type": "object",
#       "properties": {
#         "delay_minutes":  { "type": "number", "title": "delay (minutes)", "order": 1 },
#         "relay_fragment": { "type": "string", "title": "operation fragment", "order": 2 }
#       }
#     },
#     "contexts": ["asset", "event", "operation"]
#   }'
#
# Requirements:
#   * go-c8y-cli (https://goc8ycli.netlify.app) with an active session
#     (`c8y sessions set`); everything goes through `c8y api`. An ADMIN session:
#     a device cannot define tenant schemas -- the device user is refused with
#     "does not have permission ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE"
#     (confirmed on device).
#   * The dtm and device-parameter microservices subscribed to the tenant (you
#     may need a Cumulocity support ticket for these).
#   * Your user needs ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE and
#     ROLE_DIGITAL_TWIN_DEFINITIONS_ADMIN, e.g.
#       c8y userroles addRoleToGroup --role ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE --group admins
#       c8y userroles addRoleToGroup --role ROLE_DIGITAL_TWIN_DEFINITIONS_ADMIN  --group admins
#
# The schemas below mirror the fields of the on-device parameter plugins
# (modules/tedge/merge/parameter-plugins/*). Keep them in sync: a field that is
# not in the schema cannot be set from the UI, and one the plugin ignores is
# silently dropped.
set -eu

ACTION=create
DRY=""
SETS=""

while [ $# -gt 0 ]; do
    case "$1" in
        create|delete|list) ACTION="$1" ;;
        --dry)              DRY="--dry" ;;
        -h|--help)          sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//;s/^#$//'; exit 0 ;;
        -*)                 echo "Unknown option: $1" >&2; exit 2 ;;
        *)                  SETS="${SETS:+$SETS }$1" ;;
    esac
    shift
done

[ -n "$SETS" ] || SETS="Metrics Relay Container"
CONTEXTS="asset,event,operation"

if ! type c8y >/dev/null 2>&1; then
    echo "ERROR: go-c8y-cli ('c8y') not found on PATH -- see https://goc8ycli.netlify.app" >&2
    exit 1
fi

if [ "$ACTION" = "list" ]; then
    c8y api GET "/service/dtm/definitions/properties?contexts=$CONTEXTS" -n --raw $DRY
    exit 0
fi

# schema_for <set> -- the DTM property definition body. Plain JSON, which is
# also valid jsonnet, so it can be handed to `c8y api --template`.
schema_for() {
    case "$1" in
    Metrics) cat <<'EOT'
{
  "identifier": "Metrics",
  "jsonSchema": {
    "title": "System metrics",
    "description": "Router metrics (CPU load, memory, disk, board temperature/voltage) published to Cumulocity as measurements.",
    "type": "object",
    "properties": {
      "enabled":  { "type": "boolean", "title": "enabled", "default": true, "order": 1 },
      "interval": { "type": "integer", "title": "interval (s)", "minimum": 5, "maximum": 86400, "default": 60, "order": 2 },
      "series":   { "type": "string",  "title": "series", "description": "Space-separated groups: cpu memory disk environment", "default": "cpu memory disk environment", "order": 3 },
      "type":     { "type": "string",  "title": "measurement type", "default": "resources", "order": 4 }
    }
  },
  "tags": ["thin-edge.io", "advantech-icr"],
  "contexts": ["asset", "event", "operation"]
}
EOT
        ;;
    Relay) cat <<'EOT'
{
  "identifier": "Relay",
  "jsonSchema": {
    "title": "Relay outputs",
    "description": "Digital output control: the c8y_Relay / c8y_RelayArray operations and the poller that reflects device-side changes back to the cloud.",
    "type": "object",
    "properties": {
      "enabled":      { "type": "boolean", "title": "enabled", "default": true, "order": 1 },
      "pollInterval": { "type": "integer", "title": "poll interval (s)", "minimum": 1, "maximum": 3600, "default": 5, "order": 2 },
      "outputs":      { "type": "string",  "title": "outputs", "description": "Ordered, space-separated ICR-OS io output names, e.g. out0 out1. Defines the size of the RelayArray widget.", "default": "out0", "order": 3 },
      "activeLow":    { "type": "boolean", "title": "active low", "description": "On Advantech ICR the output is active-low relative to the relay contact; leave enabled unless the hardware differs.", "default": true, "order": 4 }
    }
  },
  "tags": ["thin-edge.io", "advantech-icr"],
  "contexts": ["asset", "event", "operation"]
}
EOT
        ;;
    Container) cat <<'EOT'
{
  "identifier": "Container",
  "jsonSchema": {
    "title": "Container monitoring",
    "description": "The tedge-container monitoring daemon, which reports container status and telemetry as Cumulocity services. Requires the Advantech Docker Router App. Software management for containers works regardless of this flag.",
    "type": "object",
    "properties": {
      "enabled": { "type": "boolean", "title": "enabled", "default": true, "order": 1 }
    }
  },
  "tags": ["thin-edge.io", "advantech-icr"],
  "contexts": ["asset", "event", "operation"]
}
EOT
        ;;
    *) return 1 ;;
    esac
}

rc=0
for s in $SETS; do
    body=$(schema_for "$s" 2>/dev/null) || {
        echo "SKIP   $s -- no schema in this script (add one, or check the spelling)" >&2
        rc=1
        continue
    }

    case "$ACTION" in
        create)
            echo "CREATE $s"
            # --template takes the body as jsonnet (plain JSON qualifies); -n so
            # c8y does not wait on stdin. A definition that already exists comes
            # back as an error from the dtm microservice -- delete it first if
            # you are changing a schema, since PUT is not supported for all
            # fields.
            c8y api POST /service/dtm/definitions/properties \
                --template "$body" -n --raw $DRY || rc=1
            ;;
        delete)
            echo "DELETE $s"
            c8y api DELETE "/service/dtm/definitions/properties/$s?contexts=$CONTEXTS" \
                -n --raw $DRY || rc=1
            ;;
    esac
done

echo
echo "Device side: the router publishes its current parameter values at start"
echo "(/opt/tedge/bin/parameter-seed). To republish them now, run on the router:"
echo "  /opt/tedge/etc/init reload parameters"
exit $rc
