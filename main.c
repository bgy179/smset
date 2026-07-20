#if defined(_WIN32)
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "sms_modem.h"

#define DEFAULT_PORT "COM3"
#define DEFAULT_BAUD 9600
#define SERVICE_NAME "smset"

typedef struct {
    char port[32];
    unsigned long baud;
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

static const char *modem_result_str(int rc) {
    switch (rc) {
        case SMS_MODEM_OK:            return "OK";
        case SMS_MODEM_INVALID_INPUT: return "invalid input";
        case SMS_MODEM_PORT_ERROR:    return "port open/config error";
        case SMS_MODEM_WRITE_ERROR:   return "serial write error";
        case SMS_MODEM_READ_TIMEOUT:  return "read timeout";
        case SMS_MODEM_MODEM_ERROR:   return "modem returned ERROR";
        case SMS_MODEM_NO_PROMPT:     return "no '>' prompt";
        case SMS_MODEM_NO_CONFIRM:    return "no +CMGS confirmation";
        case SMS_MODEM_INIT_FAILED:   return "modem init failed";
        default:                      return "unknown error";
    }
}

#if defined(_WIN32)
static int run_sms_loop(const SmsConfig *cfg) {
    sms_modem_port_t port;
    int rc = sms_modem_open(cfg->port, cfg->baud, &port);
    if (rc != SMS_MODEM_OK) {
        log_message("Failed to open port %s: %s", cfg->port, modem_result_str(rc));
        return 1;
    }

    rc = sms_modem_init(port);
    if (rc != SMS_MODEM_OK) {
        log_message("Modem initialization failed: %s", modem_result_str(rc));
        sms_modem_close(port);
        return 1;
    }

    log_message("Monitoring SMS on %s. Press Ctrl+C to stop.", cfg->port);

    while (g_running) {
        char response[SMS_MODEM_RESPONSE_MAX];
        memset(response, 0, sizeof(response));

        rc = sms_modem_receive(port, response, sizeof(response));
        if (rc == SMS_MODEM_OK && response[0] != '\0') {
            log_message("Incoming SMS data:\n%s", response);
        }

        Sleep(5000);
    }

    log_message("Stopping SMS monitoring loop.");
    sms_modem_close(port);
    return 0;
}
#endif

#if defined(_WIN32)
static void set_service_status(DWORD state, DWORD exit_code, DWORD wait_hint) {
    g_status.dwCurrentState  = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint      = wait_hint;
    g_status.dwCheckPoint    =
        (state == SERVICE_RUNNING || state == SERVICE_START_PENDING) ? 1 : 0;
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
    if (!g_status_handle) return;

    memset(&g_status, 0, sizeof(g_status));
    g_status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_status.dwCurrentState     = SERVICE_START_PENDING;
    g_status.dwWaitHint         = 1000;
    set_service_status(SERVICE_START_PENDING, 0, 1000);

    SmsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.port, DEFAULT_PORT, sizeof(cfg.port) - 1);
    cfg.baud         = DEFAULT_BAUD;
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

static int run_self_test(const SmsConfig *cfg) {
    log_message("Starting self-test: port=%s baud=%lu", cfg->port, cfg->baud);

    sms_modem_port_t port;
    int rc = sms_modem_open(cfg->port, cfg->baud, &port);
    if (rc != SMS_MODEM_OK) {
        log_message("Self-test failed (open): %s", modem_result_str(rc));
        return 1;
    }

    rc = sms_modem_init(port);
    if (rc != SMS_MODEM_OK) {
        log_message("Self-test failed (init): %s", modem_result_str(rc));
        sms_modem_close(port);
        return 1;
    }

    char response[SMS_MODEM_RESPONSE_MAX];

    rc = sms_modem_send(port, "+15551234567", "Self-test SMS",
                        response, sizeof(response));
    if (rc != SMS_MODEM_OK) {
        log_message("Self-test failed (send): %s", modem_result_str(rc));
        sms_modem_close(port);
        return 1;
    }

    rc = sms_modem_receive(port, response, sizeof(response));
    if (rc != SMS_MODEM_OK) {
        log_message("Self-test failed (receive): %s", modem_result_str(rc));
        sms_modem_close(port);
        return 1;
    }

    log_message("Self-test completed successfully.");
    sms_modem_close(port);
    return 0;
}

int main(int argc, char **argv) {
    SmsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.port, DEFAULT_PORT, sizeof(cfg.port) - 1);
    cfg.baud         = DEFAULT_BAUD;
    cfg.service_mode = 0;

#if defined(_WIN32)
    if (argc >= 2 && strcmp(argv[1], "--service") == 0)
        return run_as_service();
#endif

    if (argc >= 2 && strcmp(argv[1], "--self-test") == 0)
        return run_self_test(&cfg);

    if (argc >= 4 && strcmp(argv[1], "--send") == 0) {
        sms_modem_port_t port;
        int rc = sms_modem_open(cfg.port, cfg.baud, &port);
        if (rc != SMS_MODEM_OK) {
            log_message("Failed to open port: %s", modem_result_str(rc));
            return 1;
        }
        rc = sms_modem_init(port);
        if (rc != SMS_MODEM_OK) {
            log_message("Modem init failed: %s", modem_result_str(rc));
            sms_modem_close(port);
            return 1;
        }
        char response[SMS_MODEM_RESPONSE_MAX];
        rc = sms_modem_send(port, argv[2], argv[3], response, sizeof(response));
        if (rc == SMS_MODEM_OK)
            log_message("SMS sent. Modem reply: %s", response);
        else
            log_message("SMS send failed: %s", modem_result_str(rc));
        sms_modem_close(port);
        return (rc == SMS_MODEM_OK) ? 0 : 1;
    }

    if (argc >= 2 && strcmp(argv[1], "--receive") == 0) {
        sms_modem_port_t port;
        int rc = sms_modem_open(cfg.port, cfg.baud, &port);
        if (rc != SMS_MODEM_OK) {
            log_message("Failed to open port: %s", modem_result_str(rc));
            return 1;
        }
        rc = sms_modem_init(port);
        if (rc != SMS_MODEM_OK) {
            log_message("Modem init failed: %s", modem_result_str(rc));
            sms_modem_close(port);
            return 1;
        }
        char response[SMS_MODEM_RESPONSE_MAX];
        rc = sms_modem_receive(port, response, sizeof(response));
        if (rc == SMS_MODEM_OK)
            printf("%s\n", response);
        else
            log_message("Receive failed: %s", modem_result_str(rc));
        sms_modem_close(port);
        return (rc == SMS_MODEM_OK) ? 0 : 1;
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
