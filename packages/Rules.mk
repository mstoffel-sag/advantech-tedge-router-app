# 3rd-party package compatibility matrix (see modules/Rules.mk for details).
#
# The `tedge` package fetches the matching thin-edge.io *standalone* release
# binaries for the target architecture and repackages them for the Router App.
tedge = v2i v3 v4 v4i

# The `tedge-container` package fetches the matching tedge-container-plugin
# binary and lays it into the tedge platform tree (folded into the tedge module,
# not a standalone Router App).
tedge-container = v2i v3 v4 v4i

# The `jq` package fetches the matching statically-linked jq binary and lays it
# into the tedge platform tree (bin/jq). Required by the device-parameter
# feature, which parses the JSON payload of c8y_ParameterUpdate operations.
jq = v2i v3 v4 v4i
