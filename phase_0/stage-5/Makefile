CC = gcc
CFLAGS = -Wall -Wextra -std=c99
SERVER_TARGET = server
SERVER_SOURCE = tcp_proxy.c
# CLIENT_TARGET = client
# CLIENT_SOURCE = tcp_multi_client.c
# CLIENT_SOURCE = tcp_client.c
BIN = bin

all: $(SERVER_TARGET)

$(SERVER_TARGET): $(SERVER_SOURCE)
	$(CC) $(CFLAGS) -o $(BIN)/$(SERVER_TARGET) $(SERVER_SOURCE)

$(CLIENT_TARGET): $(CLIENT_SOURCE)
	$(CC) $(CFLAGS) -o $(BIN)/$(CLIENT_TARGET) $(CLIENT_SOURCE)

buildServer: $(SERVER_SOURCE)
	$(CC) $(CFLAGS) -o $(BIN)/$(SERVER_TARGET) $(SERVER_SOURCE)

runServer: $(SERVER_TARGET)
	./$(BIN)/$(SERVER_TARGET)

# buildClient: $(CLIENT_SOURCE)
# 	$(CC) $(CFLAGS) -o $(BIN)/$(CLIENT_TARGET) $(CLIENT_SOURCE)

# runClient: $(CLIENT_TARGET)
# 	./$(BIN)/$(CLIENT_TARGET)

clean:
	rm -f $(BIN)/$(SERVER_TARGET) $(BIN)/$(CLIENT_TARGET)

.PHONY: all run clean
