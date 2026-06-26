#!/usr/bin/env python3
"""Fake ds4-agent: persistent stdin mode with flushed WAITING markers."""
import argparse
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--non-interactive", action="store_true")
    parser.add_argument("--chdir")
    parser.add_argument("-m", "--model")
    parser.add_argument("-c", "--ctx")
    parser.add_argument("--trace")
    parser.add_argument("--nothink", action="store_true")
    parser.add_argument("--think", action="store_true")
    parser.add_argument("--think-max", action="store_true")
    parser.add_argument("-p", "--prompt")
    args = parser.parse_args()

    if args.prompt:
        print(f"fixture-ok: {args.prompt}", flush=True)
        return

    print("+DWARFSTAR_WAITING", flush=True)
    for line in sys.stdin:
        text = line.strip()
        if not text:
            continue
        print(f"fixture-ok: {text}", flush=True)
        print("+DWARFSTAR_WAITING", flush=True)


if __name__ == "__main__":
    main()
