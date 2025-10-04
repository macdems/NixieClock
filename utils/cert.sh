#!/bin/sh
if [ "$1" = "-ca" ]; then
    openssl req -x509 -new -newkey rsa:2048 -nodes -keyout ssl/ca_key.pem -subj "/CN=ESP-IDF CA" -out ssl/ca_cert.pem
else
    CN=$1
    [ -z "$CN" ] && CN=$(hostname)
    openssl req -new -newkey rsa:2048 -nodes -keyout ssl/server_key.pem -subj "/CN=$CN" | \
    openssl x509 -req -days 365 -CA ssl/ca_cert.pem -CAkey ssl/ca_key.pem -CAcreateserial -out ssl/server_cert.pem
fi
