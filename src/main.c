#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <icmpapi.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define PING_TARGET   "1.1.1.1"
#define PING_TIMEOUT  1000   /* ms */
#define PING_EVERY    5      /* ticks between pings */

/* ---- helpers ---------------------------------------------------------- */

/* Convert "1.1.1.1" into an IPAddr. Returns 0 if the string is malformed.
   Built by hand so we don't need to link ws2_32 or call WSAStartup. */
static IPAddr parse_ipv4(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    return (IPAddr)(a | (b << 8) | (c << 16) | (d << 24));
}

/* Sum InOctets and OutOctets across all interfaces.
   Returns 0 on success, non-zero on failure. */
static int read_counters(ULONG64 *down, ULONG64 *up) {
    MIB_IF_TABLE2 *table = NULL;

    if (GetIfTable2(&table) != NO_ERROR)
        return 1;

    ULONG64 in = 0, out = 0;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        in  += table->Table[i].InOctets;
        out += table->Table[i].OutOctets;
    }

    FreeMibTable(table);

    *down = in;
    *up   = out;
    return 0;
}

/* Format a byte rate into a short human-readable string. */
void format_speed(double bytes_per_sec, char *out, size_t n) {
    if (bytes_per_sec >= 1024.0 * 1024.0)
        snprintf(out, n, "%.1f MB/s", bytes_per_sec / (1024.0 * 1024.0));
    else if (bytes_per_sec >= 1024.0)
        snprintf(out, n, "%.1f KB/s", bytes_per_sec / 1024.0);
    else
        snprintf(out, n, "%.0f B/s", bytes_per_sec);
}

/* Round-trip time in ms, or -1 on timeout / failure. Blocks up to PING_TIMEOUT. */
static int ping_ms(IPAddr dest) {
    if (dest == 0)
        return -1;

    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE)
        return -1;

    char payload[32] = "netspeed-probe";

    /* Reply buffer holds the reply struct, the echoed payload,
       and up to 8 bytes for an optional ICMP error message. */
    DWORD reply_size = sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8;
    char *reply_buf = malloc(reply_size);
    if (!reply_buf) {
        IcmpCloseHandle(h);
        return -1;
    }

    int result = -1;

    DWORD n = IcmpSendEcho(h, dest,
                           payload, (WORD)sizeof(payload),
                           NULL,
                           reply_buf, reply_size,
                           PING_TIMEOUT);

    if (n > 0) {
        ICMP_ECHO_REPLY *reply = (ICMP_ECHO_REPLY *)reply_buf;
        if (reply->Status == IP_SUCCESS)
            result = (int)reply->RoundTripTime;
    }

    free(reply_buf);
    IcmpCloseHandle(h);
    return result;
}

/* ---- main ------------------------------------------------------------- */

int main(void) {
    IPAddr ping_dest = parse_ipv4(PING_TARGET);

    ULONG64 prev_down = 0, prev_up = 0;

    if (read_counters(&prev_down, &prev_up) != 0) {
        printf("Failed to read interface table\n");
        return 1;
    }
    ULONGLONG prev_tick = GetTickCount64();

    int tick = 0;
    int latency = -1;

    for (;;) {
        Sleep(1000);

        ULONG64 cur_down, cur_up;
        if (read_counters(&cur_down, &cur_up) != 0)
            continue;

        ULONGLONG cur_tick = GetTickCount64();
        double elapsed = (double)(cur_tick - prev_tick) / 1000.0;
        if (elapsed <= 0.0)
            continue;

        ULONG64 d_delta = (cur_down > prev_down) ? cur_down - prev_down : 0;
        ULONG64 u_delta = (cur_up   > prev_up)   ? cur_up   - prev_up   : 0;

        prev_down = cur_down;
        prev_up   = cur_up;
        prev_tick = cur_tick;

        if (tick % PING_EVERY == 0)
            latency = ping_ms(ping_dest);
        tick++;

        char down_str[32], up_str[32], lat_str[16];
        format_speed((double)d_delta / elapsed, down_str, sizeof down_str);
        format_speed((double)u_delta / elapsed, up_str,   sizeof up_str);

        if (latency >= 0)
            snprintf(lat_str, sizeof lat_str, "%d ms", latency);
        else
            snprintf(lat_str, sizeof lat_str, "--");

        printf("\rDown: %-12s  Up: %-12s  Ping: %-8s",
               down_str, up_str, lat_str);
        fflush(stdout);
    }

    return 0;
}