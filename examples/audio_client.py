#!/usr/bin/env python3
"""Example: audio_client.py — CLI client wrapper."""
from ws_audiod import *

if __name__ == "__main__":
    # Delegate to the library's built-in CLI
    import ws_audiod
    import runpy
    runpy.run_module("ws_audiod", run_name="__main__")
