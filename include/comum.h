/*
 * comum.h - Definições compartilhadas por todos os componentes da
 *           Lanchonete Concorrente.
 */
#ifndef COMUM_H
#define COMUM_H

#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>

/* ---------- Nomes dos recursos de IPC ---------- */
#define NOME_FIFO   "/tmp/lanchonete_fifo"   /* Named pipe: clientes externos -> recepção */
#define NOME_MQ     "/lanchonete_prontos"    /* Fila POSIX: cozinha -> entregador         */
#define NOME_SHM    "/lanchonete_stats"      /* Memória compartilhada: estatísticas       */

/* ---------- Cardápio ---------- */
#define NUM_ITENS 5

typedef struct {
    const char *nome;
    double      preco;
    int         tempo_preparo_ms;
} ItemCardapio;

extern const ItemCardapio CARDAPIO[NUM_ITENS];

/* ---------- Mensagens ---------- */
enum { ORIGEM_BALCAO = 0, ORIGEM_APP = 1 };   /* APP = cliente externo via FIFO */

/* Pedido trafega pelo pipe anônimo (recepção -> cozinha).
 * sizeof(Pedido) < PIPE_BUF, então cada write() é atômico. */
typedef struct {
    int  id;
    int  item;
    int  quantidade;
    int  origem;
    char cliente[32];
} Pedido;

/* Mensagem que o cliente externo escreve no FIFO. */
typedef struct {
    char cliente[32];
    int  item;
    int  quantidade;
} PedidoCliente;

/* Mensagem da fila POSIX (cozinha -> entregador). id < 0 = fim. */
typedef struct {
    Pedido pedido;
    int    cozinheiro;
} PedidoPronto;

/* ---------- Memória compartilhada ---------- */
typedef struct {
    sem_t  mutex;                 /* semáforo binário com pshared = 1 */
    int    recebidos;
    int    preparados;
    int    entregues;
    int    por_item[NUM_ITENS];
    double faturamento;
} Estatisticas;

/* ---------- Configuração ---------- */
typedef struct {
    int duracao_s;
    int cozinheiros;
    int fogoes;
    int tam_buffer;
    int intervalo_ms;   /* intervalo médio entre pedidos do balcão */
} Config;

/* ---------- Funções dos componentes (mesmo binário, via fork) ---------- */
void processo_recepcao(int fd_escrita_pipe, Estatisticas *st, int intervalo_ms);
void processo_cozinha(int fd_leitura_pipe, Estatisticas *st, const Config *cfg);

/* ---------- Log colorido ---------- */
#define COR_RESET   "\033[0m"
#define COR_GERENTE "\033[1;37m"
#define COR_RECEP   "\033[1;36m"
#define COR_COZINHA "\033[1;33m"
#define COR_ENTREG  "\033[1;32m"
#define COR_ERRO    "\033[1;31m"

#define LOG(cor, tag, fmt, ...) \
    do { printf(cor "[%-10s] " COR_RESET fmt "\n", tag, ##__VA_ARGS__); fflush(stdout); } while (0)

static inline void dormir_ms(int ms) {
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) == -1) { /* retoma se interrompido por sinal */ }
}

#endif /* COMUM_H */
