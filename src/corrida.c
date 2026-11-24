/*
 * corrida.c - Demonstração de CONDIÇÃO DE CORRIDA e sua solução.
 *
 * Várias threads incrementam o mesmo contador (o "caixa" da lanchonete).
 *   - Sem sincronização: incrementos se perdem (contador++ não é atômico:
 *     ler -> somar -> escrever podem se intercalar entre threads).
 *   - Com pthread_mutex: o resultado é sempre o esperado.
 *   - Com semáforo POSIX binário: idem, usando sem_wait/sem_post.
 *
 * Uso: corrida [threads] [incrementos_por_thread]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile long caixa = 0;
static long incrementos;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t sem;

static void *sem_sync(void *a)  { (void)a; for (long i = 0; i < incrementos; i++) caixa++; return NULL; }
static void *com_mutex(void *a) { (void)a; for (long i = 0; i < incrementos; i++) { pthread_mutex_lock(&mtx); caixa++; pthread_mutex_unlock(&mtx); } return NULL; }
static void *com_sem(void *a)   { (void)a; for (long i = 0; i < incrementos; i++) { sem_wait(&sem); caixa++; sem_post(&sem); } return NULL; }

static double agora(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void rodar(const char *nome, void *(*f)(void *), int n) {
    pthread_t th[n];
    caixa = 0;
    double t0 = agora();
    for (int i = 0; i < n; i++) pthread_create(&th[i], NULL, f, NULL);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    long esperado = n * incrementos;
    printf("%-22s esperado=%-9ld obtido=%-9ld perdidos=%-9ld tempo=%.3fs %s\n",
           nome, esperado, caixa, esperado - caixa, agora() - t0,
           caixa == esperado ? "\033[1;32mOK\033[0m" : "\033[1;31mCORRIDA!\033[0m");
}

int main(int argc, char *argv[]) {
    int n = argc > 1 ? atoi(argv[1]) : 4;
    incrementos = argc > 2 ? atol(argv[2]) : 1000000;
    if (n < 1 || n > 64 || incrementos < 1) { fprintf(stderr, "parâmetros inválidos\n"); return 1; }

    sem_init(&sem, 0, 1);
    printf("Condição de corrida: %d threads x %ld incrementos no mesmo contador\n\n", n, incrementos);
    rodar("Sem proteção (inseguro)", sem_sync, n);
    rodar("Com pthread_mutex", com_mutex, n);
    rodar("Com semáforo POSIX", com_sem, n);
    sem_destroy(&sem);
    return 0;
}
