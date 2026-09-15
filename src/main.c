#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <icmpapi.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PING_TARGET   "1.1.1.1"
#define PING_TIMEOUT  1000 // mili seconds
#define PING_EVERY    5 /* ticks between pings */

#define WM_TRAYICON   (WM_APP + 1)
#define TIMER_ID      1
#define TRAY_UID      1
#define ID_TRAY_EXIT   1001
#define ID_TRAY_RESET  1002
#define ID_TRAY_AUTOSTART 1003

#define HIST_LEN 16
#define RUN_KEY   "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_NAME  "NetSpeed"

static double g_hist_down[HIST_LEN];
static double g_hist_up[HIST_LEN];
static int    g_hist_pos;

static NOTIFYICONDATA g_nid;

static ULONG64   g_prev_down, g_prev_up;
static ULONGLONG g_prev_tick;
static int       g_tick;
static int       g_latency = -1;
static IPAddr    g_ping_dest;
static UINT g_taskbar_created;
static HICON g_current_icon = NULL;

static int  g_interval    = 1000;
static char g_ping_target[64] = PING_TARGET;
static int  g_ping_every  = PING_EVERY;

static int g_log_day = -1;
static ULONG64 g_day_down, g_day_up;


/* ---- helpers ---------------------------------------------------------- */

/* Map a byte rate onto 0.0-1.0 logarithmically.
   0 -> 0.0, 1 KB/s -> ~0.25, 1 MB/s -> ~0.75, 100 MB/s -> 1.0 */
static double scale_log(double bytes_per_sec) {
    if (bytes_per_sec < 1.0)
        return 0.0;

    double v = log10(bytes_per_sec) / 8.0;   /* 10^8 ~= 100 MB/s */
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return v;
}

/* Is autostart currently enabled? */
static int autostart_enabled(void) {
    HKEY key;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;

    LONG r = RegQueryValueEx(key, RUN_NAME, NULL, NULL, NULL, NULL);
    RegCloseKey(key);

    return (r == ERROR_SUCCESS);
}

/* Turn autostart on or off. Returns 0 on success. */
static int autostart_set(int enable) {
    HKEY key;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return 1;

    LONG r;

    if (enable) {
        char path[MAX_PATH];
        if (GetModuleFileName(NULL, path, MAX_PATH) == 0) {
            RegCloseKey(key);
            return 1;
        }

        /* Quote it — an unquoted path with spaces is both broken and a
           classic privilege-escalation vector. */
        char quoted[MAX_PATH + 2];
        snprintf(quoted, sizeof quoted, "\"%s\"", path);

        r = RegSetValueEx(key, RUN_NAME, 0, REG_SZ,
                          (const BYTE *)quoted,
                          (DWORD)(strlen(quoted) + 1));
    } else {
        r = RegDeleteValue(key, RUN_NAME);
    }

    RegCloseKey(key);
    return (r == ERROR_SUCCESS) ? 0 : 1;
}

/* Append yesterday's totals when the date rolls over. */
static void log_daily(ULONG64 d_delta, ULONG64 u_delta) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    if (g_log_day == -1) {
        g_log_day = st.wDay;
    } else if (st.wDay != g_log_day) {
        char path[MAX_PATH];
        if (GetModuleFileName(NULL, path, MAX_PATH) > 0) {
            char *slash = strrchr(path, '\\');
            if (slash) {
                snprintf(slash + 1, sizeof path - (slash + 1 - path),
                         "netspeed-usage.csv");

                FILE *f = fopen(path, "a");
                if (f) {
                    fprintf(f, "%04d-%02d-%02d,%llu,%llu\n",
                            st.wYear, st.wMonth, st.wDay,
                            (unsigned long long)g_day_down,
                            (unsigned long long)g_day_up);
                    fclose(f);
                }
            }
        }
        g_day_down = 0;
        g_day_up   = 0;
        g_log_day  = st.wDay;
    }

    g_day_down += d_delta;
    g_day_up   += u_delta;
}


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

/* Should this interface be counted toward the total? */
static int is_countable(const MIB_IF_ROW2 *row) {
    if (row->InterfaceAndOperStatusFlags.FilterInterface)  return 0;
    if (!row->InterfaceAndOperStatusFlags.HardwareInterface) return 0;
    if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK)            return 0;
    if (row->Type == IF_TYPE_TUNNEL)                       return 0;
    if (row->OperStatus != IfOperStatusUp)                 return 0;
    return 1;
}

/* Sum InOctets and OutOctets across genuinely active interfaces.
   Returns 0 on success, non-zero on failure. */
static int read_counters(ULONG64 *down, ULONG64 *up) {
    MIB_IF_TABLE2 *table = NULL;

    if (GetIfTable2(&table) != NO_ERROR)
        return 1;

    ULONG64 in = 0, out = 0;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        MIB_IF_ROW2 *row = &table->Table[i];
        if (!is_countable(row))
            continue;
        in  += row->InOctets;
        out += row->OutOctets;
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

/* Print every interface with its type, status, and totals. */

static const char *type_name(ULONG type) {
    switch (type) {
        case IF_TYPE_ETHERNET_CSMACD:    return "Ethernet";
        case IF_TYPE_IEEE80211:          return "Wi-Fi";
        case IF_TYPE_SOFTWARE_LOOPBACK:  return "Loopback";
        case IF_TYPE_TUNNEL:             return "Tunnel";
        case IF_TYPE_PPP:                return "PPP";
        default:                         return "Other";
    }
}

static void list_interfaces(void) {
    MIB_IF_TABLE2 *table = NULL;

    if (GetIfTable2(&table) != NO_ERROR) {
        printf("Failed to read interface table\n");
        return;
    }

    printf("\n  %-32s  %-9s  %-6s  %10s  %10s\n",
           "Alias", "Type", "Status", "In (MB)", "Out (MB)");
    printf("  %.32s  %.9s  %.6s  %.10s  %.10s\n",
           "--------------------------------",
           "---------", "------", "----------", "----------");

    int shown = 0;

    /* Counted interfaces first, then the rest. */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)
            printf("\n  -- not counted --\n");

        for (ULONG i = 0; i < table->NumEntries; i++) {
            MIB_IF_ROW2 *row = &table->Table[i];
            int counted = is_countable(row);

            if (counted != (pass == 0))
                continue;

            /* In the second pass, hide the noise: filter layers and
               adapters that have never carried a byte. */
            if (pass == 1) {
                if (row->InterfaceAndOperStatusFlags.FilterInterface) continue;
                if (row->InOctets == 0 && row->OutOctets == 0)        continue;
            }

            printf("%s %-32.32ls  %-9s  %-6s  %10.1f  %10.1f\n",
                   counted ? ">" : " ",
                   row->Alias,
                   type_name(row->Type),
                   (row->OperStatus == IfOperStatusUp) ? "up" : "down",
                   (double)row->InOctets  / (1024.0 * 1024.0),
                   (double)row->OutOctets / (1024.0 * 1024.0));

            if (counted) shown++;
        }
    }

    printf("\n  %lu interfaces total, %d counted\n\n",
           (unsigned long)table->NumEntries, shown);

    FreeMibTable(table);
}

/* ---- tray -------------------------------------------------------------- */

static void tray_add(HWND hwnd) {
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize           = sizeof g_nid;
    g_nid.hWnd             = hwnd;
    g_nid.uID              = TRAY_UID;
    g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(g_nid.szTip, sizeof g_nid.szTip, "NetSpeed: starting...");

    Shell_NotifyIcon(NIM_ADD, &g_nid);
}


/* Build a sparkline icon from the history buffers. */
static HICON make_icon(void) {
    int w = GetSystemMetrics(SM_CXSMICON);
    int h = GetSystemMetrics(SM_CYSMICON);

    HDC screen = GetDC(NULL);
    HDC mem    = CreateCompatibleDC(screen);

    HBITMAP color = CreateCompatibleBitmap(screen, w, h);
    HBITMAP mask  = CreateBitmap(w, h, 1, 1, NULL);

    ReleaseDC(NULL, screen);

    HBITMAP old_bmp = SelectObject(mem, color);

    /* Background */
    RECT full = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 16));
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    int mid = h / 2;

    HBRUSH down_brush = CreateSolidBrush(RGB(80, 220, 120));   /* green */
    HBRUSH up_brush   = CreateSolidBrush(RGB(255, 160, 60));   /* orange */

    /* One column per pixel of width, oldest on the left. */
    for (int x = 0; x < w; x++) {
        int idx = (g_hist_pos + x) % HIST_LEN;

        int dh = (int)(scale_log(g_hist_down[idx]) * mid);
        int uh = (int)(scale_log(g_hist_up[idx])   * (h - mid - 1));

        if (dh > 0) {
            RECT bar = { x, mid - dh, x + 1, mid };
            FillRect(mem, &bar, down_brush);
        }
        if (uh > 0) {
            RECT bar = { x, mid + 1, x + 1, mid + 1 + uh };
            FillRect(mem, &bar, up_brush);
        }
    }

    DeleteObject(down_brush);
    DeleteObject(up_brush);

    /* Midline separator */
    HBRUSH line = CreateSolidBrush(RGB(70, 70, 70));
    RECT mid_rect = { 0, mid, w, mid + 1 };
    FillRect(mem, &mid_rect, line);
    DeleteObject(line);

    SelectObject(mem, old_bmp);

    ICONINFO ii;
    memset(&ii, 0, sizeof ii);
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;

    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(mem);

    return icon;
}


/* One poll: refresh counters, latency, tooltip, and the icon itself. */
static void tray_update(void) {
    ULONG64 cur_down, cur_up;
    if (read_counters(&cur_down, &cur_up) != 0)
        return;

    ULONGLONG cur_tick = GetTickCount64();
    double elapsed = (double)(cur_tick - g_prev_tick) / 1000.0;
    if (elapsed <= 0.0)
        return;

    ULONG64 d_delta = (cur_down > g_prev_down) ? cur_down - g_prev_down : 0;
    ULONG64 u_delta = (cur_up   > g_prev_up)   ? cur_up   - g_prev_up   : 0;

    log_daily(d_delta, u_delta);

    g_prev_down = cur_down;
    g_prev_up   = cur_up;
    g_prev_tick = cur_tick;

    if (g_tick % g_ping_every == 0)
        g_latency = ping_ms(g_ping_dest);
    g_tick++;

    char down_str[32], up_str[32];
    format_speed((double)d_delta / elapsed, down_str, sizeof down_str);
    format_speed((double)u_delta / elapsed, up_str,   sizeof up_str);

    if (g_latency >= 0)
        snprintf(g_nid.szTip, sizeof g_nid.szTip,
                 "Down: %s\nUp: %s\nPing: %d ms", down_str, up_str, g_latency);
    else
        snprintf(g_nid.szTip, sizeof g_nid.szTip,
                 "Down: %s\nUp: %s\nPing: --", down_str, up_str);

    /* Push this second's readings into the ring buffer. */
    g_hist_down[g_hist_pos] = (double)d_delta / elapsed;
    g_hist_up[g_hist_pos]   = (double)u_delta / elapsed;
    g_hist_pos = (g_hist_pos + 1) % HIST_LEN;

    HICON new_icon = make_icon();

    if (new_icon) {
        HICON old_icon = g_current_icon;

        g_nid.hIcon  = new_icon;
        g_nid.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIcon(NIM_MODIFY, &g_nid);

        g_current_icon = new_icon;

        /* Destroy only after the shell has taken the new one. */
        if (old_icon)
            DestroyIcon(old_icon);
    } else {
        /* Icon creation failed — still update the tooltip. */
        g_nid.uFlags = NIF_TIP;
        Shell_NotifyIcon(NIM_MODIFY, &g_nid);
    }
}

static void show_tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenu(menu, MF_STRING, ID_TRAY_RESET, "Reset counters");

    AppendMenu(menu, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0),
               ID_TRAY_AUTOSTART, "Start with Windows");
    
    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(menu, MF_STRING, ID_TRAY_EXIT,  "Exit");

    /* Required workaround — see note below. */
    SetForegroundWindow(hwnd);

    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);

    PostMessage(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

/* ---- window ------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {

    /* Explorer restarted and rebuilt the taskbar — re-add our icon.
       Can't be a switch case: g_taskbar_created is set at runtime,
       and case labels must be compile-time constants. */
    if (msg == g_taskbar_created) {
        tray_add(hwnd);
        return 0;
    }

    switch (msg) {

    case WM_TIMER:
        if (wp == TIMER_ID)
            tray_update();
        return 0;

    case WM_TRAYICON:
        /* For tray callbacks the mouse event arrives in lParam;
           wParam holds the icon's uID. */
        if (lp == WM_RBUTTONUP)
            show_tray_menu(hwnd);
        else if (lp == WM_LBUTTONDBLCLK)
            DestroyWindow(hwnd);
        return 0;

    case WM_COMMAND:
        /* High word of wParam is the notification code, so mask it off. */
        switch (LOWORD(wp)) {

        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;

        case ID_TRAY_RESET:
            /* Re-baseline so the next tick measures from now. */
            read_counters(&g_prev_down, &g_prev_up);
            g_prev_tick = GetTickCount64();
            g_tick      = 0;
            break;
        case ID_TRAY_AUTOSTART:
            autostart_set(!autostart_enabled());
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        Shell_NotifyIcon(NIM_DELETE, &g_nid);
        if (g_current_icon)
            DestroyIcon(g_current_icon);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

/* Read key=value lines from netspeed.cfg beside the exe.
   Missing file is fine — defaults stay. */
static void load_config(void) {
    char path[MAX_PATH];
    if (GetModuleFileName(NULL, path, MAX_PATH) == 0)
        return;

    char *slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, sizeof path - (slash + 1 - path), "netspeed.cfg");

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;

        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) != 2)
            continue;

        if (strcmp(key, "interval_ms") == 0) {
            int v = atoi(val);
            if (v >= 200 && v <= 60000) g_interval = v;
        } else if (strcmp(key, "ping_target") == 0) {
            snprintf(g_ping_target, sizeof g_ping_target, "%s", val);
        } else if (strcmp(key, "ping_every") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 60) g_ping_every = v;
        }
    }

    fclose(f);
}


/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        list_interfaces();
        return 0;
    }

    load_config();
    g_ping_dest = parse_ipv4(g_ping_target);

    if (read_counters(&g_prev_down, &g_prev_up) != 0) {
        printf("Failed to read interface table\n");
        return 1;
    }
    g_prev_tick = GetTickCount64();

    HINSTANCE inst = GetModuleHandle(NULL);

    WNDCLASSEX wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = "NetSpeedWndClass";

    if (!RegisterClassEx(&wc)) {
        printf("RegisterClassEx failed (%lu)\n", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowEx(0, "NetSpeedWndClass", "NetSpeed",
                               0, 0, 0, 0, 0,
                               NULL, NULL, inst, NULL);
    if (!hwnd) {
        printf("CreateWindowEx failed (%lu)\n", GetLastError());
        return 1;
    }
    /* Deliberately no ShowWindow — the window exists only for messages. */

    g_taskbar_created = RegisterWindowMessage(TEXT("TaskbarCreated"));
    tray_add(hwnd);
    SetTimer(hwnd, TIMER_ID, g_interval, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}