/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (from tools.xbe_parser, Lethal Shot Xbox retail build):
 *   Title:       Lethal Shot (Xbox, 2002)
 *   Debug path:  C:\<title>\Release\<title>.exe
 *   Title ID:    0x54430004
 *   Base addr:   0x00010000
 *   Entry point: 0x000DAB5D
 *   Image size:  0x00395DC0 (3.59 MB), .text = 0xD5B80 (~874 KB)
 *   Sections:    11 (.text, D3D, D3DX, XGRPH, DSOUND, WMVDEC, XPP, .rdata,
 *                .data, DOLBY, $$XTIMAGE)
 *   XDK:         XAPILIB/D3D8/D3DX8/XGRAPHC/DSOUND/WMVDEC/XBOXKRNL/LIBCMT,
 *                all 1.0.4721
 *   Kernel imports: 103
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/* ── NV2A register model ────────────────────────────────────
 *
 * The XDK D3D8 library is linked into this XBE (section "D3D"), so the
 * game's device creation runs as recompiled code that talks to the NV2A
 * GPU through its register aperture at 0xFD000000. The runtime maps that
 * aperture as plain zeroed RAM ("no register semantics"), which is enough
 * for titles that only write push buffers -- but Lethal Shot's D3D8 lib
 * *reads* the register file during device bring-up (263 reads of
 * NV_PMC_BOOT_0 alone, plus PFB/PRAMDAC/PFIFO status) and never writes it.
 * Every read answers 0, the library's GPU-identity check fails, and its
 * DMA channel is never initialised: dev->[0x44] (the consumed position)
 * stays 0 while dev->[0] (the ring write pointer) is 0x80002000, so the
 * game spins forever in the pushbuffer wait at 0x000EECD6.
 *
 * The toolkit ships the register model (src/nv2a, with hardware-
 * NV_PMC_BOOT_0 -> 0x02A000A3, PFIFO/PFB/PGRAPH/PRAMDAC). It is
 * not installed by default -- the game project installs it. Route the
 * aperture's accesses into the model by trapping the pages and decoding
 * the faulting instruction, exactly as the toolkit's MMIO hook documents.
 */
#include "nv2a/nv2a_state.h"
#include "nv2a/nv2a_mmio_hook.h"

/* Aperture geometry, mirroring the runtime's own constants (it keeps them
 * private in xbox_memory_layout.c). */
#define ENGINE_NV2A_BASE  0xFD000000u
#define ENGINE_NV2A_SIZE  (16u * 1024u * 1024u)
#define ENGINE_VRAM_BASE  0xF0000000u

/* Native-to-Xbox VA offset, defined by the runtime's memory layout module
 * (the master header documents it but declares no prototype for it). */
extern ptrdiff_t g_xbox_mem_offset;

/* One guest dword, from host code (the runtime exposes no generic guest
 * read helper for game projects, and the recomp MEM32 macros live in the
 * generated code's translation unit). */
static uint32_t engine_mem32(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)va + (uintptr_t)g_xbox_mem_offset);
}

static void engine_mem32_write(uint32_t va, uint32_t v)
{
    *(volatile uint32_t *)((uintptr_t)va + (uintptr_t)g_xbox_mem_offset) = v;
}

/* ── PFIFO user-DMA channel acknowledgement ─────────────────
 *
 * The XDK D3D8 in this title keeps its NV2A FIFO user-DMA channel at
 * guest VA 0x00800000 (outside the register aperture): the submitted
 * write position (PUT) at +0x40, the consumed position (GET) at +0x44.
 * Submission writes the same position to the doorbell at VA 0x80000000
 * and the library then spins until GET advances to PUT -- the wait at
 * 0x000EECD6, `test (GET ^ PUT), 0x0FFFFFFF / jne`.
 *
 * Nothing here runs a GPU pusher, so GET would stay at 0 forever:
 * on hardware the GPU consumes the ring asynchronously, which is a
 * handshake, not a computation to reproduce.
 *
 * This is the same acknowledgement the runtime's own NV2A ack thread
 * models for the aperture's USER window (PUT -> GET at 0xFD800040 /
 * 0xFD800044, xbox_memory_layout.c) -- the commands are not executed
 * from the push buffer here (the D3D11 layer draws), so reporting them
 * consumed is the truthful answer, in the runtime's own words. The
 * title just keeps its channel copy outside the aperture, so
 * acknowledge that path here rather than in the aperture thread.
 *
 * Field offsets, recovered from the boot trace and cross-checked
 * against the on-disk XBE disassembly (0x000EEC78..0x000EECD6):
 *   dev  = MEM32(0xF9B80)     the D3D device object (0x000F6F40 here)
 *   chan = MEM32(dev + 0x23BC) the channel object (0x00800000 here)
 * A zero or wild pointer means the channel is not established yet, so
 * the polls are silent until device creation finishes storing it.
 *
 * There is a second, stricter handshake on the same object: the title's
 * software FIFO submits one NV2A method per call through sub_000F1823
 * and, when its write cursor (dev->[0], a VA inside the 512 KB
 * contiguous window) reaches the sliding end (dev->[4]), it asks for
 * room via sub_000EDED0 -> sub_000EDD50 -> sub_000EDD89. The gate
 * there, from the XBE at 0x000EDE19..0x000EDE21, is
 *
 *     room  <=>  consumed >= cursor  ||  consumed > cursor + chunk
 *     consumed = MEM32(chan + 0x44) | 0x80000000   (the channel GET,
 *                bounds-checked against dev->[0x24]..dev->[0x28], else
 *                the aperture GET at [dev->[0x4FC] + 0x324C])
 *     chunk    = MEM32(0xFA320)       (the title's own ring chunk size)
 *
 * so the GPU-side maintenance this thread models is not "GET == PUT"
 * (that reads as zero room and starves the waiter -- measured at
 * exactly 2 dwords of space, forever) but "the consumer lags the
 * writer by one chunk", which is the same handshake expressed the way
 * the waiter measures it. All of the geometry is read from the device
 * object at run time, so nothing about this title is baked in.
 */
#define ENGINE_PFIFO_DEV_GLOBAL   0xF9B80u
#define ENGINE_PFIFO_DEV_CHAN_OFF 0x23BCu
#define ENGINE_PFIFO_CHAN_PUT_OFF 0x40u
#define ENGINE_PFIFO_CHAN_GET_OFF 0x44u
#define ENGINE_PFIFO_RAM_HI       0x04000000u  /* 64 MB guest window */
#define ENGINE_PFIFO_RING_BASE_OFF 0x24u       /* ring base VA (dev->[0x24]) */
#define ENGINE_PFIFO_RING_END_OFF  0x28u       /* ring end VA  (dev->[0x28]) */
#define ENGINE_PFIFO_CHUNK_GLOBAL  0xFA320u    /* ring chunk size (bytes) */

static DWORD WINAPI engine_pfifo_ack_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        uint32_t dev, chan, cursor;

        Sleep(1);
        dev = engine_mem32(ENGINE_PFIFO_DEV_GLOBAL);
        if (!dev || dev >= ENGINE_PFIFO_RAM_HI)
            continue;
        chan = engine_mem32(dev + ENGINE_PFIFO_DEV_CHAN_OFF);
        if (chan < 0x10000u || chan >= ENGINE_PFIFO_RAM_HI)
            continue;                       /* channel not established yet */
        /* The ring-space model. The title writes method words into its ring,
         * advancing dev->[0] as a guest VA, and only folds that cursor into
         * the channel pair when it flushes -- which it never reaches while
         * the waiter starves. On hardware the GPU's DMA engine advances the
         * consumed position (chan->[0x44]) as it reads the ring, and the
         * waiter measures room as "consumed is a chunk ahead of the cursor"
         * (see the gate quoted in the comment above).
         *
         * The model: keep the consumer exactly one chunk behind the writer,
         * so the room the waiter measures is (ring size - chunk) -- always
         * more than the chunk it demands. Everything submitted is treated as
         * consumed (the D3D11 layer draws, the same argument the runtime's
         * own aperture ack thread makes for the USER window). Geometry and
         * lag come from the device object and the title's own chunk global,
         * so a mid-init or wild cursor is skipped rather than guessed at.
         *
         * The channel's PUT is deliberately left alone: mirroring PUT into
         * GET (the library's own flush semantics) reads as zero room and is
         * what starved this wait in the first place. */
        {
            uint32_t base  = engine_mem32(dev + ENGINE_PFIFO_RING_BASE_OFF)
                             & 0x0FFFFFFFu;
            uint32_t upper = engine_mem32(dev + ENGINE_PFIFO_RING_END_OFF)
                             & 0x0FFFFFFFu;
            if (base && upper > base + 0x8000u) {   /* a real ring, not noise */
                uint32_t size = upper - base;
                uint32_t lag  = engine_mem32(ENGINE_PFIFO_CHUNK_GLOBAL);
                if (lag == 0u || lag >= size / 2u)  /* a sane backlog */
                    lag = size / 4u;
                cursor = engine_mem32(dev) & 0x0FFFFFFFu;
                if (cursor >= base && cursor < upper) {
                    uint32_t get = base +
                        ((cursor - base + size - lag) % size);
                    if (get >= upper)              /* cannot happen; keep */
                        get -= size;               /* the waiter's bounds */
                    engine_mem32_write(chan + ENGINE_PFIFO_CHAN_GET_OFF, get);
                }
            }
        }
    }
    return 0;
}

static void engine_pfifo_ack_install(void)
{
    HANDLE t = CreateThread(NULL, 0, engine_pfifo_ack_thread, NULL, 0, NULL);
    if (!t) {
        fprintf(stderr, "[PFIFO] channel ack thread not started (error %lu); "
                        "the push-buffer wait will hang\n", GetLastError());
        return;
    }
    CloseHandle(t);
    fprintf(stderr, "[PFIFO] channel ack armed (GET follows PUT on the "
                    "0x00800000 user-DMA channel)\n");
}

/* Route an NV2A access into the register model. Returns TRUE when the
 * instruction was decoded and executed (RIP advanced past it). */
static LONG CALLBACK engine_nv2a_veh(PEXCEPTION_POINTERS ep)
{
    uintptr_t fault_addr;
    uintptr_t base;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    base = (uintptr_t)ENGINE_NV2A_BASE + (uintptr_t)g_xbox_mem_offset;
    if (fault_addr < base || fault_addr >= base + ENGINE_NV2A_SIZE) {
        /* The VRAM window above the runtime's tiled RAM alias: the alias
         * (0xF0000000, physical RAM a second time) is mapped, but the
         * framebuffer space above it is not -- the title reads and writes
         * its surfaces there. The hook's placeholder allocates zeroed
         * pages on demand, which is what a hardware read would give a
         * title before the first frame anyway. */
        uintptr_t vram = (uintptr_t)ENGINE_VRAM_BASE +
                         (uintptr_t)g_xbox_mem_offset;
        if (fault_addr >= vram && fault_addr < base) {
            uint32_t vram_va = (uint32_t)(fault_addr -
                                          (uintptr_t)g_xbox_mem_offset);
            if (nv2a_hook_handle_vram(fault_addr, vram_va))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    {
        int is_write = ep->ExceptionRecord->ExceptionInformation[0] != 0;
        uint32_t fault_va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
        if (nv2a_hook_handle_mmio(ep->ContextRecord, fault_addr,
                                  fault_va, is_write))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Install the model over the whole register aperture. Called once the
 * Xbox memory is mapped and the kernel bridge is up, before the game's
 * entry point runs. */
static void engine_nv2a_install(void)
{
    uintptr_t base = (uintptr_t)ENGINE_NV2A_BASE + (uintptr_t)g_xbox_mem_offset;
    DWORD old_protect;
    size_t off;

    /* The handler FIRST, then the traps. The runtime's own NV2A ack thread
     * ticks the register file from the moment the layout is up, and the
     * hook's init traps the PVIDEO page before this function returns: an
     * access to a trapped page before the handler exists is an unguarded
     * fault that takes the process down silently (observed on a fast
     * runner, never on the slow notebook -- it is a timing race, so it is
     * fixed by ordering, not by luck). */
    if (!AddVectoredExceptionHandler(1, engine_nv2a_veh)) {
        fprintf(stderr, "[NV2A] VEH not installed; register reads will "
                        "keep returning 0\n");
        return;
    }

    /* The model itself: VRAM, RAMIN and the register handlers. The hook's
     * own init also traps the PVIDEO page for the overlay scan-out. */
    nv2a_hook_init(g_xbox_mem_offset);

    for (off = 0; off < ENGINE_NV2A_SIZE; off += 0x1000u) {
        if (!VirtualProtect((LPVOID)(base + off), 0x1000u, PAGE_NOACCESS,
                            &old_protect)) {
            fprintf(stderr, "[NV2A] page 0x%08X not trapped (error %lu); "
                            "register reads will keep returning 0\n",
                    ENGINE_NV2A_BASE + (uint32_t)off, GetLastError());
            return;
        }
    }

    fprintf(stderr, "[NV2A] register model installed over the %u MB aperture "
                    "(reads now answer hardware values)\n",
            ENGINE_NV2A_SIZE / (1024u * 1024u));
}

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x000DAB5D  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\default.xbe"
#define YOUR_GAME_DIR            "game"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The APU host layer.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Lethal Shot - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the APU host layer.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] APU host layer %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(YOUR_GAME_DIR, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 5b: NV2A register model, so the game's D3D8 library reads real
     * hardware values from the GPU aperture instead of zeros (without it
     * the device bring-up never finishes its DMA handshake). */
    printf("Installing NV2A register model...\n");
    engine_nv2a_install();

    /* Step 5c: acknowledge the PFIFO user-DMA channel: the game's D3D8
     * library spins waiting for the GPU to consume its submitted push
     * buffer, and there is no GPU pusher here. Without this the wait at
     * 0x000EECD6 never ends and device creation hangs. */
    printf("Installing PFIFO channel ack...\n");
    engine_pfifo_ack_install();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
