# Only update-alternatives from this recipe is used (at image build time); the bash-based
# opkg-* scripts are never installed on the device, so don't drag bash into every build.
RDEPENDS:${PN}:remove = "bash"
# ...which makes those scripts' /bin/bash shebang unresolved; fine for a package we never install.
INSANE_SKIP:${PN} += "file-rdeps"
