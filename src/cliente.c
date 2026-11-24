/*
 * cliente.c - Cliente externo ("aplicativo de delivery").
 *
 * Envia um pedido à lanchonete escrevendo no NAMED PIPE (FIFO).
 * Roda em outro terminal, como um processo totalmente independente.
 *
 * Uso: cliente <nome> <item> <quantidade>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comum.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <nome> <item> <quantidade>\n\nCardápio:\n", argv[0]);
        for (int i = 0; i < NUM_ITENS; i++)
            fprintf(stderr, "  %d - %-13s R$ %.2f\n", i, CARDAPIO[i].nome, CARDAPIO[i].preco);
        return EXIT_FAILURE;
    }

    PedidoCliente pc = {0};
    snprintf(pc.cliente, sizeof pc.cliente, "%s", argv[1]);
    pc.item = atoi(argv[2]);
    pc.quantidade = atoi(argv[3]);

    /* O_NONBLOCK: falha na hora (ENXIO) se a lanchonete não estiver aberta. */
    int fd = open(NOME_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENXIO || errno == ENOENT)
            fprintf(stderr, "A lanchonete está fechada (FIFO %s indisponível).\n", NOME_FIFO);
        else
            perror("open fifo");
        return EXIT_FAILURE;
    }
    if (write(fd, &pc, sizeof pc) != sizeof pc) { perror("write fifo"); return EXIT_FAILURE; }
    close(fd);

    printf("Pedido enviado: %s quer %dx item %d\n", pc.cliente, pc.quantidade, pc.item);
    return EXIT_SUCCESS;
}
