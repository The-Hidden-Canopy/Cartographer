#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <string.h>

static int echo_input(void) {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD process_id = GetCurrentProcessId();
    DWORD written = 0U;
    if (WriteFile(output, &process_id, sizeof(process_id), &written, NULL) == 0 ||
        written != sizeof(process_id)) {
        return 10;
    }

    uint8_t buffer[256U];
    for (;;) {
        DWORD read = 0U;
        if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer),
                &read, NULL) == 0) {
            return GetLastError() == ERROR_BROKEN_PIPE
                ? 0
                : (int)(1000U + GetLastError());
        }
        if (read == 0U) return 0;
        DWORD offset = 0U;
        while (offset < read) {
            written = 0U;
            if (WriteFile(output, buffer + offset, read - offset,
                    &written, NULL) == 0 || written == 0U) {
                return 12;
            }
            offset += written;
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (strcmp(argv[1], "--provider-child-echo") == 0) return echo_input();
    if (strcmp(argv[1], "--provider-child-crash") == 0) {
        (void)TerminateProcess(GetCurrentProcess(), 0xc0000000U);
        return 101;
    }
    if (strcmp(argv[1], "--provider-child-sleep") == 0) {
        Sleep(INFINITE);
        return 102;
    }
    if (strcmp(argv[1], "--provider-child-cpu") == 0) {
        volatile uint64_t counter = 0U;
        for (;;) ++counter;
    }
    if (strcmp(argv[1], "--provider-child-spam") == 0) {
        uint8_t buffer[4096U] = {0};
        for (;;) {
            DWORD written = 0U;
            if (WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, sizeof(buffer),
                    &written, NULL) == 0 || written == 0U) {
                return 0;
            }
        }
    }
    return 3;
}
