/* Host replacement for the title's six gamepad XPP entry points. The title's
 * own input mapping, video-skip logic and menu state machine remain in charge. */
#include "conker_input.h"
#include "input_queue.h"
#include "recomp_types.h"
#include <windows.h>
#include <stdio.h>

#define GAMEPAD_DEVICE_TYPE 0x00558454u
#define PAD_HANDLE_BASE 0x58490000u
static CRITICAL_SECTION input_lock;
static ConkerInputQueue pads[4];
static unsigned connected, inserted, removed, opened;
static unsigned packet[4];
static int initialized;
static int discovery_started;

static int focused(void)
{
    DWORD pid = 0;
    HWND window = GetForegroundWindow();
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

static void sample(void)
{
    XBOX_INPUT_STATE states[4] = {0};
    unsigned mask = 0, keyboard_port = 0;
    int active = focused();
    for (unsigned port = 0; port < 4; ++port)
        if (xbox_InputGetState(port, &states[port]) == ERROR_SUCCESS)
            mask |= 1u << port;
    if (mask) {
        while (!(mask & (1u << keyboard_port))) ++keyboard_port;
    } else {
        /* A keyboard acts as player one when no physical pad is connected. */
        mask = 1;
    }
    if (!active) memset(states, 0, sizeof(states));
    if (active) {
        XBOX_GAMEPAD *p = &states[keyboard_port].Gamepad;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) p->wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) p->wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
        if (GetAsyncKeyState(VK_UP) & 0x8000) p->wButtons |= XBOX_GAMEPAD_DPAD_UP;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) p->wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
        if ((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_SPACE)) & 0x8000)
            p->bAnalogButtons[XBOX_BUTTON_A] = 255;
        if ((GetAsyncKeyState(VK_ESCAPE) | GetAsyncKeyState(VK_BACK)) & 0x8000)
            p->bAnalogButtons[XBOX_BUTTON_B] = 255;
    }
    EnterCriticalSection(&input_lock);
    inserted |= mask & ~connected;
    removed |= connected & ~mask;
    if (mask != connected)
        fprintf(stderr, "[INPUT] Available gamepad ports mask=%X (keyboard fallback when no pad)\n", mask);
    connected = mask;
    for (unsigned port = 0; port < 4; ++port) {
        if (!(mask & (1u << port)) || !active) {
            /* Disconnect/focus loss must discard pending presses immediately. */
            memset(&pads[port], 0, sizeof(pads[port]));
        }
        conker_input_queue_sample(&pads[port], &states[port]);
    }
    LeaveCriticalSection(&input_lock);
}

static DWORD WINAPI input_worker(void *unused)
{
    (void)unused;
    for (;;) { sample(); Sleep(8); }
}

void conker_input_init(void)
{
    if (initialized) return;
    InitializeCriticalSection(&input_lock);
    initialized = 1;
    sample();
    HANDLE thread = CreateThread(NULL, 0, input_worker, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    else fprintf(stderr, "[INPUT] Failed to start input sampler: %lu\n", GetLastError());
}

static int finish(unsigned result, unsigned argument_bytes)
{
    g_eax = result;
    g_esp += 4 + argument_bytes;
    return 1;
}

static int port_for_handle(unsigned handle)
{
    return handle >= PAD_HANDLE_BASE && handle < PAD_HANDLE_BASE + 4
        ? (int)(handle - PAD_HANDLE_BASE) : -1;
}

int conker_input_get_devices(void)
{
    if (MEM32(g_esp + 4) != GAMEPAD_DEVICE_TYPE) return 0;
    EnterCriticalSection(&input_lock);
    unsigned mask = connected;
    if (!discovery_started) {
        /* Start device discovery asynchronously, as the guest USB stack does.
         * Conker allocates its final input records after the startup query;
         * an immediate connection there is lost when it replaces the buffer.
         * Deliver those initial arrivals through the first change poll. */
        discovery_started = 1;
        mask = 0;
        inserted = connected;
        removed = 0;
    } else {
        inserted = removed = 0;
    }
    LeaveCriticalSection(&input_lock);
    return finish(mask, 4);
}

int conker_input_get_changes(void)
{
    if (MEM32(g_esp + 4) != GAMEPAD_DEVICE_TYPE) return 0;
    EnterCriticalSection(&input_lock);
    discovery_started = 1;
    unsigned add = inserted, drop = removed;
    inserted = removed = 0;
    LeaveCriticalSection(&input_lock);
    MEM32(MEM32(g_esp + 8)) = add;
    MEM32(MEM32(g_esp + 12)) = drop;
    return finish((add | drop) != 0, 12);
}

int conker_input_open(void)
{
    if (MEM32(g_esp + 4) != GAMEPAD_DEVICE_TYPE) return 0;
    unsigned port = MEM32(g_esp + 8), slot = MEM32(g_esp + 12), result = 0;
    EnterCriticalSection(&input_lock);
    if (port < 4 && slot == 0 && (connected & (1u << port))) {
        opened |= 1u << port;
        memset(&pads[port], 0, sizeof(pads[port]));
        result = PAD_HANDLE_BASE + port;
        fprintf(stderr, "[INPUT] Guest opened controller port %u\n", port);
    }
    LeaveCriticalSection(&input_lock);
    return finish(result, 16);
}

int conker_input_close(void)
{
    int port = port_for_handle(MEM32(g_esp + 4));
    if (port < 0) return 0;
    EnterCriticalSection(&input_lock);
    opened &= ~(1u << port);
    memset(&pads[port], 0, sizeof(pads[port]));
    LeaveCriticalSection(&input_lock);
    XBOX_VIBRATION stop = {0};
    xbox_InputSetState((DWORD)port, &stop);
    return finish(0, 4);
}

int conker_input_get_state(void)
{
    int port = port_for_handle(MEM32(g_esp + 4));
    if (port < 0) return 0;
    unsigned out = MEM32(g_esp + 8), result = ERROR_DEVICE_NOT_CONNECTED;
    XBOX_INPUT_STATE s = {0};
    EnterCriticalSection(&input_lock);
    if (connected & opened & (1u << port)) {
        conker_input_queue_read(&pads[port], &s);
        s.dwPacketNumber = ++packet[port];
        result = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&input_lock);
    /* Retail XINPUT_STATE is packed: 22 bytes, not the host C struct's 24. */
    MEM32(out) = s.dwPacketNumber;
    MEM16(out + 4) = s.Gamepad.wButtons;
    for (unsigned i = 0; i < 8; ++i) MEM8(out + 6 + i) = s.Gamepad.bAnalogButtons[i];
    MEM16(out + 14) = (uint16_t)s.Gamepad.sThumbLX;
    MEM16(out + 16) = (uint16_t)s.Gamepad.sThumbLY;
    MEM16(out + 18) = (uint16_t)s.Gamepad.sThumbRX;
    MEM16(out + 20) = (uint16_t)s.Gamepad.sThumbRY;
    if (s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] || s.Gamepad.wButtons & 12 ||
        s.Gamepad.sThumbLX < -16000 || s.Gamepad.sThumbLX > 16000) {
        static unsigned count;
        if (count++ < 24) fprintf(stderr, "[INPUT] Guest state port=%d buttons=%04X A=%u LX=%d LY=%d\n",
            port, s.Gamepad.wButtons, s.Gamepad.bAnalogButtons[XBOX_BUTTON_A],
            s.Gamepad.sThumbLX, s.Gamepad.sThumbLY);
    }
    return finish(result, 8);
}

int conker_input_set_state(void)
{
    int port = port_for_handle(MEM32(g_esp + 4));
    if (port < 0) return 0;
    unsigned feedback = MEM32(g_esp + 8);
    /* Retail feedback has status +0, event +4, reserved[58], motors +66/+68. */
    XBOX_VIBRATION vibration = {MEM16(feedback + 66), MEM16(feedback + 68)};
    unsigned result = xbox_InputSetState((DWORD)port, &vibration);
    /* The keyboard's virtual pad has no motors. Complete synchronously. */
    if (result == ERROR_DEVICE_NOT_CONNECTED && port == 0) result = ERROR_SUCCESS;
    MEM32(feedback) = result;
    return finish(result, 8);
}
