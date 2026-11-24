/*
 * entregador.c - Programa ENTREGADOR (binário separado).
 *
 * É iniciado pelo gerente com fork() + execl(). Como o exec substitui
 * a imagem do processo, ele não herda variáveis: abre a fila de mensagens
 * e a memória compartilhada pelo NOME recebido em argv.
 *
 * Uso: entregador <nome_fila> <nome_shm>
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <mqueue.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "comum.h"

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <nome_fila> <nome_shm>\n", argv[0]);
        return EXIT_FAILURE;
    }

    mqd_t mq = mq_open(argv[1], O_RDONLY);
    if (mq == (mqd_t)-1) { perror("entregador: mq_open"); return EXIT_FAILURE; }

    int fd = shm_open(argv[2], O_RDWR, 0);
    if (fd < 0) { perror("entregador: shm_open"); return EXIT_FAILURE; }
    Estatisticas *st = mmap(NULL, sizeof *st, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (st == MAP_FAILED) { perror("entregador: mmap"); return EXIT_FAILURE; }
    close(fd);

    LOG(COR_ENTREG, "ENTREGADOR", "Processo iniciado via exec (PID %d, pai %d)", getpid(), getppid());

    PedidoPronto pr;
    unsigned prio;
    for (;;) {
        ssize_t n = mq_receive(mq, (char *)&pr, sizeof pr, &prio);   /* bloqueia até chegar algo */
        if (n < 0) { perror("entregador: mq_receive"); break; }
        if (pr.pedido.id < 0) break;                                  /* sentinela de fim */

        Pedido *p = &pr.pedido;
        double valor = CARDAPIO[p->item].preco * p->quantidade;
        dormir_ms(200);

        sem_wait(&st->mutex);                 /* seção crítica entre processos */
        st->entregues++;
        st->por_item[p->item] += p->quantidade;
        st->faturamento += valor;
        sem_post(&st->mutex);

        LOG(COR_ENTREG, "ENTREGADOR", "Entregue #%-3d para %-8s R$ %6.2f %s(cozinheiro %d, prio %u)",
            p->id, p->cliente, valor, prio > 1 ? "★ APP " : "", pr.cozinheiro, prio);
    }

    LOG(COR_ENTREG, "ENTREGADOR", "Todas as entregas concluídas.");
    munmap(st, sizeof *st);
    mq_close(mq);
    return EXIT_SUCCESS;
}
