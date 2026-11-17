/*
 * bench.c - on-target benchmark of the IDS-RNN detector.
 *
 *  1. Replays the golden vectors (firmware/tests/ids_golden.h) through
 *     ids_rnn_step() and compares every probability with the Python int8 model,
 *     so correctness is checked on the real FPU / libm, not just on the PC.
 *  2. Times every step with the DWT cycle counter (pass 1: cold, pass 2: warm).
 *  3. Measures peak stack use by painting the free RAM with a pattern first.
 *
 * Results are printed on USART2 (the Nucleo-64 ST-Link virtual COM port,
 * 115200 8N1) and are also left in the global `g_bench` for a debugger.
 * The CPU runs on its reset clock (no PLL), so the flash has zero wait states;
 * cycles are clock-independent only under that condition (see the README).
 */
#include <math.h>
#include <stdint.h>

#include "fw/ids_rnn.h"
#include "ids_golden.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* ---- DWT cycle counter (Cortex-M3/M4/M7) --------------------------------- */
#define DEMCR      REG(0xE000EDFCu)
#define DWT_CTRL   REG(0xE0001000u)
#define DWT_CYCCNT REG(0xE0001004u)

static void cyccnt_init(void)
{
    DEMCR |= 1u << 24;    /* TRCENA */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;       /* CYCCNTENA */
}

/* ---- USART2 on PA2 (TX), AF7 ---------------------------------------------- */
#if defined(BENCH_STM32L4)
#define CPU_HZ     4000000u   /* MSI 4 MHz after reset */
#define RCC_AHB2EN REG(0x4002104Cu)
#define RCC_APB1EN REG(0x40021058u)
#define GPIOA_MODER REG(0x48000000u)
#define GPIOA_AFRL  REG(0x48000020u)
#define U2_CR1     REG(0x40004400u)
#define U2_BRR     REG(0x4000440Cu)
#define U2_ISR     REG(0x4000441Cu)
#define U2_TDR     REG(0x40004428u)
static void uart_init(void)
{
    RCC_AHB2EN |= 1u << 0;
    RCC_APB1EN |= 1u << 17;
    GPIOA_MODER = (GPIOA_MODER & ~(3u << 4)) | (2u << 4);
    GPIOA_AFRL  = (GPIOA_AFRL & ~(0xFu << 8)) | (7u << 8);
    U2_BRR = (CPU_HZ + 57600u) / 115200u;
    U2_CR1 = (1u << 3) | (1u << 0);   /* TE | UE */
}
static void uart_putc(char c)
{
    while (!(U2_ISR & (1u << 7))) {
    }
    U2_TDR = (uint8_t)c;
}
#else /* STM32F4 (F401 / F411 / F446 ...) */
#define CPU_HZ     16000000u  /* HSI 16 MHz after reset */
#define RCC_AHB1EN REG(0x40023830u)
#define RCC_APB1EN REG(0x40023840u)
#define GPIOA_MODER REG(0x40020000u)
#define GPIOA_AFRL  REG(0x40020020u)
#define U2_SR      REG(0x40004400u)
#define U2_DR      REG(0x40004404u)
#define U2_BRR     REG(0x40004408u)
#define U2_CR1     REG(0x4000440Cu)
static void uart_init(void)
{
    RCC_AHB1EN |= 1u << 0;
    RCC_APB1EN |= 1u << 17;
    GPIOA_MODER = (GPIOA_MODER & ~(3u << 4)) | (2u << 4);
    GPIOA_AFRL  = (GPIOA_AFRL & ~(0xFu << 8)) | (7u << 8);
    U2_BRR = (CPU_HZ + 57600u) / 115200u;
    U2_CR1 = (1u << 13) | (1u << 3);  /* UE | TE */
}
static void uart_putc(char c)
{
    while (!(U2_SR & (1u << 7))) {
    }
    U2_DR = (uint8_t)c;
}
#endif

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void put_u32(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    puts_(buf + i);
}

/* Fixed-point print with `dec` decimals (no printf, no float formatting libs). */
static void put_fix(float v, int dec)
{
    if (v < 0.0f) {
        uart_putc('-');
        v = -v;
    }
    uint32_t mul = 1;
    for (int i = 0; i < dec; ++i)
        mul *= 10u;
    uint32_t x = (uint32_t)(v * (float)mul + 0.5f);
    put_u32(x / mul);
    if (dec) {
        uart_putc('.');
        uint32_t frac = x % mul;
        for (uint32_t m = mul / 10u; m; m /= 10u) {
            uart_putc((char)('0' + frac / m));
            frac %= m;
        }
    }
}

/* ---- stack painting ------------------------------------------------------ */
extern uint32_t _ebss, _estack;
#define PAINT 0xA5A5A5A5u

static void paint_stack(void)
{
    uint32_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    for (volatile uint32_t *p = &_ebss; (uint32_t)p < sp - 64u; ++p)
        *p = PAINT;
}

static uint32_t stack_low_water(void)
{
    const volatile uint32_t *p = &_ebss;
    while (*p == PAINT && p < &_estack)
        ++p;
    return (uint32_t)&_estack - (uint32_t)p;  /* bytes used, from the top of RAM */
}

/* ---- results (also readable with a debugger) ------------------------------ */
typedef struct {
    uint32_t magic;            /* 0x1D5BE7C4 when complete */
    uint32_t rows;
    uint32_t cyc_cold_min, cyc_cold_max, cyc_cold_mean;
    uint32_t cyc_warm_min, cyc_warm_max, cyc_warm_mean;
    uint32_t cyc_feat_mean;    /* feature extraction alone */
    uint32_t cyc_overhead;     /* cost of reading DWT_CYCCNT twice, subtracted above */
    float    max_prob_err;
    uint32_t max_err_row;
    uint32_t alert_mismatch;
    uint32_t stack_idle;       /* stack used by main() before the detector runs */
    uint32_t stack_peak;       /* stack used at the deepest point of the benchmark */
    uint32_t state_bytes;
    uint32_t model_bytes;
    uint32_t cpu_hz;
} bench_t;

volatile bench_t g_bench __attribute__((used));

static ids_rnn_t det;

static __attribute__((noinline)) uint32_t step_timed(const ids_input_t *in)
{
    uint32_t t0 = DWT_CYCCNT;
    ids_rnn_step(&det, in);
    return DWT_CYCCNT - t0;
}

static __attribute__((noinline)) uint32_t empty_timed(void)
{
    uint32_t t0 = DWT_CYCCNT;
    __asm volatile("" ::: "memory");
    return DWT_CYCCNT - t0;
}

static void run_pass(uint32_t *mn, uint32_t *mx, uint32_t *mean, int check)
{
    uint64_t sum = 0;
    *mn = UINT32_MAX;
    *mx = 0;
    ids_rnn_init(&det);
    int above = 0;
    for (int t = 0; t < IDS_GOLDEN_N; ++t) {
        uint32_t c = step_timed(&IDS_GOLDEN[t].in) - g_bench.cyc_overhead;
        sum += c;
        if (c < *mn) *mn = c;
        if (c > *mx) *mx = c;
        if (!check)
            continue;
        for (int k = 0; k < IDS_N_CLASSES; ++k) {
            float e = fabsf(det.prob[k] - IDS_GOLDEN[t].prob[k]);
            if (e > g_bench.max_prob_err) {
                g_bench.max_prob_err = e;
                g_bench.max_err_row  = (uint32_t)t;
            }
        }
        float att = 1.0f - IDS_GOLDEN[t].prob[IDS_CLASS_NONE] - IDS_GOLDEN[t].prob[IDS_CLASS_OCCLUSION];
        if (fabsf(att - IDS_THRESHOLD) >= 5e-3f) {
            above = att > IDS_THRESHOLD ? above + 1 : 0;
            if (det.alert != (above >= IDS_DEBOUNCE))
                g_bench.alert_mismatch++;
        }
    }
    *mean = (uint32_t)(sum / IDS_GOLDEN_N);
}

static void line(const char *label, uint32_t cyc)
{
    puts_(label);
    put_u32(cyc);
    puts_(" cycles = ");
    put_fix((float)cyc * 1e6f / (float)CPU_HZ, 1);
    puts_(" us @ ");
    put_u32(CPU_HZ / 1000000u);
    puts_(" MHz (");
    put_fix((float)cyc * 1e6f / 84e6f, 1);
    puts_(" us @ 84 MHz, ");
    put_fix((float)cyc * 1e6f / 180e6f, 1);
    puts_(" us @ 180 MHz)\n");
}

int main(void)
{
    paint_stack();
    g_bench.stack_idle = stack_low_water();
    uart_init();
    cyccnt_init();

    g_bench.rows        = IDS_GOLDEN_N;
    g_bench.cpu_hz      = CPU_HZ;
    g_bench.state_bytes = sizeof(ids_rnn_t);
    g_bench.model_bytes = IDS_MODEL_BYTES;

    uint32_t ov = UINT32_MAX;
    for (int i = 0; i < 16; ++i) {
        uint32_t c = empty_timed();
        if (c < ov) ov = c;
    }
    g_bench.cyc_overhead = ov;

    paint_stack();   /* repaint below main's frame; everything deeper is the detector */
    uint32_t a, b, c;
    run_pass(&a, &b, &c, 1);
    g_bench.cyc_cold_min = a; g_bench.cyc_cold_max = b; g_bench.cyc_cold_mean = c;
    g_bench.stack_peak = stack_low_water();
    run_pass(&a, &b, &c, 0);
    g_bench.cyc_warm_min = a; g_bench.cyc_warm_max = b; g_bench.cyc_warm_mean = c;

    ids_feat_t f = { 0 };
    float x[IDS_N_FEATURES];
    uint64_t fsum = 0;
    for (int t = 0; t < IDS_GOLDEN_N; ++t) {
        uint32_t t0 = DWT_CYCCNT;
        ids_features(&f, &IDS_GOLDEN[t].in, x);
        fsum += DWT_CYCCNT - t0 - ov;
    }
    g_bench.cyc_feat_mean = (uint32_t)(fsum / IDS_GOLDEN_N);
    g_bench.magic         = 0x1D5BE7C4u;

    for (;;) {
        puts_("\n=== IDS-RNN on-target benchmark ===\n");
        puts_("golden rows        : "); put_u32(g_bench.rows); puts_("\n");
        puts_("max |dP| vs Python : "); put_fix(g_bench.max_prob_err, 6);
        puts_(" (row "); put_u32(g_bench.max_err_row); puts_(")  ");
        puts_(g_bench.max_prob_err <= 2e-3f ? "PASS\n" : "FAIL\n");
        puts_("alert mismatches   : "); put_u32(g_bench.alert_mismatch);
        puts_(g_bench.alert_mismatch == 0 ? "  PASS\n" : "  FAIL\n");
        line("step, cold  mean   : ", g_bench.cyc_cold_mean);
        line("step, cold  max    : ", g_bench.cyc_cold_max);
        line("step, warm  min    : ", g_bench.cyc_warm_min);
        line("step, warm  mean   : ", g_bench.cyc_warm_mean);
        line("step, warm  max    : ", g_bench.cyc_warm_max);
        line("features only mean : ", g_bench.cyc_feat_mean);
        puts_("CPU load @ 100 ms period, 16 MHz: ");
        put_fix(100.0f * (float)g_bench.cyc_warm_max / (0.1f * (float)CPU_HZ), 3); puts_(" %\n");
        puts_("stack, main idle   : "); put_u32(g_bench.stack_idle); puts_(" B\n");
        puts_("stack, peak        : "); put_u32(g_bench.stack_peak); puts_(" B  (detector = peak - idle = ");
        put_u32(g_bench.stack_peak - g_bench.stack_idle); puts_(" B)\n");
        puts_("detector state     : "); put_u32(g_bench.state_bytes); puts_(" B (static RAM)\n");
        puts_("model parameters   : "); put_u32(g_bench.model_bytes); puts_(" B (flash)\n");
        for (volatile uint32_t d = 0; d < CPU_HZ / 2u; ++d) {
        }
    }
}
