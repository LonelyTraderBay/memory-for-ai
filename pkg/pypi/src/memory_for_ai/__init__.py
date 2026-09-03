"""
memory-for-ai — Fast code intelligence engine for AI coding agents.
Downloads and runs the memory-for-ai binary from GitHub Releases.
"""

try:
    from importlib.metadata import version, PackageNotFoundError
    try:
        __version__ = version("memory-for-ai")
    except PackageNotFoundError:
        __version__ = "unknown"
except ImportError:
    __version__ = "unknown"

from ._cli import main

__all__ = ["main", "__version__"]
