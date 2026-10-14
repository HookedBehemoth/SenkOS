require mesa-main.inc
# Only the Mali-G31's arch (v7): all ten made libgallium 21.6 MB, slow to load from squashfs
EXTRA_OEMESON += "-Dpanfrost-archs=7"
