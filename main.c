#include "MK64F12.h"
#include <stdint.h>

#define MASK(x) (1UL << (x))

// ---------------- Game States ----------------

typedef enum {
    IDLE,
    STARTING,
    WAITING,
    MEASURE,
    WON,
    LOST,
    FALSE_START,
    ENDED
} GameState;

#define STAR_SHOW_MS    1000U   // ค้าง STAR 1 วินาที ก่อนโชว์เลขที่สุ่มได้
#define TARGET_SHOW_MS    2000U // โชว์เลขเป้าหมาย 2 วินาที ก่อนเริ่มนับ
#define WIN_TOLERANCE_MS  300U  // กดตรงเป้าหมาย +/- 0.300 วินาที ถือว่า GOOD
#define END_DELAY_MS    2000U   // ค้าง END 2 วินาที แล้วกลับไปขึ้น PLAY

volatile GameState game_state = IDLE;

volatile uint32_t tick_ms = 0;
volatile uint32_t wait_ms = 0;
volatile uint32_t elapsed_ms = 0;
volatile uint32_t reaction_ms = 0;
volatile uint32_t last_press_ms = 0;
volatile uint32_t buzzer_ms = 0;
volatile uint32_t end_ms = 0;
volatile uint32_t star_ms = 0;
volatile uint32_t rand_ms = 0;   // เลขเป้าหมายที่สุ่มได้ (ms) ต้องกดให้ตรง

uint32_t random_seed = 12345;

// ---------------- 7-Segment ----------------

const uint8_t SEGMENT_MAP[10] = {
    0xC0, 0xF9, 0xA4, 0xB0, 0x99,
    0x92, 0x82, 0xF8, 0x80, 0x90
};

const uint8_t DIGIT_SELECT[4] = {
    0xF1, 0xF2, 0xF4, 0xF8
};

const uint8_t FAIL_MAP[4] = {
    0x8E, 0x88, 0xF9, 0xC7
};

// P L A Y
const uint8_t PLAY_MAP[4] = {
    0x8C, 0xC7, 0x88, 0x91
};

// S t A r
const uint8_t STAR_MAP[4] = {
    0x92, 0x87, 0x88, 0xAF
};

// _ E n d
const uint8_t END_MAP[4] = {
    0xFF, 0x86, 0xAB, 0xA1
};

// G o o d
const uint8_t GOOD_MAP[4] = {
    0xC2, 0xA3, 0xA3, 0xA1
};

// L O S E
const uint8_t LOSE_MAP[4] = {
    0xC7, 0xC0, 0x92, 0x86
};

// "StAr" (แสดงตอนกด SW2 เริ่มเกม)
const uint8_t START_MAP[4] = {
    0x92, 0x87, 0x88, 0xAF
};

// ---------------- Buzzer (active-low, ปิด = Hi-Z + pull-up) ----------------

#define BEEP_PRESS_MS        200U    // กดถูก
#define BEEP_FALSE_START_MS  1500U   // กดพลาด 1.5 วินาที

void Buzzer_Off(void)
{
    PTA->PDDR &= ~MASK(1);      // input (Hi-Z) ให้ pull-up ดึงขึ้น = ปิด
    buzzer_ms = 0;
}

void Buzzer_Beep(uint32_t duration)
{
    buzzer_ms = duration;
    PTA->PCOR = MASK(1);        // data = 0
    PTA->PDDR |= MASK(1);       // drive low = ดัง
}

void Buzzer_Init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;

    PORTA->PCR[1] = PORT_PCR_MUX(1) |
                    PORT_PCR_PE_MASK |
                    PORT_PCR_PS_MASK;   // pull-up

    Buzzer_Off();
}

// ---------------- GPIO ----------------

void GPIO_Init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK |
                  SIM_SCGC5_PORTD_MASK;

    // SW1 = PTB3 (Interrupt)
    PORTB->PCR[3] = PORT_PCR_MUX(1) |
                    PORT_PCR_PE_MASK |
                    PORT_PCR_PS_MASK |
                    PORT_PCR_IRQC(0xA);

    // SW2 = PTB10 (Start)
    PORTB->PCR[10] = PORT_PCR_MUX(1) |
                     PORT_PCR_PE_MASK |
                     PORT_PCR_PS_MASK;

    // SW3 = PTB11 (Reset)
    PORTB->PCR[11] = PORT_PCR_MUX(1) |
                     PORT_PCR_PE_MASK |
                     PORT_PCR_PS_MASK;

    // All switches input
    PTB->PDDR &= ~(MASK(3) | MASK(10) | MASK(11));

    // LED D4 = PTD0 (Active LOW)
    PORTD->PCR[0] = PORT_PCR_MUX(1);

    PTD->PDDR |= MASK(0);

    // LED OFF
    PTD->PSOR = MASK(0);

    NVIC_EnableIRQ(PORTB_IRQn);
}

// ---------------- Timer ----------------

void Timer_Init(void)
{
    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000U);
}

void SysTick_Handler(void)
{
    tick_ms++;

    if (game_state == STARTING)
    {
        if (star_ms > 0)
            star_ms--;

        if (star_ms == 0)
            game_state = WAITING;   // โชว์เลขเป้าหมาย
    }
    else if (game_state == WAITING)
    {
        if (wait_ms > 0)
            wait_ms--;

        if (wait_ms == 0)
        {
            PTD->PCOR = MASK(0);

            elapsed_ms = 0;
            game_state = MEASURE;
        }
    }
    else if (game_state == MEASURE)
    {
        elapsed_ms++;

        // นับเลยเป้าหมายไปแล้วยังไม่กด = LOSE
        if (elapsed_ms > rand_ms + WIN_TOLERANCE_MS)
        {
            PTD->PSOR = MASK(0);

            game_state = LOST;

            Buzzer_Beep(BEEP_FALSE_START_MS);
        }
    }
    else if (game_state == ENDED)
    {
        if (end_ms > 0)
            end_ms--;

        if (end_ms == 0)
            game_state = IDLE;      // กลับไปขึ้น PLAY
    }

    // Buzzer timeout
    if (buzzer_ms > 0)
    {
        buzzer_ms--;

        if (buzzer_ms == 0)
            Buzzer_Off();
    }
}

// ---------------- Random ----------------

uint32_t Random_Number(void)
{
    random_seed ^= random_seed << 13;
    random_seed ^= random_seed >> 17;
    random_seed ^= random_seed << 5;

    return random_seed;
}

uint32_t Random_Delay(void)
{
    return 2000U + (Random_Number() % 3001U);
}

// ---------------- Game ----------------

// Start game using SW2
void Game_Start(void)
{
    random_seed ^= tick_ms;

    if (random_seed == 0)
        random_seed = 12345;

    Buzzer_Off();

    PTD->PSOR = MASK(0);

    reaction_ms = 0;
    elapsed_ms = 0;

    rand_ms = Random_Delay();   // สุ่มเป้าหมาย 2.000-5.000 วินาที
    wait_ms = TARGET_SHOW_MS;   // เวลาที่โชว์เลขเป้าหมายก่อนเริ่มนับ

    star_ms = STAR_SHOW_MS;

    game_state = STARTING;
}

// Reset game using SW3
void Game_Reset(void)
{
    Buzzer_Off();

    PTD->PSOR = MASK(0);

    reaction_ms = 0;
    elapsed_ms = 0;
    wait_ms = 0;

    game_state = IDLE;
}

// End game using SW3
void Game_End(void)
{
    Buzzer_Off();

    PTD->PSOR = MASK(0);

    reaction_ms = 0;
    elapsed_ms = 0;
    wait_ms = 0;

    star_ms = 0;
    end_ms = END_DELAY_MS;

    game_state = ENDED;
}

// ---------------- SW1 Interrupt ----------------

void PORTB_IRQHandler(void)
{
    PORTB->ISFR = MASK(3);

    if ((uint32_t)(tick_ms - last_press_ms) < 200U)
        return;

    last_press_ms = tick_ms;

    if (game_state == WAITING)
    {
        // False start
        PTD->PSOR = MASK(0);

        game_state = FALSE_START;

        Buzzer_Beep(BEEP_FALSE_START_MS);   // ดัง 1.5 วินาที
    }
    else if (game_state == MEASURE)
    {
        // Reaction time recorded
        reaction_ms = elapsed_ms;

        PTD->PSOR = MASK(0);

        uint32_t diff = (reaction_ms > rand_ms) ?
                        (reaction_ms - rand_ms) :
                        (rand_ms - reaction_ms);

        if (diff <= WIN_TOLERANCE_MS)
        {
            game_state = WON;
            Buzzer_Beep(BEEP_PRESS_MS);         // ถูก ดังสั้นๆ
        }
        else
        {
            game_state = LOST;
            Buzzer_Beep(BEEP_FALSE_START_MS);   // ไม่ถูก ดัง 1.5 วินาที
        }
    }
}

// ---------------- 7-Segment ----------------

void Display_Init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK |
                  SIM_SCGC5_PORTC_MASK;

    // Latch = PTB23
    PORTB->PCR[23] = PORT_PCR_MUX(1);

    PTB->PDDR |= MASK(23);

    // Clock = PTC3, Data = PTC12
    PORTC->PCR[3] = PORT_PCR_MUX(1);
    PORTC->PCR[12] = PORT_PCR_MUX(1);

    PTC->PDDR |= MASK(3) | MASK(12);

    PTB->PCOR = MASK(23);
    PTC->PCOR = MASK(3) | MASK(12);
}

void Shift_Out(uint8_t data)
{
    for (int i = 7; i >= 0; i--)
    {
        if (data & (1U << i))
            PTC->PSOR = MASK(12);
        else
            PTC->PCOR = MASK(12);

        PTC->PSOR = MASK(3);
        PTC->PCOR = MASK(3);
    }
}

void Display_Digit(uint8_t position, uint8_t number)
{
    PTB->PCOR = MASK(23);

    Shift_Out(SEGMENT_MAP[number]);
    Shift_Out(DIGIT_SELECT[position]);

    PTB->PSOR = MASK(23);
}

static void Display_Pattern(const uint8_t *map, uint8_t position)
{
    PTB->PCOR = MASK(23);

    Shift_Out(map[position]);
    Shift_Out(DIGIT_SELECT[position]);

    PTB->PSOR = MASK(23);
}

void Display_Refresh(void)
{
    static uint8_t position = 0;

    GameState state = game_state;

    if (state == IDLE)
    {
        Display_Pattern(PLAY_MAP, position);        // เปิดเครื่อง / พร้อมเล่น
    }
    else if (state == STARTING)
    {
        Display_Pattern(STAR_MAP, position);        // กด SW2 แล้วขึ้น STAR
    }
    else if (state == FALSE_START)
    {
        Display_Pattern(FAIL_MAP, position);        // กดก่อนสัญญาณ
    }
    else if (state == ENDED)
    {
        Display_Pattern(END_MAP, position);         // กด SW3 จบเกม
    }
    else if (state == WON)
    {
        Display_Pattern(GOOD_MAP, position);        // กดตรงเป้าหมาย
    }
    else if (state == LOST)
    {
        Display_Pattern(LOSE_MAP, position);        // กดไม่ตรง
    }
    else
    {
        // WAITING = โชว์เลขเป้าหมายที่สุ่มได้ค้างไว้
        // MEASURE = นับเวลาขึ้นสดจาก 0.000
        // แสดงเป็นวินาที รูปแบบ S.mmm (เช่น 3.472)
        uint32_t number;

        if (state == WAITING)
            number = rand_ms;
        else
            number = elapsed_ms;
        uint8_t digit;
        uint8_t seg;

        if (number > 9999)
            number = 9999;

        switch (position)
        {
            case 0:
                digit = (number / 1000) % 10;
                break;

            case 1:
                digit = (number / 100) % 10;
                break;

            case 2:
                digit = (number / 10) % 10;
                break;

            default:
                digit = number % 10;
                break;
        }

        seg = SEGMENT_MAP[digit];

        if (position == 0)
            seg &= 0x7F;        // เปิดจุดทศนิยมหลังหลักวินาที

        PTB->PCOR = MASK(23);

        Shift_Out(seg);
        Shift_Out(DIGIT_SELECT[position]);

        PTB->PSOR = MASK(23);
    }

    position++;

    if (position >= 4)
        position = 0;
}

// ---------------- Main ----------------

int main(void)
{
    Buzzer_Init();

    GPIO_Init();

    Display_Init();

    Timer_Init();

    last_press_ms = tick_ms - 200U;

    uint8_t prev_sw2 = 1;
    uint8_t prev_sw3 = 1;

    uint32_t sw2_time = 0;
    uint32_t sw3_time = 0;

    while (1)
    {
        uint32_t sw = PTB->PDIR;

        uint8_t sw2 = (sw & MASK(10)) ? 1 : 0;
        uint8_t sw3 = (sw & MASK(11)) ? 1 : 0;

        // SW2 = START
        if (prev_sw2 == 1 && sw2 == 0)
        {
            if ((uint32_t)(tick_ms - sw2_time) >= 200U)
            {
                sw2_time = tick_ms;

                if (game_state == IDLE)
                {
                    Game_Start();
                }
            }
        }

        // SW3 = RESET
        if (prev_sw3 == 1 && sw3 == 0)
        {
            if ((uint32_t)(tick_ms - sw3_time) >= 200U)
            {
                sw3_time = tick_ms;

                if (game_state != IDLE && game_state != ENDED)
                {
                    Game_End();
                }
            }
        }

        prev_sw2 = sw2;
        prev_sw3 = sw3;

        Display_Refresh();
    }
}
