#!/usr/bin/python3

# Convert (Json) file to hex values, similar as 'hexdump' without lines.
#
# License: Revised BSD License, see LICENSE.TXT file including in the project
#

import os
import sys


def dump(
    file_from: str,
    var_name: str,
    file_to: str | None = None,
):
    """
    Reads a file and dumps it to bytes frame. Can write to STDOUT
    or to a `file_to`.

    Args:
        file_from: Filename to read from
        file_to: None (STDOUT) or file to write
        var_name: The variable name to associate with the bytes frame
    """
    result = (
        f"// dump from file {os.path.basename(file_from)}\n"
        f"static const char {var_name}[] = {{\n    "
    )

    # read raw bytes so UTF-8 text (accents, symbols) is embedded correctly
    with open(file_from, "rb") as f:
        chars = f.read()

    for i, x in enumerate(chars, 1):
        result += f" 0x{x:02X},"
        if i % 16 == 0:
            result += "\n    "
    result += " 0x00\n};"

    if file_to is None:
        print(result)
    else:
        with open(file_to, "w") as f:
            f.writelines(result)


if __name__ == "__main__":
    dump(
        file_from=sys.argv[1],
        var_name=sys.argv[2],
    )
