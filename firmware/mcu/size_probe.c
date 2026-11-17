/*
 * size_probe.c - isolates the detector's true flash/RAM cost.
 *
 * Built twice with identical flags: WITH_IDS=0 (a loop that only touches the
 * same inputs) and WITH_IDS=1 (the loop runs the detector). The difference
 * between the two images is everything the detector pulls in: its code, its
 * weights, its state, and any libm / libgcc routines it needs.
 */
#include <stdint.h>

#include "fw/ids_rnn.h"

volatile ids_input_t g_in;   /* volatile: the compiler cannot constant-fold inputs */
volatile float       g_out;

#if WITH_IDS
static ids_rnn_t det;
#endif

int main(void)
{
#if WITH_IDS
    ids_rnn_init(&det);
#endif
    for (;;) {
        ids_input_t in;
        in.running     = g_in.running;
        in.alarm       = g_in.alarm;
        in.mem_alarm   = g_in.mem_alarm;
        in.target_rate = g_in.target_rate;
        in.flow        = g_in.flow;
        in.flow_seq    = g_in.flow_seq;
        in.flow_ok     = g_in.flow_ok;
        in.pressure    = g_in.pressure;
        in.press_seq   = g_in.press_seq;
        in.press_ok    = g_in.press_ok;
        in.duty        = g_in.duty;
#if WITH_IDS
        ids_rnn_step(&det, &in);
        g_out = det.attack_prob + (det.alert ? 1.0f : 0.0f);
#else
        g_out = in.flow + in.pressure + in.duty + in.target_rate + (float)(in.flow_seq + in.press_seq) +
                (in.running ? 1.0f : 0.0f);
#endif
    }
}
