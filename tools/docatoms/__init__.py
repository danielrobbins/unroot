"""Build-time compiler for Docatoms documentation sources."""

from .core import Atom, Document, parse
from .render import render_man, render_markdown

__all__ = ["Atom", "Document", "parse", "render_man", "render_markdown"]
