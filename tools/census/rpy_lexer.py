"""The census uses the compiler's lexer (rpy2psp/compiler/lexer.py)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from rpy2psp.compiler.lexer import (Line, block_source, build_tree, logical_lines, parse_file,  # noqa: E402,F401
                                    read_source, string_value, tokenize)
