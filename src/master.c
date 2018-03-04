#include "tcp-echo.h"

extern char **environ;

typedef struct worker {
    uv_process_t child;
    uv_process_options_t options;
    unsigned int id;
    uv_pid_t pid;
    int64_t status;
    int signal;
    int alive;
    char title[MAX_TITLE];
} te_worker_t;

int te_spawn_worker(uv_loop_t *loop, te_worker_t *worker);

void te_free_worker_env(te_worker_t *worker)
{
    int i;

    for (i = 0; worker->options.env[i] != NULL; i++)
        free(worker->options.env[i]);

    free(worker->options.env);
}

void te_set_worker_env(char ***env, char *name, char *value)
{
    int i;
    char **newenv;

    for (i = 0; (*env)[i] != NULL; i++)
        continue;

    newenv = te_realloc(*env, sizeof(char *) * (i + 2));

    te_asprintf(&newenv[i], "%s=%s", name, value);

    newenv[++i] = NULL;

    *env = newenv;
}

char **te_init_worker_env()
{
    /* NOTE(awiddersheim): Want to pass on the original environment used
     * when the master process was spawned but also be able to include
     * additional variables that only get passed on to the worker. To do
     * that, copy the environment to a new location for use in the worker.
     */
    int i;
    char **envp;

    for (i = 0; environ[i] != NULL; i++)
        continue;

    envp = te_malloc(sizeof(char *) * (i + 1));

    for (i = 0; environ[i] != NULL; i++)
        if ((envp[i] = strdup(environ[i])) == NULL)
            te_log_errno(FATAL, "Could not initialize worker environment");

    envp[i] = NULL;

    return envp;
}

void te_init_worker(te_worker_t *worker, int id)
{
    int result;

    memset(worker, 0x0, sizeof(te_worker_t));

    worker->id = id;

    result = snprintf(worker->title, sizeof(worker->title), "worker-%d", id);

    if (result >= 0 && (size_t) result >= sizeof(worker->title))
        te_log(WARN, "Could not write entire worker title (worker-%d)", id);
}

void te_on_worker_exit(uv_process_t *process, int64_t status, int signal)
{
    te_worker_t *worker = (te_worker_t *) process;
    te_process_t *this_process = (te_process_t *) process->loop->data;
    struct timespec wait;

    te_log(
        INFO,
        "Worker (%s) with pid (%d) exited with (%ld)",
        worker->title,
        uv_process_get_pid(process),
        (long) status
    );

    worker->status = status;
    worker->signal = signal;
    worker->alive = 0;

    te_free_worker_env(worker);

    if (this_process->state == RUNNING) {
        /* TODO(awiddersheim): This _could_ block the entire event loop
         * for a while so find a better way to spawn this with retries.
         */
        while (te_spawn_worker(process->loop, worker)) {
            /* Sleep for 0.1 seconds */
            wait.tv_sec = 0;
            wait.tv_nsec = 100000000;

            while (nanosleep(&wait, &wait));
        }
    }
}


int te_spawn_worker(uv_loop_t *loop, te_worker_t *worker)
{
    int result;
    uv_stdio_container_t stdio[2];
    char *args[2];

    args[0] = "tcp-echo-worker";
    args[1] = NULL;

    worker->options.exit_cb = te_on_worker_exit;
    worker->options.file = "tcp-echo-worker";
    worker->options.args = args;
    worker->options.env = te_init_worker_env();

    te_set_worker_env(&worker->options.env, "WORKER_TITLE", worker->title);

    worker->options.stdio = stdio;
    worker->options.stdio[0].flags = UV_INHERIT_FD;
    worker->options.stdio[0].data.fd = 1;
    worker->options.stdio[1].flags = UV_INHERIT_FD;
    worker->options.stdio[1].data.fd = 2;
    worker->options.stdio_count = 2;

    te_log(INFO, "Creating worker (%s)", worker->title);

    if ((result = uv_spawn(loop, (uv_process_t *) worker, &worker->options)) < 0) {
        te_log_uv(ERROR, result, "Could not spawn worker (%d)", worker->id);
        return result;
    }

    worker->pid = uv_process_get_pid((uv_process_t *) worker);
    worker->alive = 1;

    te_log(INFO, "Created worker (%s) with pid (%d)", worker->title, worker->pid);

    return 0;
}

int te_update_path()
{
    int result;
    char *path;
    char *newpath;

    result = te_os_getenv("PATH", &path);

    te_asprintf(
        &newpath,
        result ? "%s." : "%s:.",
        result ? "" : path
    );

    te_log(DEBUG, "Setting PATH to (%s)", newpath);

    if ((result = uv_os_setenv("PATH", newpath)) < 0)
        te_log_uv(WARN, result, "Could not setup path");

    free(newpath);
    free(path);

    return result;
}

int main(int argc, char *argv[])
{
    int i;
    int result;
    te_worker_t *workers;
    uv_cpu_info_t *cpu_info;
    int cpu_count;
    te_process_t process = {RUNNING, 0};
    uv_loop_t loop;
    uv_signal_t sigquit;
    uv_signal_t sigterm;
    uv_signal_t sigint;

    te_initproctitle(argc, argv);
    te_setproctitle("tcp-echo", "master");
    snprintf(title, sizeof(title), "master");

    if ((result = uv_loop_init(&loop)) < 0)
        te_log_uv(FATAL, result, "Could not create master loop");

    loop.data = &process;

    uv_signal_init(&loop, &sigquit);
    uv_signal_init(&loop, &sigterm);
    uv_signal_init(&loop, &sigint);
    uv_signal_start(&sigquit, te_signal_recv, SIGQUIT);
    uv_signal_start(&sigterm, te_signal_recv, SIGTERM);
    uv_signal_start(&sigint, te_signal_recv, SIGINT);

    te_update_path();

    if ((result = uv_cpu_info(&cpu_info, &cpu_count)) < 0)
        te_log_uv(FATAL, result, "Could not determine number of CPUs");

    uv_free_cpu_info(cpu_info, cpu_count);

    #if defined(WORKERS) && WORKERS > 0
    cpu_count = WORKERS;
    #endif

    te_log(INFO, "Starting (%d) workers", cpu_count);

    workers = te_malloc(sizeof(te_worker_t) * cpu_count);

    /* TODO(awiddersheim): Pin each worker to it's own CPU. */
    for (i = 0; i < cpu_count;) {
        te_init_worker(&workers[i], i + 1);

        if (te_spawn_worker(&loop, &workers[i]))
            continue;

        i++;
    }

    te_log(INFO, "Listening on 0.0.0.0:%d", PORT_NUMBER);

    uv_run(&loop, UV_RUN_DEFAULT);

    te_log(INFO, "Master shutting down");

    /* TODO(awiddersheim) Schedule CPU affinity per worker */
    for (i = 0; i < cpu_count && process.state != KILLED;) {
        if (workers[i].alive) {
            te_log(INFO, "Terminating (worker-%d) with pid (%d)", workers[i].id, workers[i].pid);

            uv_kill(workers[i].pid, SIGTERM);

            uv_run(&loop, UV_RUN_ONCE);

            if (workers[i].alive)
                continue;
        }

        i++;
    }

    if (process.state == KILLED) {
        te_log(WARN, "Master was killed before stopping all workers");
    } else {
        free(workers);
    }

    te_log(INFO, "All done");

    return 0;
}
