CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -Iinclude -pthread
LDLIBS  = -pthread -lrt
BIN     = bin

all: $(BIN)/lanchonete $(BIN)/entregador $(BIN)/cliente $(BIN)/corrida

$(BIN):
	mkdir -p $(BIN)

$(BIN)/lanchonete: src/main.c src/recepcao.c src/cozinha.c src/cardapio.c include/comum.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/main.c src/recepcao.c src/cozinha.c src/cardapio.c $(LDLIBS)

$(BIN)/entregador: src/entregador.c src/cardapio.c include/comum.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/entregador.c src/cardapio.c $(LDLIBS)

$(BIN)/cliente: src/cliente.c src/cardapio.c include/comum.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/cliente.c src/cardapio.c $(LDLIBS)

# -O0 para a condição de corrida aparecer de forma confiável
$(BIN)/corrida: src/corrida.c | $(BIN)
	$(CC) -Wall -Wextra -O0 -g -pthread -o $@ src/corrida.c $(LDLIBS)

run: all
	./$(BIN)/lanchonete

demo-corrida: $(BIN)/corrida
	./$(BIN)/corrida 4 1000000

clean:
	rm -rf $(BIN)

.PHONY: all run demo-corrida clean
