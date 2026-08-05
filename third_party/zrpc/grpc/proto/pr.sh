#!/bin/sh
#set -x

protoc -I=./ --cpp_out=../ ./*.proto

echo "run protoc success, go, go, go...";
