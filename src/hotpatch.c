#include "database_engine.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef ERROR_OUT_OF_MEMORY
#define ERROR_OUT_OF_MEMORY 14L
#endif
typedef int hotpatch_socklen_t;
#define HOTPATCH_CLOSE_SOCK(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
typedef socklen_t hotpatch_socklen_t;
#define HOTPATCH_CLOSE_SOCK(s) close(s)
#endif

#define HOTPATCH_ENV_FLAG "OPENDATA_HOTPATCH"
#define HOTPATCH_ENV_LISTEN_FD "OPENDATA_HOTPATCH_LISTEN_FD"
#define HOTPATCH_ENV_PIPE "OPENDATA_HOTPATCH_PIPE"
#define HOTPATCH_ENV_DRAIN_FD "OPENDATA_HOTPATCH_DRAIN_FD"
#define HOTPATCH_SPAWN_CONFIRM_MS 5000
/* The reactor stalls while confirming the child did not exit immediately, so
   keep this short; Windows binaries link only OS-supplied DLLs, making a late
   load failure after a successful CreateProcess effectively impossible. */
#define HOTPATCH_CHILD_CONFIRM_MS 250

static int g_launch_argc;
static char** g_launch_argv;
static char g_launch_exe[PATH_CAP];

#ifdef _WIN32
static WSAPROTOCOL_INFOA g_handoff_info;
static bool g_handoff_info_valid;
#else
static int g_handoff_listen_fd = -1;
static int g_handoff_drain_fd = -1;
#endif

static void hotpatch_unsetenv(const char* name) {
#ifdef _WIN32
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s=", name);
    _putenv(buffer);
#else
    unsetenv(name);
#endif
}

#ifndef _WIN32
static bool is_hotpatch_variable(const char* entry) {
    return !strncmp(entry, HOTPATCH_ENV_FLAG "=", sizeof(HOTPATCH_ENV_FLAG)) ||
           !strncmp(entry, HOTPATCH_ENV_LISTEN_FD "=", sizeof(HOTPATCH_ENV_LISTEN_FD)) ||
           !strncmp(entry, HOTPATCH_ENV_PIPE "=", sizeof(HOTPATCH_ENV_PIPE)) ||
           !strncmp(entry, HOTPATCH_ENV_DRAIN_FD "=", sizeof(HOTPATCH_ENV_DRAIN_FD));
}
#endif

void hotpatch_capture_launch(int argc, char** argv) {
    g_launch_argc = 0;
    g_launch_argv = NULL;
    g_launch_exe[0] = '\0';
    if (argc < 1 || !argv || !argv[0] || !*argv[0]) return;
    /* Resolve the launch path without following symlinks: a hot patch replaces
       (or repoints) the binary at the original path, and the successor must
       execute the replacement rather than the running image. */
#ifdef _WIN32
    if (strpbrk(argv[0], "\\/")) {
        DWORD length = GetFullPathNameA(argv[0], PATH_CAP, g_launch_exe, NULL);
        if (length == 0 || length >= PATH_CAP) g_launch_exe[0] = '\0';
    } else {
        DWORD length = SearchPathA(NULL, argv[0], ".exe", PATH_CAP, g_launch_exe, NULL);
        if (length == 0 || length >= PATH_CAP) g_launch_exe[0] = '\0';
    }
    if (!g_launch_exe[0]) {
        DWORD length = GetModuleFileNameA(NULL, g_launch_exe, PATH_CAP);
        if (length == 0 || length >= PATH_CAP) g_launch_exe[0] = '\0';
    }
#else
    if (strchr(argv[0], '/')) {
        if (argv[0][0] == '/') {
            snprintf(g_launch_exe, sizeof(g_launch_exe), "%s", argv[0]);
        } else {
            char cwd[PATH_CAP];
            if (getcwd(cwd, sizeof(cwd)))
                snprintf(g_launch_exe, sizeof(g_launch_exe), "%s/%s", cwd, argv[0]);
        }
    } else {
        const char* path_env = getenv("PATH");
        if (path_env && *path_env) {
            char* copy = strdup(path_env);
            if (copy) {
                char* save = NULL;
                for (char* dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
                    char candidate[PATH_CAP];
                    int length = snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv[0]);
                    if (length >= 0 && length < (int)sizeof(candidate) &&
                        access(candidate, X_OK) == 0) {
                        snprintf(g_launch_exe, sizeof(g_launch_exe), "%s", candidate);
                        break;
                    }
                }
                free(copy);
            }
        }
        if (!g_launch_exe[0]) snprintf(g_launch_exe, sizeof(g_launch_exe), "%s", argv[0]);
    }
#endif
    /* The successor continues the current database; a one-time --restore was
       completed by the predecessor and must not run again. */
    g_launch_argv = calloc((size_t)argc + 1, sizeof(char*));
    if (!g_launch_argv) return;
    g_launch_argv[0] = argv[0];
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--restore") && i + 1 < argc) {
            i++;
            continue;
        }
        g_launch_argv[kept++] = argv[i];
    }
    g_launch_argv[kept] = NULL;
    g_launch_argc = kept;
}

#ifndef _WIN32
static void hotpatch_close_inherited_fds(int keep_fd, int keep_fd2) {
#if defined(__linux__)
    DIR* dir = opendir("/proc/self/fd");
    if (dir) {
        int dir_fd = dirfd(dir);
        struct dirent* entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char* end = NULL;
            long fd = strtol(entry->d_name, &end, 10);
            if (end && !*end && fd >= 3 && fd != keep_fd && fd != keep_fd2 && fd != dir_fd)
                close((int)fd);
        }
        closedir(dir);
        return;
    }
#endif
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 1048576) max_fd = 1048576;
    for (long fd = 3; fd < max_fd; fd++)
        if (fd != keep_fd && fd != keep_fd2) close((int)fd);
}
#endif

bool hotpatch_child_bootstrap(void) {
    const char* marker = getenv(HOTPATCH_ENV_FLAG);
    if (!marker || strcmp(marker, "1")) return true;
    g_hotpatch_mode = true;
#ifdef _WIN32
    const char* pipe_text = getenv(HOTPATCH_ENV_PIPE);
    if (!pipe_text || !*pipe_text)
        return report_database_error("hot-patch handoff is missing the socket pipe handle");
    char* end = NULL;
    errno = 0;
    unsigned long long handle_value = _strtoui64(pipe_text, &end, 10);
    if (errno || end == pipe_text || *end || !handle_value)
        return report_database_error("hot-patch handoff pipe handle is invalid");
    HANDLE pipe = (HANDLE)(uintptr_t)handle_value;
    DWORD received = 0;
    if (!ReadFile(pipe, &g_handoff_info, (DWORD)sizeof(g_handoff_info), &received, NULL) ||
        received != (DWORD)sizeof(g_handoff_info)) {
        CloseHandle(pipe);
        return report_database_error("hot-patch socket handoff read failed; predecessor aborted the restart");
    }
    CloseHandle(pipe);
    g_handoff_info_valid = true;
#else
    const char* fd_text = getenv(HOTPATCH_ENV_LISTEN_FD);
    if (!fd_text || !*fd_text)
        return report_database_error("hot-patch handoff is missing the listener descriptor");
    char* end = NULL;
    errno = 0;
    long fd = strtol(fd_text, &end, 10);
    if (errno || end == fd_text || *end || fd < 3 || fd > 1048576)
        return report_database_error("hot-patch listener descriptor is invalid");
    g_handoff_listen_fd = (int)fd;
    const char* drain_text = getenv(HOTPATCH_ENV_DRAIN_FD);
    if (!drain_text || !*drain_text)
        return report_database_error("hot-patch handoff is missing the drain confirmation descriptor");
    end = NULL;
    errno = 0;
    long drain_fd = strtol(drain_text, &end, 10);
    if (errno || end == drain_text || *end || drain_fd < 3 || drain_fd > 1048576 || drain_fd == fd)
        return report_database_error("hot-patch drain confirmation descriptor is invalid");
    g_handoff_drain_fd = (int)drain_fd;
    /* The forked successor inherited every open descriptor (database lock,
       client sockets, backup files, and the parent's exec-confirmation pipe).
       Close all but the listener and the drain pipe FIRST: the parent's
       exec-confirmation write end must be closed here so the parent observes
       EOF and proceeds to write the drain byte this process then waits for.
       Closing the inherited lock copy also lets the predecessor release the
       database. */
    hotpatch_close_inherited_fds((int)fd, (int)drain_fd);
    /* The predecessor confirms the drain with a single byte on this pipe. If
       the restart was aborted after fork, the write end closes without a byte
       and this process exits instead of becoming a phantom successor. */
    char drain_byte = 0;
    ssize_t received;
    do {
        received = read(g_handoff_drain_fd, &drain_byte, 1);
    } while (received < 0 && errno == EINTR);
    close(g_handoff_drain_fd);
    g_handoff_drain_fd = -1;
    if (received != 1)
        return report_database_error("hot-patch restart was aborted by the predecessor");
#endif
    hotpatch_unsetenv(HOTPATCH_ENV_FLAG);
    hotpatch_unsetenv(HOTPATCH_ENV_LISTEN_FD);
    hotpatch_unsetenv(HOTPATCH_ENV_PIPE);
    hotpatch_unsetenv(HOTPATCH_ENV_DRAIN_FD);
    return true;
}

bool request_server_restart(void) {
    if (!g_server_mode)
        return report_database_error("RESTART applies to server mode (--server/--tcps) only");
    if (g_cluster_active)
        return report_database_error("RESTART is unavailable in cluster mode; restart nodes one at a time");
    if (atomic_load(&g_restart_requested))
        return report_database_error("a hot-patch restart is already in progress");
    if (!g_launch_argv || !g_launch_exe[0])
        return report_database_error("cannot restart: the launch path was not captured at startup");
    atomic_store(&g_restart_requested, true);
    fprintf(stderr, "AUDIT timestamp=%lld actor=%s operation=RESTART target=server result=initiated\n",
            (long long)time(NULL), g_network_user[0] ? g_network_user : "local-console");
    return true;
}

#ifdef _WIN32
static BOOL WINAPI hotpatch_console_handler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        atomic_store(&g_stop, true);
        return TRUE;
    default:
        return FALSE;
    }
}

void install_server_signal_handlers(void) {
    SetConsoleCtrlHandler(hotpatch_console_handler, TRUE);
}
#else
static void hotpatch_stop_signal(int sig) {
    (void)sig;
    atomic_store(&g_stop, true);
}

static void hotpatch_restart_signal(int sig) {
    (void)sig;
    atomic_store(&g_restart_requested, true);
}

void install_server_signal_handlers(void) {
    struct sigaction stop_action;
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = hotpatch_stop_signal;
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGTERM, &stop_action, NULL);
    sigaction(SIGINT, &stop_action, NULL);
    struct sigaction restart_action;
    memset(&restart_action, 0, sizeof(restart_action));
    restart_action.sa_handler = hotpatch_restart_signal;
    sigemptyset(&restart_action.sa_mask);
    sigaction(SIGUSR2, &restart_action, NULL);
    sigaction(SIGHUP, &restart_action, NULL);
    /* A failed successor must not linger as a zombie of the surviving server. */
    struct sigaction child_action;
    memset(&child_action, 0, sizeof(child_action));
    child_action.sa_handler = SIG_IGN;
    sigemptyset(&child_action.sa_mask);
    child_action.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &child_action, NULL);
}
#endif

#ifdef _WIN32
static bool append_command_line_arg(char* buffer, size_t capacity, size_t* used, const char* arg) {
    size_t n = *used;
    bool quote = !*arg;
    for (const char* scan = arg; *scan && !quote; scan++)
        if (*scan == ' ' || *scan == '\t' || *scan == '"') quote = true;
    if (!quote) {
        size_t length = strlen(arg);
        if (n >= capacity || length + 1 > capacity - n) return false;
        memcpy(buffer + n, arg, length + 1);
        *used = n + length;
        return true;
    }
    size_t length = strlen(arg);
    if (n >= capacity || 2 * length + 3 > capacity - n) return false;
    buffer[n++] = '"';
    size_t run = 0;
    for (const char* p = arg; *p; p++) {
        if (*p == '\\') { run++; continue; }
        /* MSVCRT/CommandLineToArgvW: a backslash run followed by a quote is
           doubled, then the quote itself is escaped. */
        size_t emit = (*p == '"') ? 2 * run : run;
        for (size_t i = 0; i < emit; i++) buffer[n++] = '\\';
        run = 0;
        if (*p == '"') buffer[n++] = '\\';
        buffer[n++] = *p;
    }
    for (size_t i = 0; i < 2 * run; i++) buffer[n++] = '\\';
    buffer[n++] = '"';
    buffer[n] = '\0';
    *used = n;
    return true;
}

bool hotpatch_spawn_successor(intptr_t listener, char* error, size_t error_cap) {
    if (!g_launch_argv || !g_launch_exe[0]) {
        snprintf(error, error_cap, "launch path was not captured at startup");
        return false;
    }
    char command_line[32768];
    size_t used = 0;
    for (int i = 0; i < g_launch_argc; i++) {
        if (i && used + 1 < sizeof(command_line)) command_line[used++] = ' ';
        if (!append_command_line_arg(command_line, sizeof(command_line), &used, g_launch_argv[i])) {
            snprintf(error, error_cap, "successor command line is too long");
            return false;
        }
    }
    HANDLE pipe_read = NULL, pipe_write = NULL;
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = (DWORD)sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(&pipe_read, &pipe_write, &security, 0)) {
        snprintf(error, error_cap, "cannot create handoff pipe (error %lu)", (unsigned long)GetLastError());
        return false;
    }
    if (!SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0)) {
        snprintf(error, error_cap, "cannot configure handoff pipe (error %lu)", (unsigned long)GetLastError());
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return false;
    }
    char pipe_variable[32];
    snprintf(pipe_variable, sizeof(pipe_variable), "%llu", (unsigned long long)(uintptr_t)pipe_read);
    if (!SetEnvironmentVariableA(HOTPATCH_ENV_FLAG, "1") ||
        !SetEnvironmentVariableA(HOTPATCH_ENV_PIPE, pipe_variable)) {
        snprintf(error, error_cap, "cannot publish handoff environment (error %lu)", (unsigned long)GetLastError());
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return false;
    }
    HANDLE inherit_list[4];
    int inherit_count = 0;
    inherit_list[inherit_count++] = pipe_read;
    HANDLE std_input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE std_output = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE std_error = GetStdHandle(STD_ERROR_HANDLE);
    if (std_input && std_input != INVALID_HANDLE_VALUE && inherit_count < 4)
        inherit_list[inherit_count++] = std_input;
    if (std_output && std_output != INVALID_HANDLE_VALUE && inherit_count < 4)
        inherit_list[inherit_count++] = std_output;
    if (std_error && std_error != INVALID_HANDLE_VALUE && inherit_count < 4)
        inherit_list[inherit_count++] = std_error;
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes =
        attribute_size ? (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size) : NULL;
    STARTUPINFOEXA startup;
    memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = (DWORD)sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = std_input;
    startup.StartupInfo.hStdOutput = std_output;
    startup.StartupInfo.hStdError = std_error;
    bool ready = false;
    if (attributes &&
        InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherit_list, (SIZE_T)(inherit_count * (int)sizeof(HANDLE)),
                                  NULL, NULL)) {
        startup.lpAttributeList = attributes;
        ready = true;
    }
    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    BOOL created = FALSE;
    if (ready) {
        created = CreateProcessA(g_launch_exe, command_line, NULL, NULL, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                 &startup.StartupInfo, &process);
    }
    DWORD create_error = created ? 0 : GetLastError();
    if (attributes) DeleteProcThreadAttributeList(attributes);
    free(attributes);
    SetEnvironmentVariableA(HOTPATCH_ENV_FLAG, NULL);
    SetEnvironmentVariableA(HOTPATCH_ENV_PIPE, NULL);
    if (!created) {
        snprintf(error, error_cap, "cannot launch %s (error %lu)", g_launch_exe,
                 (unsigned long)(ready ? create_error : ERROR_OUT_OF_MEMORY));
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return false;
    }
    CloseHandle(pipe_read);
    WSAPROTOCOL_INFOA info;
    memset(&info, 0, sizeof(info));
    if (WSADuplicateSocketA((SOCKET)listener, process.dwProcessId, &info) != 0) {
        snprintf(error, error_cap, "socket duplication failed (WSA error %d)", WSAGetLastError());
        CloseHandle(pipe_write);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    DWORD written = 0;
    if (!WriteFile(pipe_write, &info, (DWORD)sizeof(info), &written, NULL) ||
        written != (DWORD)sizeof(info)) {
        snprintf(error, error_cap, "socket handoff write failed (error %lu)", (unsigned long)GetLastError());
        CloseHandle(pipe_write);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    CloseHandle(pipe_write);
    DWORD wait = WaitForSingleObject(process.hProcess, HOTPATCH_CHILD_CONFIRM_MS);
    if (wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(process.hProcess, &exit_code);
        snprintf(error, error_cap, "successor exited immediately (code %lu)", (unsigned long)exit_code);
    } else if (wait != WAIT_TIMEOUT) {
        snprintf(error, error_cap, "successor confirmation failed (error %lu)", (unsigned long)GetLastError());
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_TIMEOUT;
}
#else
bool hotpatch_spawn_successor(intptr_t listener, char* error, size_t error_cap) {
    if (!g_launch_argv || !g_launch_exe[0]) {
        snprintf(error, error_cap, "launch path was not captured at startup");
        return false;
    }
    /* The child validates the listener fd is >= 3; reject here so the restart
       can still be safely aborted before the parent commits to exiting. */
    if (listener < 3) {
        snprintf(error, error_cap, "listener descriptor %d is invalid for handoff", (int)listener);
        return false;
    }
    int sync_pipe[2];
    int drain_pipe[2];
    if (pipe(sync_pipe) != 0) {
        snprintf(error, error_cap, "cannot create exec confirmation pipe: %s", strerror(errno));
        return false;
    }
    if (pipe(drain_pipe) != 0) {
        snprintf(error, error_cap, "cannot create drain confirmation pipe: %s", strerror(errno));
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return false;
    }
    if (fcntl(sync_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        snprintf(error, error_cap, "cannot configure exec confirmation pipe: %s", strerror(errno));
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        close(drain_pipe[0]);
        close(drain_pipe[1]);
        return false;
    }
    /* Build the child environment before fork(): only async-signal-safe
       calls are permitted between fork() and execve(). */
    extern char** environ;
    size_t count = 0;
    while (environ[count]) count++;
    char** child_env = calloc(count + 4, sizeof(char*));
    if (!child_env) {
        snprintf(error, error_cap, "out of memory preparing the successor environment");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        close(drain_pipe[0]);
        close(drain_pipe[1]);
        return false;
    }
    size_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (!is_hotpatch_variable(environ[i])) child_env[kept++] = environ[i];
    char flag_variable[] = HOTPATCH_ENV_FLAG "=1";
    char fd_variable[sizeof(HOTPATCH_ENV_LISTEN_FD) + 24];
    char drain_variable[sizeof(HOTPATCH_ENV_DRAIN_FD) + 24];
    snprintf(fd_variable, sizeof(fd_variable), "%s=%d", HOTPATCH_ENV_LISTEN_FD, (int)listener);
    snprintf(drain_variable, sizeof(drain_variable), "%s=%d", HOTPATCH_ENV_DRAIN_FD, drain_pipe[0]);
    child_env[kept++] = flag_variable;
    child_env[kept++] = fd_variable;
    child_env[kept++] = drain_variable;
    child_env[kept] = NULL;
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(error, error_cap, "fork failed: %s", strerror(errno));
        free(child_env);
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        close(drain_pipe[0]);
        close(drain_pipe[1]);
        return false;
    }
    if (pid == 0) {
        close(sync_pipe[0]);
        close(drain_pipe[1]);
        execve(g_launch_exe, g_launch_argv, child_env);
        int exec_errno = errno;
        (void)!write(sync_pipe[1], &exec_errno, sizeof(exec_errno));
        _exit(127);
    }
    free(child_env);
    close(drain_pipe[0]);
    bool launched = false;
    for (;;) {
        struct pollfd confirmation;
        confirmation.fd = sync_pipe[0];
        confirmation.events = POLLIN;
        confirmation.revents = 0;
        int ready = poll(&confirmation, 1, HOTPATCH_SPAWN_CONFIRM_MS);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) {
            snprintf(error, error_cap, "exec confirmation poll failed: %s", strerror(errno));
            break;
        }
        if (ready == 0) {
            /* WSL2's execve does not honor FD_CLOEXEC on pipe write ends, so
               the exec-confirmation poll can time out even after a successful
               exec. Distinguish a live successor (still waiting for the drain
               byte) from a failed exec by probing the drain pipe: if the child
               is alive and the drain read end is still open, treat it as
               launched. */
            if (kill(pid, 0) == 0) {
                struct pollfd drain_probe;
                drain_probe.fd = drain_pipe[1];
                drain_probe.events = POLLOUT;
                drain_probe.revents = 0;
                int drain_ready = poll(&drain_probe, 1, 0);
                if (drain_ready > 0 && (drain_probe.revents & POLLOUT)) {
                    launched = true;
                    break;
                }
            }
            snprintf(error, error_cap, "successor did not confirm launch within %d ms",
                     HOTPATCH_SPAWN_CONFIRM_MS);
            break;
        }
        int exec_errno = 0;
        ssize_t received = read(sync_pipe[0], &exec_errno, sizeof(exec_errno));
        if (received < 0 && errno == EINTR) continue;
        if (received == 0) {
            launched = true;
        } else if (received == (ssize_t)sizeof(exec_errno)) {
            snprintf(error, error_cap, "cannot execute %s: %s", g_launch_exe, strerror(exec_errno));
        } else {
            snprintf(error, error_cap, "exec confirmation read failed: %s", strerror(errno));
        }
        break;
    }
    if (launched) {
        /* Confirm the drain so a late-exec child from an aborted restart
           cannot become a phantom successor after this process stops. */
        char drain_byte = 'D';
        ssize_t sent;
        do {
            sent = write(drain_pipe[1], &drain_byte, 1);
        } while (sent < 0 && errno == EINTR);
        if (sent != 1) {
            snprintf(error, error_cap, "drain confirmation write failed: %s", strerror(errno));
            launched = false;
        }
    }
    close(sync_pipe[0]);
    close(sync_pipe[1]);
    close(drain_pipe[1]);
    return launched;
}
#endif

intptr_t hotpatch_adopt_listener(int port, const char* bind_addr) {
    if (!g_hotpatch_mode) return (intptr_t)-1;
#ifdef _WIN32
    if (!g_handoff_info_valid) {
        report_database_error("hot-patch socket handoff data is unavailable");
        return (intptr_t)-1;
    }
    SOCKET adopted = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, &g_handoff_info, 0, 0);
    if (adopted == INVALID_SOCKET) {
        report_database_error("cannot adopt the handed-off listener (WSA error %d)", WSAGetLastError());
        return (intptr_t)-1;
    }
    g_handoff_info_valid = false;
    SOCKET result = adopted;
#else
    if (g_handoff_listen_fd < 0) {
        report_database_error("hot-patch listener descriptor is unavailable");
        return (intptr_t)-1;
    }
    int result = g_handoff_listen_fd;
    g_handoff_listen_fd = -1;
    int option = 0;
    hotpatch_socklen_t option_len = (hotpatch_socklen_t)sizeof(option);
    if (getsockopt(result, SOL_SOCKET, SO_TYPE, &option, &option_len) != 0 || option != SOCK_STREAM) {
        report_database_error("handed-off descriptor is not a stream socket");
        HOTPATCH_CLOSE_SOCK(result);
        return (intptr_t)-1;
    }
    option = 0;
    option_len = (hotpatch_socklen_t)sizeof(option);
    if (getsockopt(result, SOL_SOCKET, SO_ACCEPTCONN, &option, &option_len) != 0 || !option) {
        report_database_error("handed-off socket is not in listen state");
        HOTPATCH_CLOSE_SOCK(result);
        return (intptr_t)-1;
    }
#endif
    struct sockaddr_in actual;
    memset(&actual, 0, sizeof(actual));
    hotpatch_socklen_t address_len = (hotpatch_socklen_t)sizeof(actual);
    if (getsockname(result, (struct sockaddr*)&actual, &address_len) != 0 ||
        actual.sin_family != AF_INET || ntohs(actual.sin_port) != (unsigned short)port) {
        report_database_error("handed-off listener does not match the configured port");
        HOTPATCH_CLOSE_SOCK(result);
        return (intptr_t)-1;
    }
    if (bind_addr && *bind_addr) {
        struct in_addr expected;
        if (inet_pton(AF_INET, bind_addr, &expected) != 1 ||
            actual.sin_addr.s_addr != expected.s_addr) {
            report_database_error("handed-off listener does not match the configured bind address");
            HOTPATCH_CLOSE_SOCK(result);
            return (intptr_t)-1;
        }
    }
    return (intptr_t)result;
}
