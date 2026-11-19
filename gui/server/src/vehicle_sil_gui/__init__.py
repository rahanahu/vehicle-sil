"""vehicle-sil-gui: the browser GUI's server. It runs vehicle_sil_launch and relays its progress to the screen."""

# The vehicle-sil release of this server, written as vehicle_sil_launch writes it. A literal, not the package
# metadata: the tools archive puts the server in place without installing it, and the metadata has the normalized
# form of PEP 440 (rc1, not -rc.1). scripts/package.py version keeps it equal to CMakeLists.txt.
__version__ = "0.2.0-rc.1"
