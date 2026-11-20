#include "probe_config.h"
#include "pico/binary_info.h"
#include "probe/version.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

bool probe_config_pin_in_use(int pin)
{
    static const probe_pins_t swd[] = PROBE_CHANNEL_PINS;
    static const probe_uart_pins_t uart[] = PROBE_UART_PIN_TABLE;

    if (pin < 0)
        return false;
    for (uint i = 0; i < PROBE_NUM_CHANNELS; i++) {
        const probe_pins_t *p = &swd[i];
        if (p->swclk == pin || p->swdio == pin || p->swdi == pin ||
            p->swdioen == pin || p->reset == pin)
            return true;
    }
    for (uint i = 0; i < PROBE_NUM_UARTS; i++) {
        const probe_uart_pins_t *p = &uart[i];
        if (p->tx == pin || p->rx == pin || p->cts == pin ||
            p->rts == pin || p->dtr == pin)
            return true;
    }
    return false;
}

void bi_decl_config()
{
    bi_decl(bi_program_version_string(PROBE_VERSION));
    bi_decl(bi_program_feature("PROBE SWD CHANNELS " STR(PROBE_NUM_CHANNELS)));
    bi_decl(bi_program_feature("PROBE UARTS " STR(PROBE_NUM_UARTS)));
#ifdef PROBE_PIN_RESET
    bi_decl(bi_1pin_with_name(PROBE_PIN_RESET, "PROBE RESET"));
#endif

#ifdef PROBE_PIN_SWCLK
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWCLK, "PROBE SWCLK"));
#endif

#ifdef PROBE_PIN_SWDIO
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWDIO, "PROBE SWDIO"));
#endif

#ifdef PROBE_PIN_SWDI
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWDI, "PROBE SWDI"));
#endif

#ifdef PROBE_PIN_SWDIOEN
    bi_decl(bi_1pin_with_name(PROBE_PIN_SWDIOEN, "PROBE SWDIOEN"));
#endif

#ifdef PROBE_CDC_UART
    bi_decl(bi_program_feature("PROBE UART INTERFACE " STR(PROBE_UART_INTERFACE)));
    bi_decl(bi_program_feature("PROBE UART BAUDRATE " STR(PROBE_UART_BAUDRATE)));
    bi_decl(bi_1pin_with_name(PROBE_UART_TX, "PROBE UART TX"));
    bi_decl(bi_1pin_with_name(PROBE_UART_RX, "PROBE UART RX"));
#endif

#ifdef PROBE_UART_CTS
    bi_decl(bi_1pin_with_name(PROBE_UART_CTS, "PROBE UART CTS"));
#endif
#ifdef PROBE_UART_RTS
    bi_decl(bi_1pin_with_name(PROBE_UART_RTS, "PROBE UART RTS"));
#endif
#ifdef PROBE_UART_DTR
    bi_decl(bi_1pin_with_name(PROBE_UART_DTR, "PROBE UART DTR"));
#endif

}
