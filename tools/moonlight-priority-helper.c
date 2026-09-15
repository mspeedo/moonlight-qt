#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TARGET_NICE (-10)

static int parse_fd(const char* text)
{
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > INT_MAX) {
        return -1;
    }
    return (int)value;
}

static int pid_from_pidfd(int fd, pid_t* pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);

    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    char* line = NULL;
    size_t line_capacity = 0;
    int result = -1;
    while (getline(&line, &line_capacity, file) >= 0) {
        if (strncmp(line, "Pid:", 4) != 0) {
            continue;
        }

        char* value = line + 4;
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        char* end = NULL;
        errno = 0;
        long parsed = strtol(value, &end, 10);
        if (errno == 0 && end != value && parsed > 0 && parsed <= INT_MAX) {
            *pid = (pid_t)parsed;
            result = 0;
        }
        break;
    }

    free(line);
    fclose(file);
    return result;
}

static int read_comm(const char* path, char* output, size_t output_size)
{
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    if (fgets(output, (int)output_size, file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);

    output[strcspn(output, "\r\n")] = '\0';
    return 0;
}

static int allowed_thread_name(const char* name)
{
    return strcmp(name, "VideoRecv") == 0 ||
           strcmp(name, "FFDecoder") == 0 ||
           strcmp(name, "PacerRender") == 0;
}

static int validate_target(pid_t process_pid, pid_t thread_tid, const char* expected_thread)
{
    if (!allowed_thread_name(expected_thread)) {
        fprintf(stderr, "moonlight-priority-helper: rejected thread name '%s'\n", expected_thread);
        return -1;
    }

    char process_path[64];
    snprintf(process_path, sizeof(process_path), "/proc/%d", process_pid);

    struct stat process_stat;
    if (stat(process_path, &process_stat) != 0) {
        perror("moonlight-priority-helper: stat process");
        return -1;
    }

    if (process_stat.st_uid != getuid()) {
        fprintf(stderr, "moonlight-priority-helper: target process belongs to another uid\n");
        return -1;
    }

    char path[128];
    char comm[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", process_pid);
    if (read_comm(path, comm, sizeof(comm)) != 0) {
        perror("moonlight-priority-helper: read process comm");
        return -1;
    }

    if (strcmp(comm, "moonlight") != 0) {
        fprintf(stderr, "moonlight-priority-helper: target process is '%s', not moonlight\n", comm);
        return -1;
    }

    snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", process_pid, thread_tid);
    if (read_comm(path, comm, sizeof(comm)) != 0) {
        perror("moonlight-priority-helper: read thread comm");
        return -1;
    }

    if (strcmp(comm, expected_thread) != 0) {
        fprintf(stderr,
                "moonlight-priority-helper: thread is '%s', expected '%s'\n",
                comm,
                expected_thread);
        return -1;
    }

    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s PROCESS_PIDFD_FD THREAD_PIDFD_FD THREAD_NAME\n",
                argv[0]);
        return 2;
    }

    const int process_fd = parse_fd(argv[1]);
    const int thread_fd = parse_fd(argv[2]);
    if (process_fd < 0 || thread_fd < 0) {
        fprintf(stderr, "moonlight-priority-helper: invalid forwarded fd\n");
        return 3;
    }

    pid_t process_pid = -1;
    pid_t thread_tid = -1;
    if (pid_from_pidfd(process_fd, &process_pid) != 0 ||
        pid_from_pidfd(thread_fd, &thread_tid) != 0) {
        fprintf(stderr, "moonlight-priority-helper: unable to resolve forwarded pidfd\n");
        return 4;
    }

    if (validate_target(process_pid, thread_tid, argv[3]) != 0) {
        return 5;
    }

    errno = 0;
    int current_nice = getpriority(PRIO_PROCESS, (id_t)thread_tid);
    if (errno != 0) {
        perror("moonlight-priority-helper: getpriority");
        return 6;
    }

    if (current_nice > TARGET_NICE) {
        if (setpriority(PRIO_PROCESS, (id_t)thread_tid, TARGET_NICE) != 0) {
            perror("moonlight-priority-helper: setpriority");
            return 7;
        }
    }

    errno = 0;
    const int final_nice = getpriority(PRIO_PROCESS, (id_t)thread_tid);
    if (errno != 0) {
        perror("moonlight-priority-helper: verify getpriority");
        return 8;
    }

    if (final_nice > TARGET_NICE) {
        fprintf(stderr,
                "moonlight-priority-helper: priority unchanged (nice=%d)\n",
                final_nice);
        return 9;
    }

    return 0;
}
