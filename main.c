#if defined(_WIN32)
#include <windows.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define Sleep(ms) usleep((ms) * 1000)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MAX_RESPONSE 4096
#define DEFAULT_PORT "COM3"
#define SERVICE_NAME "smset"

typedef struct {
    char port[32];
    DWORD baud;
    int service_mode;
} SmsConfig;

#if defined(_WIN32)
static volatile BOOL g_running = TRUE;
static SERVICE_STATUS_HANDLE g_status_handle = 0;
static SERVICE_STATUS g_status;
#endif

static void log_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stdout, "[smset] ");
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
    va_end(args);
}

static void log_response(const char *label, const char *response) {
    if (response == NULL || response[0] == '\0') {
        log_message("%s: <empty>", label);
        return;
    }

    log_message("%s: %s", label, response);
}

static void normalize_port_name(const char *port, char *buffer, size_t buffer_size) {
#if defined(_WIN32)
    if (strncmp(port, "\\\\.\\", 4) == 0) {
        strncpy(buffer, port, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return;
    }

    snprintf(buffer, buffer_size, "\\\\.\\%s", port);
#else
    strncpy(buffer, port, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
#endif
}

static HANDLE open_port(const SmsConfig *cfg) {
    char full_port[64];
    normalize_port_name(cfg->port, full_port, sizeof(full_port));

#if defined(_WIN32)
    log_message("Opening serial port %s", full_port);
    HANDLE hPort = CreateFileA(
        full_port,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hPort == INVALID_HANDLE_VALUE) {
        log_message("Failed to open %s. Error=%lu", full_port, GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(hPort, &dcb)) {
        log_message("GetCommState failed. Error=%lu", GetLastError());
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = cfg->baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (!SetCommState(hPort, &dcb)) {
        log_message("SetCommState failed. Error=%lu", GetLastError());
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }

    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 1000;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    if (!SetCommTimeouts(hPort, &timeouts)) {
        log_message("SetCommTimeouts failed. Error=%lu", GetLastError());
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }

    PurgeComm(hPort, PURGE_RXCLEAR | PURGE_TXCLEAR);
    log_message("Port configuration complete for %s at %lu baud", full_port, cfg->baud);
    return hPort;
#else
    log_message("Simulating serial port open for %s", full_port);
    return (HANDLE)1;
#endif
}

static int write_serial(HANDLE hPort, const char *data) {
#if defined(_WIN32)
    DWORD written = 0;
    return WriteFile(hPort, data, (DWORD)strlen(data), &written, NULL) && written > 0;
#else
    (void)hPort;
    log_message("TX: %s", data);
    return 1;
#endif
}

static int read_serial(HANDLE hPort, char *buffer, size_t buffer_size, DWORD timeout_ms) {
#if defined(_WIN32)
    DWORD start = GetTickCount();
    size_t index = 0;
    DWORD read = 0;

    while ((GetTickCount() - start) < timeout_ms) {
        if (ReadFile(hPort, buffer + index, 1, &read, NULL) && read == 1) {
            index++;
            if (index >= buffer_size - 1) {
                break;
            }
            if (strstr(buffer, "OK") != NULL || strstr(buffer, "ERROR") != NULL || strstr(buffer, ">") != NULL) {
                break;
            }
        } else {
            Sleep(50);
        }
    }

    if (index > 0) {
        buffer[index] = '\0';
        return 1;
    }

    buffer[0] = '\0';
    return 0;
#else
    (void)hPort;
    (void)timeout_ms;
    if (buffer != NULL && buffer_size > 0) {
        if (strstr(buffer, ">") != NULL) {
            snprintf(buffer, buffer_size, ">" );
        } else {
            snprintf(buffer, buffer_size, "OK");
        }
    }
    log_message("RX: %s", buffer != NULL ? buffer : "<null>");
    return 1;
#endif
}

static int send_command(HANDLE hPort, const char *command, char *response, size_t response_size) {
    char local_response[MAX_RESPONSE];
    memset(local_response, 0, sizeof(local_response));

    log_message("Sending AT command: %s", command != NULL ? command : "<empty>");

    if (command != NULL && command[0] != '\0') {
        if (!write_serial(hPort, command)) {
            log_message("Failed to write command: %s", command);
            return 0;
        }
    }

    if (!read_serial(hPort, local_response, sizeof(local_response), 2000)) {
        log_message("No response for: %s", command != NULL ? command : "<empty>");
        return 0;
    }

    log_response("Modem reply", local_response);

    if (response != NULL && response_size > 0) {
        strncpy(response, local_response, response_size - 1);
        response[response_size - 1] = '\0';
    }

    return 1;
}

static int init_modem(HANDLE hPort) {
    char response[MAX_RESPONSE];
    log_message("Initializing modem with AT commands");

    if (!send_command(hPort, "AT\r", response, sizeof(response))) {
        return 0;
    }

    if (!send_command(hPort, "AT+CMGF=1\r", response, sizeof(response))) {
        return 0;
    }

    if (!send_command(hPort, "AT+CNMI=2,1,0,0,0\r", response, sizeof(response))) {
        return 0;
    }

    log_message("Modem initialized successfully.");
    return 1;
}

static int send_sms(HANDLE hPort, const char *phone, const char *message, char *response, size_t response_size) {
    char cmd[256];
    char payload[MAX_RESPONSE];

    log_message("Preparing SMS send to %s", phone);
    log_message("SMS payload length: %zu", strlen(message));

    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r", phone);
    if (!send_command(hPort, cmd, response, response_size)) {
        return 0;
    }

    if (strstr(response, ">") == NULL) {
        log_message("Modem did not return the SMS prompt. Response: %s", response);
        return 0;
    }

    snprintf(payload, sizeof(payload), "%s\x1A", message);
    log_message("Transmitting SMS payload");
    if (!write_serial(hPort, payload)) {
        return 0;
    }

    if (!read_serial(hPort, response, response_size, 5000)) {
        log_message("No confirmation received after sending SMS.");
        return 0;
    }

    log_response("SMS send confirmation", response);
    return 1;
}

static int receive_sms(HANDLE hPort, char *response, size_t response_size) {
    log_message("Polling modem for incoming SMS messages");
    if (!send_command(hPort, "AT+CMGL=\"ALL\"\r", response, response_size)) {
        return 0;
    }
    return 1;
}

#if defined(_WIN32)
static int run_sms_loop(const SmsConfig *cfg) {
    HANDLE hPort = open_port(cfg);
    if (hPort == INVALID_HANDLE_VALUE) {
        return 1;
    }

    if (!init_modem(hPort)) {
        log_message("Modem initialization failed.");
        return 1;
    }

    log_message("Monitoring SMS on %s. Press Ctrl+C to stop.", cfg->port);

    while (g_running) {
        char response[MAX_RESPONSE];
        memset(response, 0, sizeof(response));

        if (receive_sms(hPort, response, sizeof(response))) {
            if (response[0] != '\0') {
                log_message("Incoming SMS data:\n%s", response);
            }
        }

        Sleep(5000);
    }

    log_message("Stopping SMS monitoring loop.");
    return 0;
}
#endif

#if defined(_WIN32)

        Sleep(5000);
    }

    log_message("Stopping SMS monitoring loop.");
    return 0;
}
#endif

#if defined(_WIN32)
static void set_service_status(DWORD state, DWORD exit_code, DWORD wait_hint) {
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint = wait_hint;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_START_PENDING) ? 1 : 0;
    SetServiceStatus(g_status_handle, &g_status);
}

static DWORD WINAPI service_worker_thread(LPVOID param) {
    SmsConfig *cfg = (SmsConfig *)param;
    int result = run_sms_loop(cfg);
    (void)result;
    set_service_status(SERVICE_STOPPED, 0, 0);
    return 0;
}

static void WINAPI service_ctrl_handler(DWORD control) {
    if (control == SERVICE_CONTROL_STOP) {
        g_running = FALSE;
        set_service_status(SERVICE_STOP_PENDING, 0, 1000);
    }
}

static void WINAPI service_main(DWORD argc, LPTSTR *argv) {
    (void)argc;
    (void)argv;

    g_status_handle = RegisterServiceCtrlHandlerA(SERVICE_NAME, service_ctrl_handler);
    if (!g_status_handle) {
        return;
    }

    memset(&g_status, 0, sizeof(g_status));
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    g_status.dwWaitHint = 1000;

    set_service_status(SERVICE_START_PENDING, 0, 1000);

    SmsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.port, DEFAULT_PORT, sizeof(cfg.port) - 1);
    cfg.baud = 9600;
    cfg.service_mode = 1;

    HANDLE thread = CreateThread(NULL, 0, service_worker_thread, &cfg, 0, NULL);
    if (thread != NULL) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }

    set_service_status(SERVICE_STOPPED, 0, 0);
}

static int run_as_service(void) {
    SERVICE_TABLE_ENTRYA table[] = {
        { SERVICE_NAME, service_main },
        { NULL, NULL }
    };
    return StartServiceCtrlDispatcherA(table) ? 0 : 1;
}
#endif

static int run_self_test(void) {
    SmsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.port, DEFAULT_PORT, sizeof(cfg.port) - 1);
    cfg.baud = 9600;
    cfg.service_mode = 0;

    log_message("Starting self-test sequence");
    log_message("Configuration: port=%s baud=%lu", cfg.port, cfg.baud);

    HANDLE hPort = open_port(&cfg);
    if (hPort == INVALID_HANDLE_VALUE) {
        log_message("Self-test failed: port open step failed");
        return 1;
    }

    if (!init_modem(hPort)) {
        log_message("Self-test failed: modem initialization failed");
        return 1;
    }

    char response[MAX_RESPONSE];
    memset(response, 0, sizeof(response));
    if (!send_sms(hPort, "+15551234567", "Self-test SMS", response, sizeof(response))) {
        log_message("Self-test failed: SMS send step failed");
        return 1;
    }

    if (!receive_sms(hPort, response, sizeof(response))) {
        log_message("Self-test failed: SMS receive step failed");
        return 1;
    }

    log_message("Self-test completed successfully");
    return 0;
}

int main(int argc, char **argv) {
    SmsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.port, DEFAULT_PORT, sizeof(cfg.port) - 1);
    cfg.baud = 9600;
    cfg.service_mode = 0;

#if defined(_WIN32)
    if (argc >= 2 && strcmp(argv[1], "--service") == 0) {
        return run_as_service();
    }
#endif

    if (argc >= 2 && strcmp(argv[1], "--self-test") == 0) {
        return run_self_test();
    }

    if (argc >= 4 && strcmp(argv[1], "--send") == 0) {
        HANDLE hPort = open_port(&cfg);
        if (hPort == INVALID_HANDLE_VALUE) {
            return 1;
        }

        if (!init_modem(hPort)) {
            return 1;
        }

        char response[MAX_RESPONSE];
        memset(response, 0, sizeof(response));
        int ok = send_sms(hPort, argv[2], argv[3], response, sizeof(response));
        if (ok) {
            log_message("SMS send result: %s", response);
        } else {
            log_message("SMS send failed.");
        }

        return ok ? 0 : 1;
    }

    if (argc >= 2 && strcmp(argv[1], "--receive") == 0) {
        HANDLE hPort = open_port(&cfg);
        if (hPort == INVALID_HANDLE_VALUE) {
            return 1;
        }

        if (!init_modem(hPort)) {
            return 1;
        }

        char response[MAX_RESPONSE];
        memset(response, 0, sizeof(response));
        if (receive_sms(hPort, response, sizeof(response))) {
            printf("%s\n", response);
        }

        return 0;
    }

    printf("Usage:\n");
    printf("  %s --self-test\n", argv[0]);
#if defined(_WIN32)
    printf("  %s --service\n", argv[0]);
#endif
    printf("  %s --send <phone> <message>\n", argv[0]);
    printf("  %s --receive\n", argv[0]);
    return 0;
}
