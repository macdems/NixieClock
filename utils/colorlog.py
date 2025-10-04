"""Shared ANSI color logging formatter for tooling scripts."""
import logging

class LevelColorFormatter(logging.Formatter):
    COLORS = {
        logging.DEBUG: "\033[36m",     # Cyan
        logging.INFO: "\033[32m",      # Green
        logging.WARNING: "\033[33m",   # Yellow
        logging.ERROR: "\033[31m",     # Red
        logging.CRITICAL: "\033[41m\033[97m",  # White on red background
    }
    RESET = "\033[0m"

    def __init__(self, fmt: str, datefmt: str | None = None, use_color: bool = True):
        super().__init__(fmt=fmt, datefmt=datefmt)
        self.use_color = use_color

    def format(self, record: logging.LogRecord) -> str:
        base = super().format(record)
        if self.use_color:
            color = self.COLORS.get(record.levelno)
            if color:
                return f"{color}{base}{self.RESET}"
        return base

__all__ = ["LevelColorFormatter"]
