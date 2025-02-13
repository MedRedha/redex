#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
import argparse
import io
import logging
import os
import re
import signal
import sys

from debug_line_map import DebugLineMap
from dexdump import DexdumpSymbolicator
from iodi import IODIMetadata
from line_unmap import PositionMap
from logcat import LogcatSymbolicator
from symbol_files import SymbolFiles


# A simple symbolicator for line-based input,
# i.e. a newline separated list of class names to be symbolicated.
class LinesSymbolicator(object):
    def __init__(self, symbol_maps):
        self.symbol_maps = symbol_maps

    def symbolicate(self, line):
        class_name = line[:-7]  # strip '.class' suffix.
        class_name = class_name.replace("/", ".")

        if class_name in self.symbol_maps.class_map:
            ans = self.symbol_maps.class_map[class_name]
            return ans + "\n"
        return line


class SymbolMaps(object):
    def __init__(self, symbol_files):
        self.class_map = self.get_class_map(symbol_files.extracted_symbols)
        self.line_map = PositionMap.read_from(symbol_files.line_map)
        self.debug_line_map = (
            DebugLineMap.read_from(symbol_files.debug_line_map)
            if os.path.exists(symbol_files.debug_line_map)
            else None
        )
        if os.path.exists(symbol_files.iodi_metadata):
            if not self.debug_line_map:
                logging.error(
                    "In order to symbolicate with IODI, redex-debug-line-map-v2 is required!"
                )
                sys.exit(1)
            self.iodi_metadata = IODIMetadata(symbol_files.iodi_metadata)
            logging.info(
                "Unpacked "
                + str(len(self.iodi_metadata.collision_free))
                + " methods from iodi metadata"
            )
        else:
            self.iodi_metadata = None

    @staticmethod
    def get_class_map(mapping_filename):
        mapping = {}
        with open(mapping_filename) as f:
            for line in f:
                if " -> " in line and line[0] != " ":
                    original, _sep, new = line.partition(" -> ")
                    new = new.strip()[:-1]
                    mapping[new] = original
        return mapping


def parse_args(args):
    """
    args is a list of (str, dict) tuples that should be passed to
    ArgumentParser.add_argument with the initial str as the flag
    """
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="""=== Android Symbolicator ===

Supports logcat and dexdump inputs when piped to stdin.

Examples of usage:

    adb logcat | ./symbolicator.py --artifacts /path/to/artifacts/

    dexdump -d secondary-1.dex | ./symbolicator.py --artifacts /artifacts/
""",
    )

    parser.add_argument(
        "--target",
        type=str,
        help="Buck target that's been built. This will be slow as it "
        "queries buck. Additionally it requires the cwd to be a subdir "
        "of a buck directory. Pass the artifact directory directly if "
        "you know it",
    )
    parser.add_argument(
        "--artifacts",
        type=str,
        help="e.g. ~/buckrepo/buck-out/gen/path/to/app/artifacts/",
    )
    parser.add_argument(
        "--input-type", type=str, choices=("logcat", "dexdump", "lines")
    )
    if args is not None:
        for flag, arg in args:
            parser.add_argument(flag, **arg)

    return parser.parse_args()


def main(arg_desc=None, symbol_file_generator=None):
    logging.basicConfig(level=logging.INFO)

    args = parse_args(arg_desc)

    if args.artifacts is not None:
        symbol_files = SymbolFiles.from_buck_artifact_dir(args.artifacts)
    elif args.target is not None:
        symbol_files = SymbolFiles.from_buck_target(args.target)
    elif symbol_file_generator is not None:
        symbol_files = symbol_file_generator(args)
    if symbol_files is None:
        logging.error(
            "Unable to find symbol files used to symbolicate input!"
            "Try passing --target or --artifacts."
        )
        sys.exit(1)

    symbol_maps = SymbolMaps(symbol_files)

    # I occasionally use ctrl-C to e.g. terminate a text search in `less`; I
    # don't want to kill this script in the process, though
    if not sys.stdout.isatty():
        signal.signal(signal.SIGINT, signal.SIG_IGN)

    reader = io.TextIOWrapper(
        sys.stdin.buffer, encoding="utf-8", errors="surrogateescape"
    )
    writer = io.TextIOWrapper(
        sys.stdout.buffer, encoding="utf-8", errors="surrogateescape"
    )

    first_line = ""
    while first_line == "":
        try:
            first_line = next(reader)
        except StopIteration:
            logging.warning("Empty input")
            sys.exit(0)

    if args.input_type == "lines":
        symbolicator = LinesSymbolicator(symbol_maps)
    elif args.input_type == "logcat" or LogcatSymbolicator.is_likely_logcat(first_line):
        symbolicator = LogcatSymbolicator(symbol_maps)
    elif args.input_type == "dexdump" or DexdumpSymbolicator.is_likely_dexdump(
        first_line
    ):
        symbolicator = DexdumpSymbolicator(symbol_maps)
    else:
        logging.warning("Could not figure out input kind, assuming logcat")
        symbolicator = LogcatSymbolicator(symbol_maps)

    logging.info("Using %s", type(symbolicator).__name__)

    def output(s):
        if s is not None:
            writer.write(s)

    output(symbolicator.symbolicate(first_line))
    for line in reader:
        output(symbolicator.symbolicate(line))


if __name__ == "__main__":
    main()
