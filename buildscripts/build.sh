#!/bin/bash
cd "$(dirname "$0")/.." # project root 

dtrace -C -h -s src/mongo/db/commands/uprobes.d -o src/mongo/db/commands/uprobes.h
buildscripts/scons.py mongod -j3 --disable-warnings-as-errors