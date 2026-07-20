#include "sms_modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* ============================================================
 * Windows implementation
 * ============================================================ */

int sms_modem_open(const char *port, unsigned long baud,
                   sms_modem_port_t *out_port)
{
    if (port == NULL || out_port == NULL)
        return SMS_MODEM_INVALID_INPUT;

    /* Build the extended device path so COM10+ works. */
    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return SMS_MODEM_PORT_ERROR;

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return SMS_MODEM_PORT_ERROR;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return SMS_MODEM_PORT_ERROR;
    }

    COMMTIMEOUTS to;
    to.ReadIntervalTimeout         = 50;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 5000;  /* 5 s total read timeout */
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 5000;
    SetCommTimeouts(h, &to);

    *out_port = h;
    return SMS_MODEM_OK;
}

void sms_modem_close(sms_modem_port_t port)
{
    if (port != INVALID_HANDLE_VALUE)
        CloseHandle(port);
}

static int port_write(sms_modem_port_t port, const char *buf, size_t len)
{
    DWORD written;
    if (!WriteFile(port, buf, (DWORD)len, &written, NULL))
        return SMS_MODEM_WRITE_ERROR;
    return (written == (DWORD)len) ? SMS_MODEM_OK : SMS_MODEM_WRITE_ERROR;
}

/*
 * Reads bytes from the serial port until `terminator` appears in the
 * accumulated buffer, or the buffer is full, or a timeout occurs.
 *
 * Returns SMS_MODEM_OK on success, SMS_MODEM_READ_TIMEOUT if the terminator
 * is never seen, or SMS_MODEM_MODEM_ERROR if "ERROR" appears in the response.
 */
static int port_read_until(sms_modem_port_t port,
                            const char *terminator,
                            char *buf, size_t buf_size)
{
    size_t total = 0;
    DWORD  got;
    memset(buf, 0, buf_size);

    while (total < buf_size - 1) {
        if (!ReadFile(port, buf + total, 1, &got, NULL) || got == 0)
            return SMS_MODEM_READ_TIMEOUT;
        total++;
        buf[total] = '\0';
        if (strstr(buf, terminator)) return SMS_MODEM_OK;
        if (strstr(buf, "ERROR"))    return SMS_MODEM_MODEM_ERROR;
    }
    return SMS_MODEM_READ_TIMEOUT;
}

#else /* POSIX */
/* ============================================================
 * POSIX implementation (Linux / macOS)
 * ============================================================ */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <time.h>

int sms_modem_open(const char *port, unsigned long baud,
                   sms_modem_port_t *out_port)
{
    if (port == NULL || out_port == NULL)
        return SMS_MODEM_INVALID_INPUT;

    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0)
        return SMS_MODEM_PORT_ERROR;

    /* Switch to blocking mode. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return SMS_MODEM_PORT_ERROR;
    }

    /* Map the numeric baud rate to a speed_t constant. */
    speed_t speed;
    switch (baud) {
        case 9600:   speed = B9600;   break;
        case 19200:  speed = B19200;  break;
        case 38400:  speed = B38400;  break;
        case 57600:  speed = B57600;  break;
        case 115200: speed = B115200; break;
        default:
            close(fd);
            return SMS_MODEM_INVALID_INPUT;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |=  CLOCAL | CREAD;
    tty.c_lflag  = 0;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tty.c_oflag  = 0;

    /* VMIN=0, VTIME=50 (5 seconds). */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 50;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return SMS_MODEM_PORT_ERROR;
    }

    tcflush(fd, TCIOFLUSH);
    *out_port = fd;
    return SMS_MODEM_OK;
}

void sms_modem_close(sms_modem_port_t port)
{
    if (port >= 0)
        close(port);
}

static int port_write(sms_modem_port_t port, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(port, buf + sent, len - sent);
        if (n < 0) return SMS_MODEM_WRITE_ERROR;
        sent += (size_t)n;
    }
    return SMS_MODEM_OK;
}

static int port_read_until(sms_modem_port_t port,
                            const char *terminator,
                            char *buf, size_t buf_size)
{
    size_t total = 0;
    memset(buf, 0, buf_size);

    while (total < buf_size - 1) {
        ssize_t n = read(port, buf + total, 1);
        if (n < 0)  return SMS_MODEM_READ_TIMEOUT;
        if (n == 0) return SMS_MODEM_READ_TIMEOUT;  /* VTIME expired */
        total++;
        buf[total] = '\0';
        if (strstr(buf, terminator)) return SMS_MODEM_OK;
        if (strstr(buf, "ERROR"))    return SMS_MODEM_MODEM_ERROR;
    }
    return SMS_MODEM_READ_TIMEOUT;
}

#endif /* _WIN32 / POSIX */

/* ============================================================
 * Platform-independent AT helpers
 * ============================================================ */

/*
 * Sends an AT command (appends "\r\n") then waits for `expect` in the reply.
 */
static int at_cmd(sms_modem_port_t port,
                  const char *cmd, const char *expect,
                  char *resp, size_t resp_size)
{
    char line[256];
    snprintf(line, sizeof(line), "%s\r\n", cmd);

    int rc = port_write(port, line, strlen(line));
    if (rc != SMS_MODEM_OK) return rc;

    return port_read_until(port, expect, resp, resp_size);
}

/* ============================================================
 * Public API
 * ============================================================ */

int sms_modem_init(sms_modem_port_t port)
{
#if defined(_WIN32)
    if (port == INVALID_HANDLE_VALUE) return SMS_MODEM_INVALID_INPUT;
#else
    if (port < 0) return SMS_MODEM_INVALID_INPUT;
#endif

    char resp[SMS_MODEM_RESPONSE_MAX];

    /* 1. Connectivity check. */
    if (at_cmd(port, "AT", "OK", resp, sizeof(resp)) != SMS_MODEM_OK)
        return SMS_MODEM_INIT_FAILED;

    /* 2. Switch to text mode. */
    if (at_cmd(port, "AT+CMGF=1", "OK", resp, sizeof(resp)) != SMS_MODEM_OK)
        return SMS_MODEM_INIT_FAILED;

    /* 3. Enable new-message notifications (route directly to TE). */
    if (at_cmd(port, "AT+CNMI=2,1,0,0,0", "OK", resp, sizeof(resp)) != SMS_MODEM_OK)
        return SMS_MODEM_INIT_FAILED;

    return SMS_MODEM_OK;
}

int sms_modem_send(sms_modem_port_t port, const char *phone,
                   const char *message, char *response, size_t response_size)
{
#if defined(_WIN32)
    if (port == INVALID_HANDLE_VALUE) return SMS_MODEM_INVALID_INPUT;
#else
    if (port < 0) return SMS_MODEM_INVALID_INPUT;
#endif
    if (phone == NULL || message == NULL)
        return SMS_MODEM_INVALID_INPUT;
    if (response == NULL || response_size == 0)
        return SMS_MODEM_INVALID_INPUT;

    /* Basic sanity: reject characters that would break the AT command. */
    if (strchr(phone, '"') || strchr(phone, '\r') || strchr(phone, '\n'))
        return SMS_MODEM_INVALID_INPUT;

    /* Step 1 – AT+CMGS="<phone>"\r\n  →  wait for '>' prompt. */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", phone);

    int rc = port_write(port, cmd, strlen(cmd));
    if (rc != SMS_MODEM_OK) return rc;

    char prompt[64];
    if (port_read_until(port, ">", prompt, sizeof(prompt)) != SMS_MODEM_OK)
        return SMS_MODEM_NO_PROMPT;

    /* Step 2 – message text  + SUB (0x1A) to send. */
    rc = port_write(port, message, strlen(message));
    if (rc != SMS_MODEM_OK) return rc;

    const char sub[2] = { 0x1A, '\0' };
    rc = port_write(port, sub, 1);
    if (rc != SMS_MODEM_OK) return rc;

    /* Step 3 – wait for +CMGS confirmation followed by OK. */
    if (port_read_until(port, "+CMGS", response, response_size) != SMS_MODEM_OK)
        return SMS_MODEM_NO_CONFIRM;

    return SMS_MODEM_OK;
}

int sms_modem_receive(sms_modem_port_t port, char *response,
                      size_t response_size)
{
#if defined(_WIN32)
    if (port == INVALID_HANDLE_VALUE) return SMS_MODEM_INVALID_INPUT;
#else
    if (port < 0) return SMS_MODEM_INVALID_INPUT;
#endif
    if (response == NULL || response_size == 0)
        return SMS_MODEM_INVALID_INPUT;

    return at_cmd(port, "AT+CMGL=\"ALL\"", "OK", response, response_size);
}
