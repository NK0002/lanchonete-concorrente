/*
 * cozinha.c - Processo COZINHA.
 *
 * Modelo PRODUTOR-CONSUMIDOR com buffer limitado implementado como MONITOR
 * (pthread_mutex + duas variáveis de condição):
 *   - thread "garçom" (produtora): lê pedidos do pipe e coloca no buffer;
 *   - N threads "cozinheiro" (consumidoras): retiram do buffer e preparam.
 *
 * Os fogões são um recurso limitado controlado por um SEMÁFORO CONTADOR
 * (sem_init com valor = número de fogões): no máximo F cozinheiros
 * cozinham ao mesmo tempo.
 *
 * Pedidos prontos seguem para o entregador por uma FILA DE MENSAGENS POSIX
 * (pedidos do app recebem prioridade maior).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comum.h"

/* ---------- Monitor: buffer circular limitado ---------- */
typedef struct {
    Pedido         *itens;
    int             capacidade, inicio, qtd;
    int             encerrado;          /* pipe chegou ao fim */
    pthread_mutex_t mtx;
    pthread_cond_t  nao_cheio, nao_vazio;
} BufferPedidos;

static BufferPedidos buf;
static sem_t         fogoes;
static int           fogoes_em_uso = 0;
static pthread_mutex_t mtx_fogoes = PTHREAD_MUTEX_INITIALIZER;
static mqd_t         mq;
static Estatisticas *stats;
static int           fd_pipe;

static void buffer_iniciar(int cap) {
    buf.itens = calloc(cap, sizeof(Pedido));
    buf.capacidade = cap;
    buf.inicio = buf.qtd = buf.encerrado = 0;
    pthread_mutex_init(&buf.mtx, NULL);
    pthread_cond_init(&buf.nao_cheio, NULL);
    pthread_cond_init(&buf.nao_vazio, NULL);
}

/* Operação do monitor: depositar (bloqueia se cheio). */
static void buffer_depositar(Pedido p) {
    pthread_mutex_lock(&buf.mtx);
    while (buf.qtd == buf.capacidade) {
        LOG(COR_COZINHA, "COZINHA", "Buffer CHEIO (%d/%d) - garçom aguardando...", buf.qtd, buf.capacidade);
        pthread_cond_wait(&buf.nao_cheio, &buf.mtx);
    }
    buf.itens[(buf.inicio + buf.qtd) % buf.capacidade] = p;
    buf.qtd++;
    pthread_cond_signal(&buf.nao_vazio);
    pthread_mutex_unlock(&buf.mtx);
}

/* Operação do monitor: retirar (bloqueia se vazio). Retorna 0 quando acabou. */
static int buffer_retirar(Pedido *p) {
    pthread_mutex_lock(&buf.mtx);
    while (buf.qtd == 0 && !buf.encerrado)
        pthread_cond_wait(&buf.nao_vazio, &buf.mtx);
    if (buf.qtd == 0 && buf.encerrado) {
        pthread_mutex_unlock(&buf.mtx);
        return 0;
    }
    *p = buf.itens[buf.inicio];
    buf.inicio = (buf.inicio + 1) % buf.capacidade;
    buf.qtd--;
    pthread_cond_signal(&buf.nao_cheio);
    pthread_mutex_unlock(&buf.mtx);
    return 1;
}

static void buffer_encerrar(void) {
    pthread_mutex_lock(&buf.mtx);
    buf.encerrado = 1;
    pthread_cond_broadcast(&buf.nao_vazio);   /* acorda todos os cozinheiros */
    pthread_mutex_unlock(&buf.mtx);
}

/* ---------- Thread produtora: garçom ---------- */
static void *thread_garcom(void *arg) {
    (void)arg;
    Pedido p;
    ssize_t n;
    while ((n = read(fd_pipe, &p, sizeof p)) == sizeof p)
        buffer_depositar(p);
    /* read() == 0  =>  EOF: a recepção fechou o pipe. */
    LOG(COR_COZINHA, "COZINHA", "Garçom: pipe fechado (EOF). Sem novos pedidos.");
    buffer_encerrar();
    return NULL;
}

/* ---------- Threads consumidoras: cozinheiros ---------- */
static void *thread_cozinheiro(void *arg) {
    int id = (int)(long)arg;
    Pedido p;
    while (buffer_retirar(&p)) {
        sem_wait(&fogoes);                              /* pega um fogão (ou espera) */
        pthread_mutex_lock(&mtx_fogoes);
        int em_uso = ++fogoes_em_uso;
        pthread_mutex_unlock(&mtx_fogoes);

        LOG(COR_COZINHA, "COZINHA", "Cozinheiro %d preparando #%-3d %dx %-12s (fogões em uso: %d)",
            id, p.id, p.quantidade, CARDAPIO[p.item].nome, em_uso);
        dormir_ms(CARDAPIO[p.item].tempo_preparo_ms + 150 * (p.quantidade - 1));

        pthread_mutex_lock(&mtx_fogoes);
        fogoes_em_uso--;
        pthread_mutex_unlock(&mtx_fogoes);
        sem_post(&fogoes);                              /* libera o fogão */

        sem_wait(&stats->mutex);
        stats->preparados++;
        sem_post(&stats->mutex);

        PedidoPronto pr = { .pedido = p, .cozinheiro = id };
        unsigned prioridade = (p.origem == ORIGEM_APP) ? 5 : 1;
        if (mq_send(mq, (const char *)&pr, sizeof pr, prioridade) < 0)
            perror("cozinha: mq_send");
    }
    LOG(COR_COZINHA, "COZINHA", "Cozinheiro %d terminou o expediente.", id);
    return NULL;
}

void processo_cozinha(int fd_leitura_pipe, Estatisticas *st, const Config *cfg) {
    fd_pipe = fd_leitura_pipe;
    stats = st;

    mq = mq_open(NOME_MQ, O_WRONLY);
    if (mq == (mqd_t)-1) { perror("cozinha: mq_open"); exit(EXIT_FAILURE); }

    buffer_iniciar(cfg->tam_buffer);
    sem_init(&fogoes, 0 /* entre threads */, cfg->fogoes);

    LOG(COR_COZINHA, "COZINHA", "Processo iniciado (PID %d). %d cozinheiros, %d fogões.",
        getpid(), cfg->cozinheiros, cfg->fogoes);

    pthread_t garcom, *cozinheiros = calloc(cfg->cozinheiros, sizeof(pthread_t));
    pthread_create(&garcom, NULL, thread_garcom, NULL);
    for (long i = 0; i < cfg->cozinheiros; i++)
        pthread_create(&cozinheiros[i], NULL, thread_cozinheiro, (void *)(i + 1));

    pthread_join(garcom, NULL);
    for (int i = 0; i < cfg->cozinheiros; i++)
        pthread_join(cozinheiros[i], NULL);

    /* Mensagem sentinela: avisa o entregador que acabou (prioridade mínima = vai por último). */
    PedidoPronto fim = { .pedido = { .id = -1 } };
    mq_send(mq, (const char *)&fim, sizeof fim, 0);

    LOG(COR_COZINHA, "COZINHA", "Cozinha fechada.");
    mq_close(mq);
    close(fd_pipe);
    sem_destroy(&fogoes);
    free(cozinheiros);
    free(buf.itens);
}
