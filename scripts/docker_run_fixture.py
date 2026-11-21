#!/usr/bin/env python3
"""Runs the container on one dataset. Usage: docker_run_fixture.py DATA OUT"""
import os
import subprocess
import sys

data, out = (os.path.abspath(p) for p in sys.argv[1:3])
image = os.environ.get("IMAGE", "shopping-sessions")
cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "--network", "none", "--read-only",
       "--tmpfs", "/tmp:rw,size=64m", "--cpus", "8", "--memory", "24g", "--user", "1000:1000",
       "-v", f"{data}:/data:ro", "-v", f"{out}:/output", "-v", f"{out}:/scratch",
       "-e", "DATA_DIR=/data", "-e", "OUTPUT_DIR=/output", "-e", "SCRATCH_DIR=/scratch", image]
os.chmod(out, 0o777)
sys.exit(subprocess.run(cmd).returncode)
