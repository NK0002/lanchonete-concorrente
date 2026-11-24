/*
 * main.c - Processo GERENTE da Lanchonete Concorrente.
 *
 * Responsável por:
 *   - criar os recursos de IPC (pipe, FIFO, fila de mensagens, memória compartilhada);
 *   - criar os processos filhos com fork() (recepção, cozinha) e fork()+exec() (entregador);
 *   - tratar sinais (SIGINT encerra, SIGUSR1 mostra o placar);
 *   - encerrar tudo de forma ordenada com kill() e waitpid();
 *   - liberar os recursos de IPC no final.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <mqueue.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "comum.h"

static volatile sig_atomic_t g_parar = 0;
static volatile sig_atomic_t g_mostrar = 0;

static void tratar_sigint(int s)  { (void)s; g_parar = 1; }
static void tratar_sigusr1(int s) { (void)s; g_mostrar = 1; }

static void morrer(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

static void mostrar_placar(Estatisticas *st, const char *titulo) {
    sem_wait(&st->mutex);                       /* seção crítica (entre processos) */
    Estatisticas c = *st;
    sem_post(&st->mutex);

    printf(COR_GERENTE "\n========== %s ==========\n" COR_RESET, titulo);
    printf("  Pedidos recebidos : %d\n", c.recebidos);
    printf("  Pedidos preparados: %d\n", c.preparados);
    printf("  Pedidos entregues : %d\n", c.entregues);
    printf("  Faturamento       : R$ %.2f\n", c.faturamento);
    printf("  Por item:\n");
    for (int i = 0; i < NUM_ITENS; i++)
        printf("    %-13s %d\n", CARDAPIO[i].nome, c.por_item[i]);
    printf(COR_GERENTE "=====================================\n\n" COR_RESET);
    fflush(stdout);
}

static void uso(const char *p) {
    fprintf(stderr,
        "Uso: %s [-d segundos] [-c cozinheiros] [-f fogoes] [-b tam_buffer] [-r ms]\n"
        "  -d  duração do expediente em segundos (padrão 20; 0 = até Ctrl+C)\n"
        "  -c  número de threads cozinheiras (padrão 3)\n"
        "  -f  número de fogões = recursos do semáforo (padrão 2)\n"
        "  -b  tamanho do buffer de pedidos da cozinha (padrão 5)\n"
        "  -r  intervalo médio entre pedidos do balcão em ms (padrão 850, mínimo 50)\n", p);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    Config cfg = { .duracao_s = 20, .cozinheiros = 3, .fogoes = 2, .tam_buffer = 5, .intervalo_ms = 850 };
    int op;
    while ((op = getopt(argc, argv, "d:c:f:b:r:h")) != -1) {
        switch (op) {
        case 'd': cfg.duracao_s   = atoi(optarg); break;
        case 'c': cfg.cozinheiros = atoi(optarg); break;
        case 'f': cfg.fogoes      = atoi(optarg); break;
        case 'b': cfg.tam_buffer  = atoi(optarg); break;
        case 'r': cfg.intervalo_ms = atoi(optarg); break;
        default:  uso(argv[0]);
        }
    }
    if (cfg.cozinheiros < 1 || cfg.fogoes < 1 || cfg.tam_buffer < 1 || cfg.duracao_s < 0 || cfg.intervalo_ms < 50)
        uso(argv[0]);

    /* Caminho do binário "entregador" (mesmo diretório deste executável). */
    char exe[PATH_MAX] = {0}, caminho_entregador[PATH_MAX + 16];
    if (readlink("/proc/self/exe", exe, sizeof exe - 1) < 0) morrer("readlink");
    snprintf(caminho_entregador, sizeof caminho_entregador, "%s/entregador", dirname(exe));

    /* ---------- Sinais ---------- */
    struct sigaction sa = {0};
    sa.sa_handler = tratar_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = tratar_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    /* Limpa restos de uma execução anterior interrompida. */
    shm_unlink(NOME_SHM);
    mq_unlink(NOME_MQ);
    unlink(NOME_FIFO);

    /* ---------- Memória compartilhada POSIX ---------- */
    int fd_shm = shm_open(NOME_SHM, O_CREAT | O_RDWR, 0600);
    if (fd_shm < 0) morrer("shm_open");
    if (ftruncate(fd_shm, sizeof(Estatisticas)) < 0) morrer("ftruncate");
    Estatisticas *st = mmap(NULL, sizeof(Estatisticas), PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd_shm, 0);
    if (st == MAP_FAILED) morrer("mmap");
    close(fd_shm);
    memset(st, 0, sizeof *st);
    if (sem_init(&st->mutex, 1 /* compartilhado entre processos */, 1) < 0) morrer("sem_init");

    /* ---------- Fila de mensagens POSIX ---------- */
    struct mq_attr attr = { .mq_maxmsg = 10, .mq_msgsize = sizeof(PedidoPronto) };
    mqd_t mq = mq_open(NOME_MQ, O_CREAT | O_RDWR, 0600, &attr);
    if (mq == (mqd_t)-1) morrer("mq_open");
    mq_close(mq);   /* os filhos abrem pelo nome */

    /* ---------- Named pipe (FIFO) ---------- */
    if (mkfifo(NOME_FIFO, 0666) < 0) morrer("mkfifo");

    /* ---------- Pipe anônimo: recepção -> cozinha ---------- */
    int p[2];
    if (pipe(p) < 0) morrer("pipe");

    LOG(COR_GERENTE, "GERENTE", "Lanchonete aberta! PID=%d | %d cozinheiros, %d fogões, buffer=%d, expediente=%ds",
        getpid(), cfg.cozinheiros, cfg.fogoes, cfg.tam_buffer, cfg.duracao_s);
    LOG(COR_GERENTE, "GERENTE", "Faça pedidos com:  ./bin/cliente <nome> <item 0-4> <qtd>");
    LOG(COR_GERENTE, "GERENTE", "Placar parcial  :  kill -USR1 %d    | Encerrar: Ctrl+C", getpid());

    /* ---------- fork: ENTREGADOR (fork + exec) ---------- */
    pid_t pid_entregador = fork();
    if (pid_entregador < 0) morrer("fork entregador");
    if (pid_entregador == 0) {
        close(p[0]); close(p[1]);
        signal(SIGINT, SIG_IGN);   /* SIG_IGN é preservado pelo exec */
        execl(caminho_entregador, "entregador", NOME_MQ, NOME_SHM, (char *)NULL);
        perror("execl entregador");
        _exit(127);
    }

    /* ---------- fork: COZINHA ---------- */
    pid_t pid_cozinha = fork();
    if (pid_cozinha < 0) morrer("fork cozinha");
    if (pid_cozinha == 0) {
        signal(SIGINT, SIG_IGN);
        signal(SIGUSR1, SIG_IGN);
        close(p[1]);                       /* cozinha só lê do pipe */
        processo_cozinha(p[0], st, &cfg);
        exit(EXIT_SUCCESS);
    }

    /* ---------- fork: RECEPÇÃO ---------- */
    pid_t pid_recepcao = fork();
    if (pid_recepcao < 0) morrer("fork recepcao");
    if (pid_recepcao == 0) {
        signal(SIGINT, SIG_IGN);
        signal(SIGUSR1, SIG_IGN);
        close(p[0]);                       /* recepção só escreve no pipe */
        processo_recepcao(p[1], st, cfg.intervalo_ms);
        exit(EXIT_SUCCESS);
    }

    /* O gerente não usa o pipe: fechar é essencial para a cozinha receber EOF. */
    close(p[0]);
    close(p[1]);

    /* ---------- Expediente ---------- */
    time_t inicio = time(NULL);
    while (!g_parar && (cfg.duracao_s == 0 || time(NULL) - inicio < cfg.duracao_s)) {
        sleep(1);   /* interrompido por sinais */
        if (g_mostrar) { g_mostrar = 0; mostrar_placar(st, "PLACAR PARCIAL"); }
    }

    /* ---------- Encerramento ordenado ---------- */
    LOG(COR_GERENTE, "GERENTE", "Fechando a lanchonete: enviando SIGTERM à recepção (PID %d)...", pid_recepcao);
    kill(pid_recepcao, SIGTERM);

    int status;
    waitpid(pid_recepcao, &status, 0);
    LOG(COR_GERENTE, "GERENTE", "Recepção encerrou (status %d). Aguardando a cozinha esvaziar...", WEXITSTATUS(status));
    waitpid(pid_cozinha, &status, 0);
    LOG(COR_GERENTE, "GERENTE", "Cozinha encerrou (status %d). Aguardando o entregador...", WEXITSTATUS(status));
    waitpid(pid_entregador, &status, 0);
    LOG(COR_GERENTE, "GERENTE", "Entregador encerrou (status %d).", WEXITSTATUS(status));

    mostrar_placar(st, "PLACAR FINAL");

    /* ---------- Limpeza dos recursos de IPC ---------- */
    sem_destroy(&st->mutex);
    munmap(st, sizeof *st);
    shm_unlink(NOME_SHM);
    mq_unlink(NOME_MQ);
    unlink(NOME_FIFO);
    LOG(COR_GERENTE, "GERENTE", "Recursos de IPC liberados. Até amanhã!");
    return 0;
}
