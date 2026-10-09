# Mali-G31 subgroup fix on a Mesa main snapshot, see mesa-main.inc
require mesa-main.inc
# Mali-G31 is v7 (Bifrost): only its copy of the per-arch code and shaders. All ten made libgallium
# 21.6 MB, and loading it from squashfs was most of Weston's GL startup.
EXTRA_OEMESON += "-Dpanfrost-archs=7"
