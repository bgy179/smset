#!/usr/bin/env python
"""IPv4 list compressor.

Library usage:
    from ipcompress import compress_ipv4_list

    lines = compress_ipv4_list([
        "10.1.2.9", "10.1.2.1", "10.1.2.3", "10.1.2.4", "10.1.2.5"
    ])
    # ["10.1.2.1+3-5+9"]
"""

from __future__ import print_function

import argparse
import re
import sys


def parse_ipv4_strict(ip_text):
    parts = ip_text.strip().split(".")
    if len(parts) != 4:
        raise ValueError("invalid IPv4: %s" % ip_text)

    octets = []
    for part in parts:
        if not part.isdigit():
            raise ValueError("invalid IPv4: %s" % ip_text)
        value = int(part)
        if value < 0 or value > 255:
            raise ValueError("invalid IPv4: %s" % ip_text)
        octets.append(value)
    return tuple(octets)


def split_ip_tokens(raw_text):
    return [token for token in re.split(r"[\s,;]+", raw_text.strip()) if token]


def group_ipv4_by_prefix(ip_values):
    grouped = {}
    for ip in ip_values:
        octets = parse_ipv4_strict(ip)
        prefix = octets[:3]
        last = octets[3]
        grouped.setdefault(prefix, set()).add(last)
    return grouped


def compress_octet_values(octets):
    if not octets:
        return ""

    values = sorted(set(octets))
    ranges = []
    start = values[0]
    prev = values[0]

    for value in values[1:]:
        if value == prev + 1:
            prev = value
            continue
        ranges.append((start, prev))
        start = value
        prev = value
    ranges.append((start, prev))

    parts = []
    for begin, end in ranges:
        if begin == end:
            parts.append(str(begin))
        elif end == begin + 1:
            parts.append("%d+%d" % (begin, end))
        else:
            parts.append("%d-%d" % (begin, end))
    return "+".join(parts)


def compress_ipv4_list(ip_values):
    grouped = group_ipv4_by_prefix(ip_values)
    lines = []
    for prefix in sorted(grouped.keys()):
        suffix = compress_octet_values(grouped[prefix])
        lines.append("%d.%d.%d.%s" % (prefix[0], prefix[1], prefix[2], suffix))
    return lines


def _parse_octet_value(text):
    if not text.isdigit():
        raise ValueError("invalid octet token: %s" % text)
    value = int(text)
    if value < 0 or value > 255:
        raise ValueError("octet out of range: %s" % text)
    return value


def uncompress_octet_expression(expression):
    if not expression:
        raise ValueError("empty octet expression")

    values = []
    for token in expression.split("+"):
        token = token.strip()
        if not token:
            raise ValueError("invalid octet expression: %s" % expression)

        if "-" in token:
            parts = token.split("-")
            if len(parts) != 2:
                raise ValueError("invalid range token: %s" % token)
            start = _parse_octet_value(parts[0])
            end = _parse_octet_value(parts[1])
            if end < start:
                raise ValueError("invalid descending range: %s" % token)
            values.extend(range(start, end + 1))
        else:
            values.append(_parse_octet_value(token))

    return sorted(set(values))


def uncompress_ipv4_line(compressed_ip):
    text = compressed_ip.strip()
    if not text:
        raise ValueError("empty compressed IPv4 line")

    parts = text.split(".")
    if len(parts) != 4:
        raise ValueError("invalid compressed IPv4: %s" % compressed_ip)

    a = _parse_octet_value(parts[0])
    b = _parse_octet_value(parts[1])
    c = _parse_octet_value(parts[2])
    suffix_values = uncompress_octet_expression(parts[3])

    return ["%d.%d.%d.%d" % (a, b, c, d) for d in suffix_values]


def uncompress_ipv4_list(compressed_lines):
    ips = []
    for line in compressed_lines:
        ips.extend(uncompress_ipv4_line(line))
    return ips


def read_ip_inputs(cli_ips, input_file):
    values = []
    if cli_ips:
        values.extend(cli_ips)

    if input_file:
        if input_file == "-":
            text = sys.stdin.read()
        else:
            with open(input_file, "r") as f:
                text = f.read()
        values.extend(split_ip_tokens(text))

    if not values:
        text = sys.stdin.read()
        values.extend(split_ip_tokens(text))
    return values


def build_parser():
    parser = argparse.ArgumentParser(
        description="Compress or uncompress IPv4 list notation (x+y+n-m+z) per /24"
    )
    parser.add_argument("ips", nargs="*", help="IPv4 values provided directly (space-separated)")
    parser.add_argument(
        "--uncompress",
        action="store_true",
        help="Interpret inputs as compressed entries and expand to full IPv4 addresses",
    )
    parser.add_argument(
        "--input-file",
        default=None,
        help="Read IPv4 values from file ('-' means stdin). Separators: whitespace/comma/semicolon",
    )
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)

    ip_values = read_ip_inputs(args.ips, args.input_file)
    if not ip_values:
        print("No IPv4 addresses provided.", file=sys.stderr)
        return 2

    try:
        if args.uncompress:
            lines = uncompress_ipv4_list(ip_values)
        else:
            lines = compress_ipv4_list(ip_values)
    except ValueError as ex:
        print(str(ex), file=sys.stderr)
        return 2

    for line in lines:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
