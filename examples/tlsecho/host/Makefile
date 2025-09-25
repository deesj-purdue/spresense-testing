PORT = 8443

all: tlsecho_server

tlsecho_server: tlsecho_server.c
	gcc -Wall -O2 $^ -o $@ -lssl -lcrypto

run_server: tlsecho_server server.crt
	./tlsecho_server $(PORT) server.crt server.key

server.crt: generate_key.sh
	./generate_key.sh

clean:
	rm -f tlsecho_server server.crt server.key

