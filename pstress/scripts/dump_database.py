#!/bin/python

# Script to manually dump all tables from a MySQL/MyRocks database.
# Dumps both schema and data into a single SQL file, preserving table structure, indexes, and partitioning.
# Handles literal newline conversion, proper semicolons, and optional removal of MySQL version-specific comments.

import re
import argparse
import os
import subprocess
import tempfile
import sys
from mysql_utils import find_mysqld, start_mysqld, wait_for_mysql, stop_mysqld

def quote_value(val):
    """Properly quote SQL values."""
    if val is None:
        return "NULL"
    if isinstance(val, (int, float)):
        return str(val)
    # Escape single quotes
    return "'" + str(val).replace("'", "''") + "'"

def uncomment_partition_clause(create_stmt):
    """
    Converts version-specific partition comments into active SQL:
    /*!50100 PARTITION BY ... */ -> PARTITION BY ...
    """
    # Match /*!50100 PARTITION ... */
    pattern = r'/\*\!\d+\s+(PARTITION\s+BY.*?\*/)'
    def repl(m):
        inner = m.group(1)
        # remove the trailing '*/'
        if inner.endswith("*/"):
            inner = inner[:-2]
        return inner.strip()

    return re.sub(pattern, repl, create_stmt, flags=re.DOTALL)

def dump_all_tables(basedir, data_dir, port, output_file, db_name="test"):
    mysqld_path = find_mysqld(basedir)

    with tempfile.NamedTemporaryFile(prefix="mysqld_", suffix=".log") as tmp_log:
        proc = start_mysqld(
            mysqld_path=mysqld_path,
            basedir=basedir,
            data_dir=data_dir,
            port=port,
            err_log=tmp_log.name,
            params=""
        )

        try:
            wait_for_mysql(basedir, port)
            mysql_client = os.path.join(basedir, "bin", "mysql")

            # Get list of tables
            tables_cmd = [
                mysql_client, "-u", "root", f"--port={port}", "--protocol=tcp",
                "-NBe", f"SELECT table_name FROM information_schema.tables WHERE table_schema='{db_name}';"
            ]
            tables = subprocess.check_output(tables_cmd, text=True).splitlines()

            with open(output_file, "w") as f_out:
                for table in tables:
                    # Write CREATE TABLE schema
                    create_cmd = [
                        mysql_client, "-u", "root", f"--port={port}", "--protocol=tcp",
                        "-NBe", f"SHOW CREATE TABLE `{db_name}`.`{table}`;"
                    ]
                    output = subprocess.check_output(create_cmd, text=True)
                    _, create_stmt = output.split("\t", 1)
                    # Convert literal \n to actual newlines
                    create_stmt = create_stmt.replace("\\n", "\n").rstrip()

                    # Ensure semicolon is **directly at the end** (no extra newline before)
                    if not create_stmt.endswith(";"):
                        create_stmt += ";"

                    create_stmt = uncomment_partition_clause(create_stmt)
                    f_out.write(f"{create_stmt}\n\n")

                    # Write table data as INSERT statements
                    select_cmd = [
                        mysql_client, "-u", "root", f"--port={port}", "--protocol=tcp",
                        "-NBe", f"SELECT * FROM `{db_name}`.`{table}`;"
                    ]
                    rows = subprocess.check_output(select_cmd, text=True).splitlines()

                    if rows:
                        # Get column count for formatting
                        col_count_cmd = [
                            mysql_client, "-u", "root", f"--port={port}", "--protocol=tcp",
                            "-NBe", f"SELECT COUNT(*) FROM information_schema.columns WHERE table_schema='{db_name}' AND table_name='{table}';"
                        ]
                        col_count = int(subprocess.check_output(col_count_cmd, text=True).strip())

                        for row in rows:
                            values = row.split("\t")
                            quoted_values = [quote_value(v) for v in values]
                            f_out.write(f"INSERT INTO `{table}` VALUES ({', '.join(quoted_values)});\n")
                    f_out.write("\n")

            print(f"[INFO] All schemas and data dumped to {output_file}")

        finally:
            stop_mysqld(proc)

def main():
    parser = argparse.ArgumentParser(description="Dump all tables with schema and data")
    parser.add_argument("--basedir", required=True, help="MySQL base directory")
    parser.add_argument("--datadir", required=True, help="Path to MySQL data directory")
    parser.add_argument("--port", type=int, default=3307, help="Port for MySQL (default: 3307)")
    parser.add_argument("--output-file", required=True, help="Path to output .sql file")
    parser.add_argument("--database", default="test", help="Database name (default: test)")

    args = parser.parse_args()

    if not os.path.isdir(args.basedir):
        print(f"[ERROR] basedir does not exist: {args.basedir}")
        sys.exit(1)

    if not os.path.isdir(args.datadir):
        print(f"[ERROR] datadir does not exist: {args.datadir}")
        sys.exit(1)

    dump_all_tables(args.basedir, args.datadir, args.port, args.output_file, args.database)

if __name__ == "__main__":
    main()
