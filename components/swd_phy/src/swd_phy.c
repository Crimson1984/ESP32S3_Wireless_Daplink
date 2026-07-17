#include "swd_phy.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "sdkconfig.h"

#define SWDIO_GPIO ((gpio_num_t)CONFIG_DATLINK_SWD_IO_GPIO)
#define SWCLK_GPIO ((gpio_num_t)CONFIG_DATLINK_SWD_CLK_GPIO)
#define RESET_GPIO ((gpio_num_t)CONFIG_DATLINK_SWD_RESET_GPIO)

static const char *TAG = "swd_phy";
static adc_oneshot_unit_handle_t s_adc;
static uint32_t s_clock_khz = CONFIG_DATLINK_SWD_INITIAL_KHZ;
static uint32_t s_half_cycles;
static bool s_enabled;

static inline void IRAM_ATTR delay_half_cycle(void)
{
    const uint32_t start = esp_cpu_get_cycle_count();
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < s_half_cycles) {}
}

static inline void IRAM_ATTR set_clk(int level)
{
    gpio_set_level(SWCLK_GPIO, level);
}

static inline void IRAM_ATTR clock_out_bit(uint32_t bit)
{
    gpio_set_level(SWDIO_GPIO, (int)(bit & 1U));
    delay_half_cycle();
    set_clk(1);
    delay_half_cycle();
    set_clk(0);
}

static inline uint32_t IRAM_ATTR clock_in_bit(void)
{
    delay_half_cycle();
    set_clk(1);
    delay_half_cycle();
    const uint32_t bit = (uint32_t)gpio_get_level(SWDIO_GPIO);
    set_clk(0);
    return bit;
}

static void swdio_output(void)
{
    gpio_set_direction(SWDIO_GPIO, GPIO_MODE_OUTPUT);
}

static void swdio_input(void)
{
    gpio_set_direction(SWDIO_GPIO, GPIO_MODE_INPUT);
}

static void write_bits(uint32_t data, unsigned count)
{
    for (unsigned bit = 0; bit < count; ++bit) clock_out_bit(data >> bit);
}

static uint32_t read_bits(unsigned count)
{
    uint32_t data = 0;
    for (unsigned bit = 0; bit < count; ++bit) data |= clock_in_bit() << bit;
    return data;
}

void swd_phy_set_clock_khz(uint32_t khz)
{
    if (khz < 50U) khz = 50U;
    if (khz > 1000U) khz = 1000U;
    s_clock_khz = khz;
    const uint32_t cpu_khz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000U;
    s_half_cycles = cpu_khz / (2U * khz);
    if (s_half_cycles < 16U) s_half_cycles = 16U;
}

uint32_t swd_phy_clock_khz(void) { return s_clock_khz; }

esp_err_t swd_phy_init(void)
{
    swd_phy_set_clock_khz(CONFIG_DATLINK_SWD_INITIAL_KHZ);
    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "ADC unit");
    const adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0,
                                                   &channel_cfg),
                        TAG, "VTref ADC channel");
    swd_phy_safe_state();
    return ESP_OK;
}

void swd_phy_safe_state(void)
{
    gpio_set_direction(SWCLK_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(SWDIO_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(RESET_GPIO, GPIO_MODE_INPUT);
    s_enabled = false;
}

bool swd_phy_target_present(void)
{
    int raw = 0;
    return s_adc != NULL && adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) == ESP_OK && raw > 300;
}

esp_err_t swd_phy_enable(void)
{
    if (!swd_phy_target_present()) return ESP_ERR_INVALID_STATE;
    const gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << SWCLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "SWCLK config");
    const gpio_config_t reset_cfg = {
        .pin_bit_mask = (1ULL << RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset_cfg), TAG, "nRESET config");
    swdio_output();
    gpio_set_level(SWDIO_GPIO, 1);
    gpio_set_level(RESET_GPIO, 1);
    set_clk(0);
    s_enabled = true;
    return ESP_OK;
}

void swd_phy_assert_reset(bool asserted)
{
    if (s_enabled) gpio_set_level(RESET_GPIO, asserted ? 0 : 1);
}

void swd_phy_line_reset(void)
{
    if (!s_enabled) return;
    swdio_output();
    gpio_set_level(SWDIO_GPIO, 1);
    write_bits(0xFFFFFFFFU, 32);
    write_bits(0xFFFFFFFFU, 28);
    write_bits(0xE79EU, 16);
    write_bits(0xFFFFFFFFU, 32);
    write_bits(0xFFFFFFFFU, 28);
    write_bits(0U, 8);
}

datlink_status_t swd_phy_transfer(bool ap, bool read, uint8_t address,
                                  uint32_t *data)
{
    if (!s_enabled || data == NULL) return DATLINK_ERR_STATE;
    const uint32_t a2 = (address >> 2) & 1U;
    const uint32_t a3 = (address >> 3) & 1U;
    const uint32_t parity = ((uint32_t)ap ^ (uint32_t)read ^ a2 ^ a3) & 1U;
    const uint32_t request = 1U | ((uint32_t)ap << 1) | ((uint32_t)read << 2) |
                             (a2 << 3) | (a3 << 4) | (parity << 5) | (1U << 7);

    swdio_output();
    write_bits(request, 8);
    swdio_input();
    (void)clock_in_bit();
    const uint32_t ack = read_bits(3);
    if (ack != SWD_ACK_OK) {
        swdio_input();
        for (unsigned i = 0; i < 33; ++i) (void)clock_in_bit();
        swdio_output();
        gpio_set_level(SWDIO_GPIO, 1);
        clock_out_bit(1);
        return ack == SWD_ACK_WAIT ? DATLINK_ERR_SWD_ACK_WAIT
             : ack == SWD_ACK_FAULT ? DATLINK_ERR_SWD_ACK_FAULT
                                    : DATLINK_ERR_LINK;
    }

    if (read) {
        const uint32_t value = read_bits(32);
        const uint32_t received_parity = read_bits(1);
        (void)clock_in_bit();
        swdio_output();
        gpio_set_level(SWDIO_GPIO, 1);
        clock_out_bit(1);
        if (((uint32_t)__builtin_parity(value)) != received_parity) {
            return DATLINK_ERR_SWD_PARITY;
        }
        *data = value;
    } else {
        (void)clock_in_bit();
        swdio_output();
        write_bits(*data, 32);
        clock_out_bit((uint32_t)__builtin_parity(*data));
        gpio_set_level(SWDIO_GPIO, 1);
        clock_out_bit(1);
    }
    return DATLINK_OK;
}
