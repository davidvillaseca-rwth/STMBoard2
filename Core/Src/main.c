#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Board monitor: PPG heart rate + buttons + potentiometer + LEDs + buzzer, streamed to Claude/ppg.html.
   Previous programs: the .c.bak files in Claude/

   PPG signal chain (pulse sensor = bottom-left block, A0 = PA0 = ADC1_IN1)
     TIM6 interrupt, 500 Hz: 16 ADC conversions summed (long 247.5-cycle sampling)
     -> 10-sample moving average (20 ms = one 50 Hz mains period: cancels light flicker) -> 100 Hz
     -> high-pass 0.5 Hz (removes DC level and drift) -> low-pass 4 Hz (2nd-order Butterworth)
     -> polarity: steep edge of the pulse (systole) made positive, automatic or forced from the page
     -> beat = rises through 50 % of a decaying peak envelope, after being below 0, >= 300 ms after the last
     -> BPM = 60000 / median of the last 5 beat-to-beat intervals

   Other parts
     Pot A1 = PA1, read on ADC2_IN2 (ADC1 is busy with the pulse sensor), 25 times a second.
       The pot sets the buzzer volume 0..50 % duty (50 = loudest); a short beep confirms each change.
     Buttons (HIGH = pressed): LEFT D2 PA10, DOWN D3 PB3, RIGHT D4 PB5, UP D5 PB4
       UP / DOWN = LED2 brightness +10 / -10, RIGHT / LEFT = LED3 brightness +10 / -10
       (hold to repeat; the LED lights for 0.6 s so you can see the new level). The beat beep is switched on / off from the page.
     LED2 green D6 PB10, LED3 yellow D7 PA8: mode off / on / flash on each beat, brightness 0..100,
       start 10 (software PWM from TIM7 at 20 kHz, duty = s*s/100). Buzzer D9 PC7 TIM3_CH2.
     Serial: TX and RX are serviced from TIM7 (ring buffers), so the main loop never blocks.

   Lines to the page
     P,ms,raw,filt,thr,bpm10,beat,ibi,q,pol,btn,l2,l3          100 per second
       raw = 12-bit x16, filt / thr = x100, bpm10 = BPM x10, beat = 1 on a beat, ibi = interval ms,
       q = 0 no pulse, 1 saturated, 2 searching, 3 good, 4 irregular, pol = +1 / -1,
       btn = bit 0 LEFT, 1 DOWN, 2 RIGHT, 3 UP;  l2 / l3 = LED lit right now (0 / 1)
     S,ms,pot,polmode,beep,m2,m3,s2,s3,vol                       10 per second
       pot = 0..4095, polmode = 0 auto / 1 normal / -1 inverted, m2 / m3 = LED mode 0 off 1 on 2 beat,
       s2 / s3 = brightness 0..100, vol = buzzer volume 0..50 (% duty)
   Commands (one per line, ending with a newline)
     i  cycle polarity        b  beep on / off
     2m0 / 2m1 / 2m2  LED2 off / on / beat   (3m.. for LED3)
     2:NN / 3:NN      LED2 / LED3 brightness 0..100                                               */

#define AMP_MIN     1.5f    /* smallest pulse peak (12-bit ADC counts) accepted as a real pulse */
#define REFRACT     30      /* samples at 100 Hz = 300 ms between beats */
#define NO_BEAT     300     /* 3 s without a beat -> BPM cleared */
#define BEEP_HZ     2000
#define VOL_MAX     50      /* % duty, loudest */
#define LED_START_S 10
#define LED_STEP    10      /* D-pad brightness step */
#define PREVIEW_MS  600     /* LED shown after a D-pad change */

#define LED2_Pin       GPIO_PIN_10   /* D6 */
#define LED2_GPIO_Port GPIOB
#define LED3_Pin       GPIO_PIN_8    /* D7 */
#define LED3_GPIO_Port GPIOA

static UART_HandleTypeDef hlpuart1;
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;

void SystemClock_Config(void);
void Error_Handler(void);

/* ---------- shared state ---------- */
static volatile int8_t  pol_mode = 0;              /* 0 auto, 1 normal, -1 inverted */
static volatile uint8_t beep_on = 0;
static volatile uint8_t led_mode[2] = {2, 0};      /* LED2 beat, LED3 off */
static volatile uint8_t led_s[2] = {LED_START_S, LED_START_S};
static volatile uint8_t led_duty[2] = {LED_START_S * LED_START_S / 100, LED_START_S * LED_START_S / 100};
static volatile uint8_t beat_flash = 0;            /* counts down at 100 Hz after a beat */
static volatile uint32_t preview_until[2] = {0, 0};/* LED forced on until this tick (D-pad change) */
static volatile uint8_t vol = 10;                  /* buzzer volume, % duty, set by the pot */
static volatile uint8_t beep_cnt = 0;              /* beep length left, in 10 ms steps */

static int led_lit(int i)
{
  return led_mode[i] == 1 || (led_mode[i] == 2 && beat_flash)
      || (int32_t)(preview_until[i] - HAL_GetTick()) > 0;
}

static void set_bright(int i, int v)               /* i = 0 LED2, 1 LED3; v = 0..100 */
{
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  uint8_t duty = (uint8_t)(v * v / 100);
  if (v > 0 && duty == 0) duty = 1;
  led_s[i] = (uint8_t)v;
  led_duty[i] = duty;
}

/* ---------- serial ring buffers, serviced from TIM7 ---------- */
#define TXN 2048
static volatile uint8_t  txbuf[TXN];
static volatile uint16_t txh = 0, txt = 0;
#define RXN 64
static volatile uint8_t  rxbuf[RXN];
static volatile uint8_t  rxh = 0, rxt = 0;

static void tx_str(const char *s)
{
  uint16_t h = txh;
  while (*s) {
    uint16_t n = (uint16_t)((h + 1) % TXN);
    if (n == txt) break;                 /* buffer full: drop the rest */
    txbuf[h] = (uint8_t)*s++;
    h = n;
  }
  txh = h;
}

/* ---------- PPG samples: filled in TIM6, sent from the main loop ---------- */
typedef struct { uint32_t ms; uint32_t raw; int32_t filt, thr; uint16_t bpm10, ibi; uint8_t beat, q; int8_t pol; } out_t;
#define OUTN 32
static volatile out_t outbuf[OUTN];
static volatile uint8_t outh = 0, outt = 0;

/* ---------- init ---------- */
static void uart_init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_LPUART1_CLK_ENABLE();
  g.Pin = GPIO_PIN_2 | GPIO_PIN_3;
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Alternate = GPIO_AF12_LPUART1;
  HAL_GPIO_Init(GPIOA, &g);

  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 115200;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK) Error_Handler();
}

static void adc_common(ADC_HandleTypeDef *h, ADC_TypeDef *inst)
{
  h->Instance = inst;
  h->Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  h->Init.Resolution = ADC_RESOLUTION_12B;
  h->Init.DataAlign = ADC_DATAALIGN_RIGHT;
  h->Init.GainCompensation = 0;
  h->Init.ScanConvMode = ADC_SCAN_DISABLE;
  h->Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  h->Init.LowPowerAutoWait = DISABLE;
  h->Init.ContinuousConvMode = DISABLE;
  h->Init.NbrOfConversion = 1;
  h->Init.DiscontinuousConvMode = DISABLE;
  h->Init.ExternalTrigConv = ADC_SOFTWARE_START;
  h->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  h->Init.DMAContinuousRequests = DISABLE;
  h->Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  h->Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(h) != HAL_OK) Error_Handler();
  if (HAL_ADCEx_Calibration_Start(h, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();
}

static void adc_channel(ADC_HandleTypeDef *h, uint32_t ch)
{
  ADC_ChannelConfTypeDef c = {0};
  c.Channel = ch;
  c.Rank = ADC_REGULAR_RANK_1;
  c.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
  c.SingleDiff = ADC_SINGLE_ENDED;
  c.OffsetNumber = ADC_OFFSET_NONE;
  c.Offset = 0;
  if (HAL_ADC_ConfigChannel(h, &c) != HAL_OK) Error_Handler();
}

static void adc_init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  g.Pin = GPIO_PIN_1;                           /* A1 pot (PA0 is set up by HAL_ADC_MspInit) */
  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &g);

  adc_common(&hadc1, ADC1);                     /* first: its MSP init sets up the shared ADC12 clock */
  adc_channel(&hadc1, ADC_CHANNEL_1);
  adc_common(&hadc2, ADC2);
  adc_channel(&hadc2, ADC_CHANNEL_2);

  /* one conversion through HAL enables ADC1; after that TIM6 starts conversions directly */
  HAL_ADC_Start(&hadc1);
  HAL_ADC_PollForConversion(&hadc1, 10);
  (void)HAL_ADC_GetValue(&hadc1);
}

static uint32_t pot_read(void)          /* average of 8 conversions, 0..4095 */
{
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    HAL_ADC_Start(&hadc2);
    HAL_ADC_PollForConversion(&hadc2, 10);
    sum += HAL_ADC_GetValue(&hadc2);
    HAL_ADC_Stop(&hadc2);
  }
  return sum / 8;
}

static void gpio_init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  g.Mode = GPIO_MODE_OUTPUT_PP;                 /* LEDs */
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Pin = LED2_Pin; HAL_GPIO_Init(LED2_GPIO_Port, &g);
  g.Pin = LED3_Pin; HAL_GPIO_Init(LED3_GPIO_Port, &g);

  g.Mode = GPIO_MODE_INPUT;                     /* buttons: same setup as the tested debug platform */
  g.Pull = GPIO_PULLUP;
  g.Pin = GPIO_PIN_10;                         HAL_GPIO_Init(GPIOA, &g);   /* LEFT  */
  g.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5; HAL_GPIO_Init(GPIOB, &g);   /* DOWN, UP, RIGHT */
}

static uint32_t buttons(void)           /* bit 0 LEFT, 1 DOWN, 2 RIGHT, 3 UP */
{
  uint32_t m = 0;
  if (GPIOA->IDR & GPIO_PIN_10) m |= 1;
  if (GPIOB->IDR & GPIO_PIN_3)  m |= 2;
  if (GPIOB->IDR & GPIO_PIN_5)  m |= 4;
  if (GPIOB->IDR & GPIO_PIN_4)  m |= 8;
  return m;
}

static void buzzer_init(void)          /* D9 = PC7 = TIM3_CH2, 1 MHz tick */
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE();
  g.Pin = GPIO_PIN_7;
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(GPIOC, &g);

  RCC->APB1ENR1 |= RCC_APB1ENR1_TIM3EN;
  (void)RCC->APB1ENR1;
  TIM3->PSC  = 169;
  TIM3->ARR  = 1000000u / BEEP_HZ - 1u;
  TIM3->CCR2 = 0;
  TIM3->CCMR1 = (6u << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;
  TIM3->CCER = TIM_CCER_CC2E;
  TIM3->CR1  = TIM_CR1_ARPE | TIM_CR1_CEN;
  TIM3->EGR  = TIM_EGR_UG;
}

static void timers_init(void)
{
  RCC->APB1ENR1 |= RCC_APB1ENR1_TIM6EN | RCC_APB1ENR1_TIM7EN;
  (void)RCC->APB1ENR1;

  TIM7->PSC = 169;                     /* 1 MHz */
  TIM7->ARR = 49;                      /* 20 kHz: LED PWM + serial */
  TIM7->EGR = TIM_EGR_UG;
  TIM7->SR = 0;
  TIM7->DIER = TIM_DIER_UIE;
  NVIC_SetPriority(TIM7_DAC_IRQn, 1);  /* higher priority: may interrupt the ADC sampling */
  NVIC_EnableIRQ(TIM7_DAC_IRQn);
  TIM7->CR1 = TIM_CR1_CEN;

  TIM6->PSC = 169;                     /* 1 MHz */
  TIM6->ARR = 1999;                    /* 500 Hz: pulse sampling */
  TIM6->EGR = TIM_EGR_UG;
  TIM6->SR = 0;
  TIM6->DIER = TIM_DIER_UIE;
  NVIC_SetPriority(TIM6_DAC_IRQn, 2);
  NVIC_EnableIRQ(TIM6_DAC_IRQn);
  TIM6->CR1 = TIM_CR1_CEN;
}

/* ---------- 20 kHz: LED dimming + serial ---------- */
void TIM7_DAC_IRQHandler(void)
{
  static uint8_t step = 0;
  TIM7->SR = 0;
  if (++step >= 100) step = 0;
  LED2_GPIO_Port->BSRR = (led_lit(0) && step < led_duty[0]) ? (uint32_t)LED2_Pin : ((uint32_t)LED2_Pin << 16);
  LED3_GPIO_Port->BSRR = (led_lit(1) && step < led_duty[1]) ? (uint32_t)LED3_Pin : ((uint32_t)LED3_Pin << 16);

  uint32_t isr = LPUART1->ISR;
  if ((isr & USART_ISR_TXE_TXFNF) && txt != txh) {
    LPUART1->TDR = txbuf[txt];
    txt = (uint16_t)((txt + 1) % TXN);
  }
  if (isr & USART_ISR_RXNE_RXFNE) {
    uint8_t c = (uint8_t)LPUART1->RDR;
    uint8_t n = (uint8_t)((rxh + 1) % RXN);
    if (n != rxt) { rxbuf[rxh] = c; rxh = n; }
  }
  if (isr & USART_ISR_ORE) LPUART1->ICR = USART_ICR_ORECF;
}

/* ---------- 4 Hz low-pass coefficients (2nd-order Butterworth, fs = 100 Hz) ---------- */
static float lp_b0, lp_b1, lp_b2, lp_a1, lp_a2;
static void lp_design(float fc, float fs)
{
  float k = tanf(3.14159265f * fc / fs);
  float n = 1.0f / (1.0f + 1.41421356f * k + k * k);
  lp_b0 = k * k * n;
  lp_b1 = 2.0f * lp_b0;
  lp_b2 = lp_b0;
  lp_a1 = 2.0f * (k * k - 1.0f) * n;
  lp_a2 = (1.0f - 1.41421356f * k + k * k) * n;
}
#define HP_A 0.969f                    /* 1st-order high-pass, fc = (1 - a) * fs / (2 pi) = 0.49 Hz */

/* ---------- 100 Hz processing: filters, polarity, beat detection ---------- */
static void process(float x, uint32_t raw16)
{
  static int init = 0;
  static float hp_x, hp_y, x1, x2, y1, y2, y_prev;
  static float penv, nenv, env;
  static int8_t pol = 1;
  static int armed = 0;
  static uint32_t since = NO_BEAT, n = 0;
  static uint16_t ibis[5];
  static uint8_t nibi = 0, ibi_i = 0;
  static uint16_t bpm10 = 0;
  static int have_prev = 0;

  n++;
  if (!init) { hp_x = x; init = 1; }

  hp_y = HP_A * (hp_y + x - hp_x);
  hp_x = x;
  float y = lp_b0 * hp_y + lp_b1 * x1 + lp_b2 * x2 - lp_a1 * y1 - lp_a2 * y2;
  x2 = x1; x1 = hp_y; y2 = y1; y1 = y;

  float d = y - y_prev;                            /* polarity from the steeper edge */
  y_prev = y;
  if (d > penv) penv = d; else penv *= 0.997f;
  if (-d > nenv) nenv = -d; else nenv *= 0.997f;
  if (pol_mode == 0) {
    if (nenv > 1.25f * penv) pol = -1;
    else if (penv > 1.25f * nenv) pol = 1;
  } else pol = pol_mode;
  float s = pol * y;

  if (s > env) env = s; else env *= 0.995f;       /* peak envelope, ~2 s memory */
  float thr = 0.5f * env;
  if (since < 100000) since++;
  if (s < 0) armed = 1;

  uint8_t beat = 0;
  uint16_t ibi = 0;
  if (armed && s > thr && env > AMP_MIN && since >= REFRACT) {
    armed = 0;
    beat = 1;
    if (have_prev && since <= 200) {               /* 300..2000 ms is plausible */
      ibi = (uint16_t)(since * 10);
      ibis[ibi_i] = ibi; ibi_i = (uint8_t)((ibi_i + 1) % 5);
      if (nibi < 5) nibi++;
    }
    have_prev = 1;
    since = 0;
    beat_flash = 8;                                /* LEDs in beat mode: 80 ms */
    if (beep_on) beep_cnt = 3;                     /* 30 ms beep */
  }
  if (since > NO_BEAT) { nibi = 0; have_prev = 0; }

  uint16_t med = 0, lo = 0xFFFF, hi = 0;
  if (nibi >= 2) {
    uint16_t t[5];
    for (int i = 0; i < nibi; i++) { t[i] = ibis[i]; if (t[i] < lo) lo = t[i]; if (t[i] > hi) hi = t[i]; }
    for (int i = 1; i < nibi; i++)
      for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { uint16_t z = t[j]; t[j] = t[j - 1]; t[j - 1] = z; }
    med = t[nibi / 2];
    bpm10 = (uint16_t)(600000u / med);
  } else bpm10 = 0;

  uint8_t q;
  if (raw16 < 40u * 16u || raw16 > 4050u * 16u) q = 1;
  else if (env < AMP_MIN) q = 0;
  else if (!bpm10) q = 2;
  else if (nibi >= 4 && (hi - lo) * 10 > med * 3) q = 4;
  else q = 3;

  if (beat_flash && !beat) beat_flash--;
  if (beep_cnt) { beep_cnt--; TIM3->CCR2 = (TIM3->ARR + 1u) * vol / 100u; }
  else TIM3->CCR2 = 0;

  uint8_t nh = (uint8_t)((outh + 1) % OUTN);
  if (nh != outt) {
    volatile out_t *o = &outbuf[outh];
    o->ms = n * 10; o->raw = raw16;
    o->filt = (int32_t)(s * 100.0f); o->thr = (int32_t)(thr * 100.0f);
    o->bpm10 = bpm10; o->ibi = ibi; o->beat = beat; o->q = q; o->pol = pol;
    outh = nh;
  }
}

/* ---------- 500 Hz pulse sampling ---------- */
void TIM6_DAC_IRQHandler(void)
{
  static uint32_t ma_buf[10], ma_sum = 0;
  static uint8_t ma_i = 0, dec = 0, warm = 0;
  TIM6->SR = 0;

  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    LL_ADC_REG_StartConversion(ADC1);
    while (!LL_ADC_IsActiveFlag_EOC(ADC1)) {}
    sum += LL_ADC_REG_ReadConversionData12(ADC1);   /* reading clears EOC */
  }

  ma_sum += sum - ma_buf[ma_i];                     /* 20 ms moving average: cancels 50 Hz */
  ma_buf[ma_i] = sum;
  ma_i = (uint8_t)((ma_i + 1) % 10);
  if (warm < 10) { warm++; return; }

  if (++dec < 5) return;                            /* 500 Hz -> 100 Hz */
  dec = 0;
  process((float)ma_sum / 160.0f, ma_sum / 10u);
}

/* ---------- commands from the page ---------- */
static void command(const char *c)
{
  if (c[0] == 'i') pol_mode = (pol_mode == 0) ? 1 : (pol_mode == 1) ? -1 : 0;
  else if (c[0] == 'b') beep_on = !beep_on;
  else if ((c[0] == '2' || c[0] == '3') && c[1] == 'm' && c[2] >= '0' && c[2] <= '2')
    led_mode[c[0] - '2'] = (uint8_t)(c[2] - '0');
  else if ((c[0] == '2' || c[0] == '3') && c[1] == ':')
    set_bright(c[0] - '2', atoi(c + 2));
}

/* D-pad: UP / DOWN = LED2, RIGHT / LEFT = LED3. Act on press, repeat while held. */
static void dpad(uint32_t btn, uint32_t now)
{
  static uint32_t prev = 0, next_rep[4] = {0}, lock[4] = {0};
  static const int led[4]  = {1, 0, 1, 0};          /* bit 0 LEFT, 1 DOWN, 2 RIGHT, 3 UP */
  static const int step[4] = {-LED_STEP, -LED_STEP, +LED_STEP, +LED_STEP};
  for (int k = 0; k < 4; k++) {
    int b = (btn >> k) & 1, was = (prev >> k) & 1;
    int fire = 0;
    if (b && !was) {                               /* press (150 ms lock-out against contact bounce) */
      if ((int32_t)(now - lock[k]) >= 0) { fire = 1; lock[k] = now + 150; }
      next_rep[k] = now + 450;                     /* first repeat after 450 ms */
    }
    else if (b && (int32_t)(now - next_rep[k]) >= 0) { fire = 1; next_rep[k] = now + 150; }
    if (fire) {
      set_bright(led[k], led_s[led[k]] + step[k]);
      preview_until[led[k]] = now + PREVIEW_MS;
    }
  }
  prev = btn;
}

int main(void)
{
  char msg[128];
  char cmd[12];
  uint8_t cl = 0;
  uint32_t pot = 0, last_pot = 0, last_s = 0, beep_next = 0;

  HAL_Init();
  SystemClock_Config();
  gpio_init();
  uart_init();
  buzzer_init();
  adc_init();
  lp_design(4.0f, 100.0f);
  pot = pot_read();
  vol = (uint8_t)(pot * VOL_MAX / 4095u);          /* start at the pot's volume, no beep */
  timers_init();

  tx_str("# board monitor ready\r\n");

  while (1)
  {
    uint32_t now = HAL_GetTick();
    uint32_t btn = buttons();
    dpad(btn, now);

    if (now - last_pot >= 40) {                    /* pot -> buzzer volume */
      last_pot = now;
      pot = (pot + pot_read()) / 2;
      int target = (int)(pot * VOL_MAX / 4095u);
      int diff = target - (int)vol;
      /* change in steps of 2 (ignores pot noise), but always reach 0 and the maximum */
      if ((diff >= 2 || diff <= -2 || ((target == 0 || target == VOL_MAX) && diff)) ) {
        vol = (uint8_t)target;
        if ((int32_t)(now - beep_next) >= 0) { beep_cnt = 3; beep_next = now + 150; }   /* short beep at the new volume */
      }
    }

    while (outt != outh) {                         /* one P line per PPG sample */
      out_t o;
      memcpy(&o, (const void *)&outbuf[outt], sizeof o);
      outt = (uint8_t)((outt + 1) % OUTN);
      snprintf(msg, sizeof msg, "P,%lu,%lu,%ld,%ld,%u,%u,%u,%u,%d,%lu,%d,%d\r\n",
               (unsigned long)o.ms, (unsigned long)o.raw, (long)o.filt, (long)o.thr,
               (unsigned)o.bpm10, (unsigned)o.beat, (unsigned)o.ibi, (unsigned)o.q, (int)o.pol,
               (unsigned long)btn, led_lit(0), led_lit(1));
      tx_str(msg);
    }

    if (now - last_s >= 100) {                     /* status line, 10 per second */
      last_s = now;
      snprintf(msg, sizeof msg, "S,%lu,%lu,%d,%u,%u,%u,%u,%u,%u\r\n",
               (unsigned long)now, (unsigned long)pot, (int)pol_mode, (unsigned)beep_on,
               (unsigned)led_mode[0], (unsigned)led_mode[1], (unsigned)led_s[0], (unsigned)led_s[1],
               (unsigned)vol);
      tx_str(msg);
    }

    while (rxt != rxh) {                           /* commands, one per line */
      char c = (char)rxbuf[rxt];
      rxt = (uint8_t)((rxt + 1) % RXN);
      if (c == '\r') continue;
      if (c != '\n') { if (cl < sizeof cmd - 1) cmd[cl++] = c; continue; }
      cmd[cl] = 0; cl = 0;
      command(cmd);
    }
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) Error_Handler();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
