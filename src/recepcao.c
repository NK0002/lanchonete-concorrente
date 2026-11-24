/*
 * recepcao.c - Processo RECEPÇÃO.
 *
 * Duas threads POSIX produzem pedidos e os escrevem no pipe anônimo
 * que leva à cozinha:
 *   - thread "balcão": gera pedidos automáticos (clientes presenciais);
 *   - thread "app"   : lê pedidos de clientes externos pelo named pipe (FIFO).
 *
 * Um pthread_mutex protege o contador de IDs e a escrita no pipe
 * (seção crítica compartilhada pelas duas threads).
 * O processo termina ao receber SIGTERM do gerente.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comum.h"

static volatile sig_atomic_t g_fim = 0;
static void tratar_sigterm(int s) { (void)s; g_fim = 1; }

static pthread_mutex_t mtx_pedido = PTHREAD_MUTEX_INITIALIZER;
static int proximo_id = 1;
static int fd_pipe;
static Estatisticas *stats;
static int intervalo = 850;

static const char *NOMES[] = { "Ana", "Bruno", "Carla", "Diego", "Elisa",
                               "Fábio", "Gabi", "Heitor", "Iara", "João" };

/* Seção crítica: gera ID, envia pelo pipe e atualiza a memória compartilhada. */
static void registrar_pedido(const char *cliente, int item, int qtd, int origem) {
    Pedido p = { .item = item, .quantidade = qtd, .origem = origem };
    snprintf(p.cliente, sizeof p.cliente, "%s", cliente);

    pthread_mutex_lock(&mtx_pedido);
    p.id = proximo_id++;
    if (write(fd_pipe, &p, sizeof p) != sizeof p)
        perror("recepcao: write pipe");
    pthread_mutex_unlock(&mtx_pedido);

    sem_wait(&stats->mutex);
    stats->recebidos++;
    sem_post(&stats->mutex);

    LOG(COR_RECEP, "RECEPÇÃO  ", "Pedido #%-3d %-6s %-8s %dx %s",
        p.id, origem == ORIGEM_APP ? "[APP]" : "[BALC]", p.cliente, qtd, CARDAPIO[item].nome);
}

/* Thread produtora 1: clientes no balcão (pedidos aleatórios). */
static void *thread_balcao(void *arg) {
    (void)arg;
    unsigned semente = (unsigned)getpid();
    while (!g_fim) {
        int espera = intervalo / 2 + rand_r(&semente) % (intervalo + 1);
        for (int t = 0; t < espera && !g_fim; t += 50) dormir_ms(50);
        if (g_fim) break;
        registrar_pedido(NOMES[rand_r(&semente) % 10],
                         rand_r(&semente) % NUM_ITENS,
                         1 + rand_r(&semente) % 3, ORIGEM_BALCAO);
    }
    return NULL;
}

/* Thread produtora 2: clientes do "aplicativo", vindos pelo FIFO. */
static void *thread_app(void *arg) {
    (void)arg;
    /* O_RDWR mantém um escritor aberto, evitando EOF quando nenhum cliente está conectado. */
    int fd = open(NOME_FIFO, O_RDWR);
    if (fd < 0) { perror("recepcao: open fifo"); return NULL; }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (!g_fim) {
        int r = poll(&pfd, 1, 200);        /* timeout para checar g_fim */
        if (r <= 0) continue;
        PedidoCliente pc;
        ssize_t n = read(fd, &pc, sizeof pc);
        if (n != sizeof pc) continue;
        pc.cliente[sizeof pc.cliente - 1] = '\0';
        if (pc.item < 0 || pc.item >= NUM_ITENS || pc.quantidade < 1) {
            LOG(COR_ERRO, "RECEPÇÃO  ", "Pedido inválido de %s ignorado", pc.cliente);
            continue;
        }
        registrar_pedido(pc.cliente, pc.item, pc.quantidade, ORIGEM_APP);
    }
    close(fd);
    return NULL;
}

void processo_recepcao(int fd_escrita_pipe, Estatisticas *st, int intervalo_ms) {
    fd_pipe = fd_escrita_pipe;
    intervalo = intervalo_ms;
    stats = st;

    struct sigaction sa = {0};
    sa.sa_handler = tratar_sigterm;
    sigaction(SIGTERM, &sa, NULL);

    LOG(COR_RECEP, "RECEPÇÃO  ", "Processo iniciado (PID %d). Threads: balcão + app(FIFO)", getpid());

    pthread_t tb, ta;
    pthread_create(&tb, NULL, thread_balcao, NULL);
    pthread_create(&ta, NULL, thread_app, NULL);
    pthread_join(tb, NULL);
    pthread_join(ta, NULL);

    close(fd_pipe);   /* fecha a escrita: a cozinha verá EOF */
    LOG(COR_RECEP, "RECEPÇÃO  ", "Portas fechadas. Pipe encerrado.");
}
