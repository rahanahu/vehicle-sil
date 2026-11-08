# Test data of the control server tests

`server.crt` and `server.key` are a self-signed certificate and its key for `localhost` and
`127.0.0.1`, made only for the TLS tests (`openssl req -x509 -newkey ec ...`). Do not use them
anywhere else.
