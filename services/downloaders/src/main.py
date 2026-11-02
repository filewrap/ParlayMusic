"""Parlay downloaders service — entry point.

Usage:
    PORT=8010 PARLAY_TOKEN=... PARLAY_DL_DIR=./downloads \
        .venv/bin/python -m src.main
    # or: .venv/bin/python src/main.py  (run from services/downloaders/)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from src.server import Config, serve  # noqa: E402

if __name__ == "__main__":
    serve(Config())
