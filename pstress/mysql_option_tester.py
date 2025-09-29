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

import re
import os
import copy
import subprocess
import time
import shutil
import shlex
import argparse
import threading
import mysql.connector
from mysql.connector import Error

def find_mysqld(basedir):
    debug_path = os.path.join(basedir, "bin", "mysqld-debug")
    normal_path = os.path.join(basedir, "bin", "mysqld")
    if os.path.isfile(debug_path) and os.access(debug_path, os.X_OK):
        print(f"[INFO] Using mysqld-debug: {debug_path}")
        return debug_path
    elif os.path.isfile(normal_path) and os.access(normal_path, os.X_OK):
        print(f"[INFO] Using mysqld: {normal_path}")
        return normal_path
    else:
        raise FileNotFoundError(f"Neither mysqld-debug nor mysqld found in {basedir}/bin")

def init_datadir(mysqld_path, basedir, data_dir, log_file):
    if os.path.exists(data_dir):
        print("[INFO] Removing old datadir...")
        shutil.rmtree(data_dir)
    os.makedirs(data_dir, exist_ok=True)

    print("[INFO] Initializing datadir...")
    with open(log_file, "w") as f:  # write to the .mysqld log file
        subprocess.check_call([
            mysqld_path,
            f"--datadir={data_dir}",
            f"--basedir={basedir}",
            "--initialize-insecure"
        ], stdout=f, stderr=subprocess.STDOUT)
    print(f"[INFO] Datadir initialized (logs -> {log_file})")

def copy_datadir(src_dir, dest_dir):
    """
    Copy a MySQL datadir from src_dir to dest_dir.
    If dest_dir exists, it will be removed before copying.
    """
    if not os.path.exists(src_dir):
        raise FileNotFoundError(f"Source datadir not found: {src_dir}")

    if os.path.exists(dest_dir):
        print("[INFO] Removing old destination datadir...")
        shutil.rmtree(dest_dir)

    print(f"[INFO] Copying datadir from {src_dir} -> {dest_dir} ...")
    shutil.copytree(src_dir, dest_dir, symlinks=True)
    print("[INFO] Datadir copy complete.")

def start_mysqld(mysqld_path, basedir, data_dir, port, err_log, params):
    """Start mysqld with RocksDB enabled, redirect logs to file."""

    params_list = shlex.split(params) if params else []

    proc = subprocess.Popen(
        [
        mysqld_path,
        f"--datadir={data_dir}",
        f"--basedir={basedir}",
        f"--port={port}",
        "--skip-networking=0",
        "--socket=mysql.sock",
        "--plugin-load=RocksDB=ha_rocksdb.so",
        f"--log-error={err_log}",
        "--rocksdb"
        ] + params_list
    )

    print(f"[INFO] Testing mysqld params={params}")
    print(f"[INFO] mysqld started with RocksDB enabled (pid={proc.pid}), logs -> {err_log}")
    return proc

def wait_for_mysql(basedir, port, timeout=30):
    """Wait until MySQL server is ready using mysql client."""
    mysql_client = os.path.join(basedir, "bin", "mysql")
    last_error = None

    for i in range(timeout):
        try:
            subprocess.run(
                [mysql_client, "-u", "root", f"--port={port}", "--protocol=tcp", "-e", "SELECT 1"],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True
            )
            print("[INFO] mysqld is ready for queries.")
            return
        except subprocess.CalledProcessError as e:
            last_error = e.stderr.strip()
            if (i + 1) % 5 == 0:  # print only every 5 seconds
                print("[WARN] MySQL not ready yet. Error:", last_error)
            time.sleep(1)

    raise RuntimeError(f"MySQL did not start in time. Last error: {last_error}")

def stop_mysqld(proc):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    print("[INFO] mysqld stopped.")

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

    base_name = os.path.splitext(args.input_file)[0]
    mysqld_log = f"{base_name}.mysqld"
    err_log = f"{base_name}.err"
    temp_datadir = args.datadir + "_temp"
    output_file = args.input_file + ".out"

    mysqld_path = find_mysqld(args.basedir)
    init_datadir(mysqld_path, args.basedir, args.datadir, mysqld_log)

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
