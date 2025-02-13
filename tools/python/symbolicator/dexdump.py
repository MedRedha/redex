# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
import re
import sys


class DexdumpSymbolicator(object):

    CLASS_REGEX = re.compile(r"\bL(?P<class>[A-Za-z][0-9A-Za-z_$]*\/[0-9A-Za-z_$\/]+);")

    LINE_REGEX = re.compile(r"(?P<prefix>0x[0-9a-f]+ line=)(?P<lineno>\d+)")

    METHOD_CLS_HDR_REGEX = re.compile(
        r"#\d+\s+:\s+\(in L(?P<class>[A-Za-z][0-9A-Za-z]*\/[0-9A-Za-z_$\/]+);\)"
    )
    METHOD_REGEX = re.compile(r"name\s+:\s+\'(?P<method>[<A-Za-z][>A-Za-z0-9_$]*)\'")

    CLS_CHUNK_HDR_REGEX = re.compile(r"  [A-Z]")
    CLS_HDR_REGEX = re.compile(r"Class #")

    def __init__(self, symbol_maps):
        self.symbol_maps = symbol_maps
        self.reading_methods = False
        self.current_class = None
        self.current_method_id = None
        self.last_line = None

    def class_replacer(self, matchobj):
        m = matchobj.group("class")
        cls = m.replace("/", ".")
        if cls in self.symbol_maps.class_map:
            return "L%s;" % self.symbol_maps.class_map[cls].replace(".", "/")
        return "L%s;" % m

    def line_replacer(self, matchobj):
        lineno = int(matchobj.group("lineno"))
        positions = map(
            lambda p: "%s:%d" % (p.file, p.line),
            self.symbol_maps.line_map.get_stack(lineno - 1),
        )
        return matchobj.group("prefix") + ", ".join(positions)

    def reset_state(self):
        self.current_class = None
        self.current_method_id = None
        self.last_line = None

    def symbolicate(self, line):
        extra = None

        def p(s):
            nonlocal extra
            if extra is None:
                extra = s
            else:
                extra += ", " + s

        if self.symbol_maps.iodi_metadata is not None:
            match = self.METHOD_CLS_HDR_REGEX.search(line)
            if match is not None:
                self.current_class = match.group("class")
            elif self.current_class is not None:
                match = self.METHOD_REGEX.search(line)
                if match is not None:
                    current_method = match.group("method")
                    qualified_method = (
                        self.current_class.replace("/", ".") + "." + current_method
                    )
                    # We should try to symbolicate this method name, but that requires
                    # changing the rename map parser so that I'll leave that for a later
                    # patch
                    iodi_map = self.symbol_maps.iodi_metadata.collision_free
                    if qualified_method in iodi_map:
                        self.current_method_id = iodi_map[qualified_method]
                elif self.current_method_id is not None:
                    match = self.LINE_REGEX.search(line)
                    if match is not None:
                        mapped_line = self.symbol_maps.debug_line_map.find_line_number(
                            self.current_method_id, match.group("lineno")
                        )
                        if mapped_line:
                            if self.last_line is not None:
                                if self.last_line == mapped_line:
                                    # Don't emit duplicate line entries
                                    return None
                            self.last_line = mapped_line
                            positions = map(
                                lambda p: "%s:%d" % (p.file, p.line),
                                self.symbol_maps.line_map.get_stack(mapped_line - 1),
                            )
                            return (
                                "        "
                                + match.group("prefix")
                                + ", ".join(positions)
                                + "\n"
                            )

            if self.CLS_CHUNK_HDR_REGEX.match(line) is not None:
                # If we match a header but its the wrong header then ignore the
                # contents of this subsection until we hit a methods subsection
                self.reading_methods = (
                    "Direct methods" in line or "Virtual methods" in line
                )
                if not self.reading_methods:
                    self.reset_state()
            elif self.CLS_HDR_REGEX.match(line) is not None:
                self.reading_methods = False
                self.reset_state()

        line = self.CLASS_REGEX.sub(self.class_replacer, line)
        line = self.LINE_REGEX.sub(self.line_replacer, line)
        return line

    @staticmethod
    def is_likely_dexdump(line):
        return re.match(r"^Processing '.*\.dex'", line) or re.search(
            r"Class #\d+", line
        )
