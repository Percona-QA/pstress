#!/bin/python

"""
mysql_option_tester.py

This Python script automates testing MySQL startup options. It reads a text file
containing MySQL command-line options and runs each option set one by one.

For every option line, it creates a temporary data directory, starts a MySQL
server with the given options, waits for it to come online, and then shuts it down.
Errors from the MySQL error log are captured and written to an output file alongside
the tested options, allowing quick identification of invalid or problematic configurations.
"""

import os
import argparse
from mysql_utils import *

def filter_log_file(input_file: str, output_fh):
    """
    Reads a log file, filters out lines containing [Warning] or [System],
    and writes the remaining lines to the provided open file handle.

    :param input_file: Path to the input log file.
    :param output_fh: Open file handle to write filtered lines.
    """
    with open(input_file, "r") as fh:
        for line in fh:
            if "[Warning]" not in line and "[System]" not in line:
                output_fh.write(line)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Start MySQL with RocksDB, run SQL file, stop MySQL.")
    parser.add_argument("--basedir", required=True, help="MySQL base directory")
    parser.add_argument("--datadir", required=True, help="Path to MySQL data directory")
    parser.add_argument("--port", type=int, default=3307, help="Port for MySQL (default: 3307)")
    parser.add_argument("--host", default="127.0.0.1", help="MySQL base directory")
    parser.add_argument("--input-file", required=True, help="Path to .txt file with mysql options")

    args = parser.parse_args()

    base_name = os.path.splitext(os.path.abspath(args.input_file))[0]
    temp_datadir = args.datadir + "_temp"
    init_log = f"{base_name}.log"
    err_log = f"{base_name}.log"
    output_file = f"{base_name}.out"

    mysqld_path = find_mysqld(args.basedir)
    init_datadir(mysqld_path, args.basedir, args.datadir, init_log)

    with open(args.input_file, "r") as infile, open(output_file, "w") as outfile:
        for line in infile:
            commands = line.strip()
            if not commands:
                continue  # skip blank lines

            outfile.write(commands + "\n")
            copy_datadir(args.datadir, temp_datadir)
            proc = start_mysqld(mysqld_path, args.basedir, temp_datadir, args.port, err_log, commands)
            try:
                wait_for_mysql(args.basedir, args.port)
            finally:
                stop_mysqld(proc)
                filter_log_file(err_log, outfile)
                outfile.flush()
                os.remove(err_log)
