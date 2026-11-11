"""vehicle-sil-gui: the browser GUI's server. It runs vehicle_sil_launch and relays its progress to the screen."""

from importlib.metadata import PackageNotFoundError, version

try:
    __version__ = version("vehicle-sil-gui")
except PackageNotFoundError:  # run from the source tree without being installed
    __version__ = "0.1.0"
