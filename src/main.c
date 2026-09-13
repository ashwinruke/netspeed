#define _WIN32_WINNT 0x0601 // Targeting Windows 7 or later

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>

int main(void) {
    MIB_IF_TABLE2 *table = NULL;

    if (GetIfTable2(&table) != NO_ERROR) {
        printf("GetIfTable2 failed\n");
        return 1;
    }

    printf("Toolchain OK. Found %lu network interfaces.\n",
           (unsigned long)table->NumEntries);

    FreeMibTable(table);
    return 0;
}