#include "mcp_common.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#endif

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
    int truncated;
} CommandOutput;

static int append_command_output(CommandOutput *output,
                                 const unsigned char *data, size_t length) {
    size_t available = output->length < output->capacity
        ? output->capacity - output->length : 0;
    size_t copy_length = length < available ? length : available;
    if (copy_length) {
        memcpy(output->data + output->length, data, copy_length);
        output->length += copy_length;
    }
    if (copy_length != length) output->truncated = 1;
    return 0;
}

#ifdef _WIN32
static wchar_t *cmd_utf8_to_wide(const char *text, int path_slashes) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     text, -1, NULL, 0);
    if (!length) return NULL;
    wchar_t *wide = calloc((size_t)length, sizeof(*wide));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             text, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    if (path_slashes)
        for (wchar_t *p = wide; *p; p++) if (*p == L'/') *p = L'\\';
    return wide;
}

static int windows_output_looks_textual(const unsigned char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char value = data[i];
        if (value == 0) return 0;
        if (value < 0x20 && value != '\t' && value != '\r' && value != '\n')
            return 0;
    }
    return 1;
}

static char *windows_oem_output_to_utf8(const unsigned char *data, size_t length,
                                        UINT *source_code_page,
                                        size_t *converted_length) {
    /* GetACP() may be CP_UTF8 because this executable has an activeCodePage
       manifest. Redirected cmd.exe built-ins still use the system OEM page. */
    UINT code_page = GetOEMCP();
    if (!length || length > INT_MAX || code_page == CP_UTF8 ||
        !windows_output_looks_textual(data, length)) return NULL;
    int wide_length = MultiByteToWideChar(code_page, 0, (const char *)data,
                                          (int)length, NULL, 0);
    if (!wide_length) return NULL;
    wchar_t *wide = malloc((size_t)wide_length * sizeof(*wide));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(code_page, 0, (const char *)data, (int)length,
                             wide, wide_length)) {
        free(wide);
        return NULL;
    }
    BOOL used_default = FALSE;
    int roundtrip_length = WideCharToMultiByte(
        code_page, WC_NO_BEST_FIT_CHARS, wide, wide_length, NULL, 0,
        NULL, &used_default);
    if (used_default || roundtrip_length != (int)length) {
        free(wide);
        return NULL;
    }
    char *roundtrip = malloc(length ? length : 1);
    if (!roundtrip || !WideCharToMultiByte(
            code_page, WC_NO_BEST_FIT_CHARS, wide, wide_length,
            roundtrip, roundtrip_length, NULL, &used_default) || used_default ||
        memcmp(roundtrip, data, length) != 0) {
        free(roundtrip);
        free(wide);
        return NULL;
    }
    free(roundtrip);
    int utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide, wide_length,
                                          NULL, 0, NULL, NULL);
    if (!utf8_length) {
        free(wide);
        return NULL;
    }
    char *utf8 = malloc((size_t)utf8_length + 1);
    if (!utf8 || !WideCharToMultiByte(CP_UTF8, 0, wide, wide_length,
                                      utf8, utf8_length, NULL, NULL)) {
        free(utf8);
        free(wide);
        return NULL;
    }
    free(wide);
    utf8[utf8_length] = '\0';
    *source_code_page = code_page;
    *converted_length = (size_t)utf8_length;
    return utf8;
}

static int run_command_process(const char *command, const char *working_directory,
                               int timeout_seconds, CommandOutput *output,
                               int *exit_code, int *timed_out) {
    wchar_t *wide_command = cmd_utf8_to_wide(command, 0);
    wchar_t *wide_directory = cmd_utf8_to_wide(working_directory, 1);
    if (!wide_command || !wide_directory) {
        free(wide_command); free(wide_directory); errno = EINVAL; return -1;
    }
    /*
     * Give cmd.exe its own hidden console and select UTF-8 before running the
     * requested command.  This avoids inheriting the user's active OEM code
     * page, which otherwise makes redirected output vary between hosts.
     */
    const wchar_t prefix[] = L"cmd.exe /d /s /c \"chcp 65001 >nul 2>&1 & ";
    size_t command_length = wcslen(prefix) + wcslen(wide_command) + 2;
    wchar_t *command_line = calloc(command_length, sizeof(*command_line));
    if (!command_line) {
        free(wide_command); free(wide_directory); return -1;
    }
    _snwprintf(command_line, command_length, L"%ls%ls\"", prefix, wide_command);
    free(wide_command);

    SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
    HANDLE read_pipe = NULL, write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        if (read_pipe) CloseHandle(read_pipe);
        if (write_pipe) CloseHandle(write_pipe);
        free(command_line); free(wide_directory); errno = EIO; return -1;
    }
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, 0, NULL);
    if (null_input == INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe); CloseHandle(write_pipe);
        free(command_line); free(wide_directory); errno = EIO; return -1;
    }

    STARTUPINFOEXW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = null_input;
    startup.StartupInfo.hStdOutput = write_pipe;
    startup.StartupInfo.hStdError = write_pipe;
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList = malloc(attribute_size);
    HANDLE inherited_handles[] = { null_input, write_pipe };
    if (!startup.lpAttributeList ||
        !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                           &attribute_size)) {
        free(startup.lpAttributeList);
        CloseHandle(null_input); CloseHandle(read_pipe); CloseHandle(write_pipe);
        free(command_line); free(wide_directory); errno = EIO; return -1;
    }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles, sizeof(inherited_handles),
                                   NULL, NULL)) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
        CloseHandle(null_input); CloseHandle(read_pipe); CloseHandle(write_pipe);
        free(command_line); free(wide_directory); errno = EIO; return -1;
    }
    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));
    BOOL created = CreateProcessW(NULL, command_line, NULL, NULL, TRUE,
                                  CREATE_NEW_CONSOLE | CREATE_SUSPENDED |
                                  CREATE_UNICODE_ENVIRONMENT |
                                  EXTENDED_STARTUPINFO_PRESENT,
                                  NULL, wide_directory, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    free(command_line); free(wide_directory);
    CloseHandle(null_input); CloseHandle(write_pipe);
    if (!created) { CloseHandle(read_pipe); errno = EIO; return -1; }

    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
        if (job) CloseHandle(job);
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        CloseHandle(read_pipe); errno = EIO; return -1;
    }
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        TerminateJobObject(job, 1);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        CloseHandle(read_pipe); CloseHandle(job); errno = EIO; return -1;
    }
    CloseHandle(process.hThread);

    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_seconds * 1000ULL;
    unsigned char buffer[8192];
    DWORD code = 1;
    for (;;) {
        DWORD available = 0;
        while (PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL) && available) {
            DWORD requested = available < sizeof(buffer) ? available : (DWORD)sizeof(buffer);
            DWORD received = 0;
            if (!ReadFile(read_pipe, buffer, requested, &received, NULL) || !received) break;
            append_command_output(output, buffer, received);
        }
        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
            GetExitCodeProcess(process.hProcess, &code);
            /* Stop descendants that outlived cmd.exe and still own the pipe. */
            TerminateJobObject(job, code);
            break;
        }
        if (GetTickCount64() >= deadline) {
            *timed_out = 1;
            TerminateJobObject(job, 124);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    for (;;) {
        DWORD received = 0;
        if (!ReadFile(read_pipe, buffer, sizeof(buffer), &received, NULL) || !received) break;
        append_command_output(output, buffer, received);
    }
    *exit_code = *timed_out ? 124 : (int)code;
    CloseHandle(read_pipe); CloseHandle(process.hProcess); CloseHandle(job);
    return 0;
}
#else
static int64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static int run_command_process(const char *command, const char *working_directory,
                               int timeout_seconds, CommandOutput *output,
                               int *exit_code, int *timed_out) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return -1;
    if (fcntl(descriptors[0], F_SETFL, fcntl(descriptors[0], F_GETFL) | O_NONBLOCK) < 0) {
        close(descriptors[0]); close(descriptors[1]); return -1;
    }
    pid_t child = fork();
    if (child < 0) { close(descriptors[0]); close(descriptors[1]); return -1; }
    if (child == 0) {
        setpgid(0, 0);
        int null_input = open("/dev/null", O_RDONLY);
        if (null_input >= 0) { dup2(null_input, STDIN_FILENO); close(null_input); }
        else close(STDIN_FILENO);
        dup2(descriptors[1], STDOUT_FILENO);
        dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[0]); close(descriptors[1]);
        if (chdir(working_directory) != 0) _exit(126);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    setpgid(child, child);
    int status = 0, child_done = 0;
    int64_t start = monotonic_milliseconds();
    unsigned char buffer[8192];
    while (!child_done) {
        ssize_t received;
        while ((received = read(descriptors[0], buffer, sizeof(buffer))) > 0)
            append_command_output(output, buffer, (size_t)received);
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) { child_done = 1; break; }
        if (waited < 0) { close(descriptors[0]); return -1; }
        int64_t now = monotonic_milliseconds();
        if (start >= 0 && now >= start + (int64_t)timeout_seconds * 1000) {
            *timed_out = 1;
            kill(-child, SIGKILL);
            waitpid(child, &status, 0);
            child_done = 1;
            break;
        }
        struct pollfd poll_descriptor = { descriptors[0], POLLIN, 0 };
        poll(&poll_descriptor, 1, 20);
    }
    if (!*timed_out) kill(-child, SIGKILL);
    for (;;) {
        ssize_t received = read(descriptors[0], buffer, sizeof(buffer));
        if (received > 0) append_command_output(output, buffer, (size_t)received);
        else if (received < 0 && errno == EINTR) continue;
        else break;
    }
    close(descriptors[0]);
    if (*timed_out) *exit_code = 124;
    else if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) *exit_code = 128 + WTERMSIG(status);
    else *exit_code = 1;
    return 0;
}
#endif

json_object* handle_run_cmd(json_object *params, json_object *id) {
    const char *command = json_object_get_string(json_object_object_get(params, "command"));
    json_object *path_object = json_object_object_get(params, "path");
    const char *path = path_object ? json_object_get_string(path_object) : "/";
    if (!command || !*command || !path || !*path) {
        send_json_rpc_error(id, -32602, "Invalid parameters: command is required and path must not be empty");
        return NULL;
    }
    int timeout_seconds = g_run_cmd_timeout_seconds;
    json_object *timeout_object = json_object_object_get(params, "timeout_seconds");
    if (timeout_object) {
        if (!json_object_is_type(timeout_object, json_type_int)) {
            send_json_rpc_error(id, -32602, "timeout_seconds must be an integer");
            return NULL;
        }
        timeout_seconds = json_object_get_int(timeout_object);
        if (timeout_seconds < 1 || timeout_seconds > g_max_run_cmd_timeout_seconds) {
            send_json_rpc_error(id, -32602, "timeout_seconds is outside the configured range");
            return NULL;
        }
    }
    char working_directory[MAX_PATH_LENGTH];
    int path_error = normalize_path(working_directory, sizeof(working_directory), (char *)path);
    if (path_error != 0) {
        send_json_rpc_error(id, path_error, "Invalid working directory");
        return NULL;
    }
    struct stat status;
    if (mcp_file_stat(working_directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
        send_json_rpc_error(id, -32602, "Working path is not a directory");
        return NULL;
    }

    CommandOutput output = {0};
    output.capacity = g_max_run_cmd_output_size;
    output.data = malloc(output.capacity ? output.capacity : 1);
    if (!output.data) {
        send_json_rpc_error(id, -32603, "Cannot allocate command output buffer");
        return NULL;
    }
    int exit_code = 1, timed_out = 0;
    if (run_command_process(command, working_directory, timeout_seconds,
                            &output, &exit_code, &timed_out) != 0) {
        free(output.data);
        send_json_rpc_error(id, -32603, "Cannot start command process");
        return NULL;
    }

    json_object *summary = json_object_new_object();
    json_object_object_add(summary, "path", json_object_new_string(path));
    json_object_object_add(summary, "exit_code", json_object_new_int(exit_code));
    json_object_object_add(summary, "timed_out", json_object_new_boolean(timed_out));
    json_object_object_add(summary, "truncated", json_object_new_boolean(output.truncated));
    if (is_valid_utf8(output.data, output.length)) {
        json_object_object_add(summary, "encoding", json_object_new_string("utf-8"));
        json_object_object_add(summary, "output",
            json_object_new_string_len((const char *)output.data, (int)output.length));
    } else {
#ifdef _WIN32
        UINT source_code_page = 0;
        size_t converted_length = 0;
        char *converted = windows_oem_output_to_utf8(
            output.data, output.length, &source_code_page, &converted_length);
        if (converted) {
            char source_encoding[32];
            snprintf(source_encoding, sizeof(source_encoding), "windows-%u",
                     (unsigned int)source_code_page);
            json_object_object_add(summary, "encoding", json_object_new_string("utf-8"));
            json_object_object_add(summary, "source_encoding",
                                   json_object_new_string(source_encoding));
            json_object_object_add(summary, "output",
                json_object_new_string_len(converted, (int)converted_length));
            free(converted);
        } else
#endif
        {
            size_t encoded_size = 4 * ((output.length + 2) / 3) + 1;
            char *encoded = malloc(encoded_size);
            if (!encoded) {
                json_object_put(summary); free(output.data);
                send_json_rpc_error(id, -32603, "Cannot encode command output");
                return NULL;
            }
            base64_encode((const char *)output.data, output.length, encoded);
            json_object_object_add(summary, "encoding", json_object_new_string("base64"));
            json_object_object_add(summary, "output", json_object_new_string(encoded));
            free(encoded);
        }
    }
    free(output.data);

    const char *text = json_object_to_json_string_ext(summary, JSON_C_TO_STRING_PLAIN);
    json_object *item = json_object_new_object();
    json_object_object_add(item, "type", json_object_new_string("text"));
    json_object_object_add(item, "text", json_object_new_string(text));
    json_object *content = json_object_new_array();
    json_object_array_add(content, item);
    json_object *result = json_object_new_object();
    json_object_object_add(result, "content", content);
    json_object_put(summary);
    return result;
}
