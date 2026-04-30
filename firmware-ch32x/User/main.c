/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2025/03/11
 * Description        : Main program body.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

/*
 *@Note
 *
 * PD SRC Sample code
 *
 * This sample code may have compatibility issues and is for learning purposes only.
 *
 * Be sure to remove the pull-down resistors on both CC wires when using this Sample code!
 *
 * The inability to control the VBUS voltage on the board may lead to some compatibility problems,
 * mainly manifested in the inability of some devices to complete the PD communication process.
 *
 */

#include <string.h>
#include <stdlib.h>

#include "debug.h"
#include "PD_Process.h"
#include "i2c_lib.h"
#include "tps55289.h"
#include "lm75b.h"
#include "mp2762a.h"

/* ADC WWDG Mode Definition*/
#define NoSCAN_MODE_WDT   0
#define SCAN_MODE_WDT     1

/* ADC WWDG Mode Selection*/
#define ADC_MODE_WDT   NoSCAN_MODE_WDT

/* WWDG Reset Enable Definition */
#define WDT_RST_ENABLE   0
#define WDT_RST_DISABLE  1

/* WWDG Reset Enable Selection */
#define WDT_RST   WDT_RST_DISABLE

void TIM1_UP_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void ADC_Function_Init(void);
u16 Get_ADC_Val(u8 ch);
void USART2_Init(uint32_t baudrate);
void USART2_SendString(const char *s);
void Send_JSON_Status(void);
#if(Wake_up_mode==USBPDWake_up)
void USBPDWakeUp_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
#elif(Wake_up_mode==GPIOWake_up)
void EXTI15_8_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
#endif
volatile UINT8  Tim_Ms_Cnt = 0x00;
volatile UINT8  Led_Cnt = 0x00;
volatile UINT32 Uptime_Sec = 0;
static UINT16   Ms_Sub_Cnt = 0;
volatile u16    DC_Inp_ADC_Val = 0;
static UINT16   DC_Inp_Voltage_mV = 0;
static UINT16   Json_Timer_Ms = 0;
static UINT16   Temp_Timer_Ms = 0;
static UINT16   Chg_Timer_Ms = 0;
static UINT16   Tps_Timer_Ms = 0;
static UINT16   Bat_Timer_Ms = 0;
static int16_t  Board_Temp_c10 = 0;
static mp2762a_data_t Chg_Data = {0};
static int16_t  Tps_Vread_v10 = 0;
static int16_t  Tps_Iread_a10 = 0;
#define JSON_INTERVAL_MS 1000
#define TEMP_INTERVAL_MS 5000
#define CHG_INTERVAL_MS  2000
#define TPS_INTERVAL_MS  2000
#define BAT_INTERVAL_MS  500

/*********************************************************************/
void VBUS_enable(void)
{
//VBUS_OUT_EN = 1
	GPIO_WriteBit(GPIOA, GPIO_Pin_7, 1);
//PDS_EN = 1
	GPIO_WriteBit(GPIOB, GPIO_Pin_0, 1);
//PDC_SRC_TX = 1
    //GPIO_WriteBit(GPIOA, GPIO_Pin_2, 1);
//LED
//	GPIO_WriteBit(GPIOB, GPIO_Pin_12, 1);
}
/*********************************************************************/
void VBUS_disable(void)
{
//VBUS_OUT_EN = 0
	GPIO_WriteBit(GPIOA, GPIO_Pin_7, 0);
//PDS_EN = 1
	GPIO_WriteBit(GPIOB, GPIO_Pin_0, 1);
//PDC_SRC_TX = 0
    //GPIO_WriteBit(GPIOA, GPIO_Pin_2, 0);
//LED
//	GPIO_WriteBit(GPIOB, GPIO_Pin_12, 0);
//tps55289 disable
	tps55289_enable_output(0);
}
/*********************************************************************/
void VBUS_set_5V(void)
{
	VBUS_enable();
//tps55289 - set 5.1V / 3A
	tps55289_set_cdc_compensation(CDC_COMP_0V7);
	tps55289_set_current_limit(3.0);
	tps55289_set_voltage(5.1);
	tps55289_enable_output(1);
}
/*********************************************************************
 * @fn      GPIO_Port_Init
 *
 * @brief   Initializes GPIO
 *
 * @return  none
 */
void GPIO_Port_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

//port A
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
//DC_INP_EN_SRC = 0
    GPIO_WriteBit(GPIOA, GPIO_Pin_6, 0);
//VBUS_OUT_EN = 0
	GPIO_WriteBit(GPIOA, GPIO_Pin_7, 0);

//port B
//PB0: PDS_EN
//PB3: PDC_CC_SEL
//PB12: PDC_SRC_STAT (LED)
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_3 | GPIO_Pin_12;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

/* PB11: PDC_CC_DET (input, pull-up) */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

//PDS_EN = 1
    GPIO_WriteBit(GPIOB, GPIO_Pin_0, 1);
//PDC_SRC_STAT (LED) = 1
    GPIO_WriteBit(GPIOB, GPIO_Pin_12, 0);
//PDC_CC_SEL = 0
	GPIO_WriteBit(GPIOB, GPIO_Pin_3, 0);
//CC MUX = 1
//	GPIO_WriteBit(GPIOB, GPIO_Pin_3, 1);
}
/*********************************************************************/

void Disable_SDI(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC|RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_IO2W, ENABLE);
//PC18 PC19
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_18|GPIO_Pin_19;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

}
/*********************************************************************
 * @fn      TIM1_Init
 *
 * @brief   Initialize TIM1
 *
 * @return  none
 */
void TIM1_Init( u16 arr, u16 psc )
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure={0};
    NVIC_InitTypeDef NVIC_InitStructure={0};
    RCC_APB2PeriphClockCmd( RCC_APB2Periph_TIM1, ENABLE );
    TIM_TimeBaseInitStructure.TIM_Period = arr;
    TIM_TimeBaseInitStructure.TIM_Prescaler = psc;
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0x00;
    TIM_TimeBaseInit( TIM1, &TIM_TimeBaseInitStructure);
    TIM_ClearITPendingBit( TIM1, TIM_IT_Update );
    NVIC_InitStructure.NVIC_IRQChannel = TIM1_UP_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 3;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    TIM_ITConfig( TIM1, TIM_IT_Update , ENABLE );
    TIM_Cmd( TIM1, ENABLE );
}

/*********************************************************************
 * @fn      EXTI_INIT
 *
 * @brief   Initialize Wake up EXTI
 *
 * @return  none
 */
void EXTI_INIT(void)
{
#if(Wake_up_mode==USBPDWake_up)
    EXTI_InitTypeDef EXTI_InitStructure = {0};
    EXTI_InitStructure.EXTI_Line = EXTI_Line29;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);
#elif(Wake_up_mode==GPIOWake_up)
    EXTI_InitTypeDef EXTI_InitStructure = {0};
    /* GPIOC.14 ----> EXTI_Line14 */
    GPIO_EXTILineConfig(GPIO_PortSourceGPIOC, GPIO_PinSource14);
    EXTI_InitStructure.EXTI_Line = EXTI_Line14;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    /* GPIOC.15 ----> EXTI_Line15 */
    GPIO_EXTILineConfig(GPIO_PortSourceGPIOC, GPIO_PinSource15);
    EXTI_InitStructure.EXTI_Line = EXTI_Line15;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);
#endif

}
/*********************************************************************
 * @fn      USART2_Init
 *
 * @brief   Initialize USART2 on PA2 (TX to RP2040).
 *
 * @return  none
 */
void USART2_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef  GPIO_InitStructure = {0};
    USART_InitTypeDef USART_InitStructure = {0};
    NVIC_InitTypeDef  NVIC_InitStructure = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    /* PA2: USART2_TX */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* PA3: USART2_RX */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = baudrate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;

    USART_Init(USART2, &USART_InitStructure);

    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(USART2, ENABLE);
}

/*********************************************************************
 * @fn      USART2_SendString
 *
 * @brief   Send null-terminated string via USART2.
 *
 * @return  none
 */
void USART2_SendString(const char *s)
{
    while (*s)
    {
        while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
        USART_SendData(USART2, *s++);
    }
}

/*********************************************************************
 * USART2 RX ring buffer + JSON command framing.
 *
 * Frames are brace-balanced, single-line: {"cmd":"<name>","id":<int>}\n
 * The host (RP2040 forwarding from the Pi service) pushes commands; we
 * push back a response or push the status JSON on demand.
 */
#define UART_RX_BUF_SIZE  128
#define UART_RX_LINE_SIZE 128

static volatile uint8_t  Uart_Rx_Buf[UART_RX_BUF_SIZE];
static volatile uint16_t Uart_Rx_Head = 0;
static volatile uint16_t Uart_Rx_Tail = 0;

void USART2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint8_t c = (uint8_t)USART_ReceiveData(USART2); /* clears RXNE */
        uint16_t next = (uint16_t)((Uart_Rx_Head + 1) % UART_RX_BUF_SIZE);
        if (next != Uart_Rx_Tail)
        {
            Uart_Rx_Buf[Uart_Rx_Head] = c;
            Uart_Rx_Head = next;
        }
        /* else: buffer full, drop byte. Main loop is too slow if this happens. */
    }
}

static int Uart_Rx_Pop(uint8_t *out)
{
    if (Uart_Rx_Head == Uart_Rx_Tail) return 0;
    *out = Uart_Rx_Buf[Uart_Rx_Tail];
    Uart_Rx_Tail = (uint16_t)((Uart_Rx_Tail + 1) % UART_RX_BUF_SIZE);
    return 1;
}

/* Power-cycle state machine: VBUS off -> wait POWERCYCLE_OFF_MS -> VBUS on. */
typedef enum {
    POWERCYCLE_IDLE = 0,
    POWERCYCLE_OFF_WAIT
} powercycle_state_t;

static powercycle_state_t Powercycle_State = POWERCYCLE_IDLE;
static UINT16 Powercycle_Timer_Ms = 0;
#define POWERCYCLE_OFF_MS 1500

static int Json_Get_Int(const char *json, const char *key, int *out)
{
    char search[24];
    int n = snprintf(search, sizeof(search), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(search)) return 0;

    const char *p = strstr(json, search);
    if (!p) return 0;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return 0;
    *out = atoi(p);
    return 1;
}

static int Json_Get_Str(const char *json, const char *key, char *out, int max_len)
{
    char search[24];
    int n = snprintf(search, sizeof(search), "\"%s\":\"", key);
    if (n <= 0 || n >= (int)sizeof(search)) return 0;

    const char *p = strstr(json, search);
    if (!p) return 0;
    p += n;
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return i;
}

static void Cmd_Send_Resp(const char *cmd, int id, int ok, const char *err)
{
    char buf[96];
    if (err)
    {
        snprintf(buf, sizeof(buf),
                 "{\"resp\":\"%s\",\"id\":%d,\"ok\":%d,\"err\":\"%s\"}\r\n",
                 cmd, id, ok, err);
    }
    else
    {
        snprintf(buf, sizeof(buf),
                 "{\"resp\":\"%s\",\"id\":%d,\"ok\":%d}\r\n",
                 cmd, id, ok);
    }
    USART2_SendString(buf);
}

static void Cmd_Dispatch(const char *json)
{
    char cmd[32] = {0};
    int  id = 0;

    Json_Get_Int(json, "id", &id);
    if (!Json_Get_Str(json, "cmd", cmd, sizeof(cmd)))
    {
        Cmd_Send_Resp("?", id, 0, "no_cmd");
        return;
    }

    if (strcmp(cmd, "ping") == 0)
    {
        Cmd_Send_Resp(cmd, id, 1, NULL);
    }
    else if (strcmp(cmd, "ups.status") == 0)
    {
        Cmd_Send_Resp(cmd, id, 1, NULL);
        Send_JSON_Status();
    }
    else if (strcmp(cmd, "ups.power.disable") == 0)
    {
        VBUS_disable();
        Cmd_Send_Resp(cmd, id, 1, NULL);
    }
    else if (strcmp(cmd, "ups.power.enable") == 0)
    {
        VBUS_set_5V();
        Cmd_Send_Resp(cmd, id, 1, NULL);
    }
    else if (strcmp(cmd, "ups.power.cycle") == 0)
    {
        VBUS_disable();
        Powercycle_Timer_Ms = 0;
        Powercycle_State = POWERCYCLE_OFF_WAIT;
        Cmd_Send_Resp(cmd, id, 1, NULL);
    }
    else if (strcmp(cmd, "ups.reset") == 0)
    {
        Cmd_Send_Resp(cmd, id, 1, NULL);
        Delay_Ms(50); /* let TX FIFO drain before reset */
        NVIC_SystemReset();
    }
    else
    {
        Cmd_Send_Resp(cmd, id, 0, "unknown");
    }
}

/* Drain the RX ring, accumulate bytes between '{' and matching '}', dispatch. */
static void Cmd_Rx_Drain(void)
{
    static char    rx_line[UART_RX_LINE_SIZE];
    static uint8_t rx_idx = 0;
    static uint8_t rx_in_json = 0;

    uint8_t c;
    while (Uart_Rx_Pop(&c))
    {
        if (c == '{')
        {
            rx_idx = 0;
            rx_line[rx_idx++] = (char)c;
            rx_in_json = 1;
        }
        else if (rx_in_json)
        {
            if (rx_idx >= UART_RX_LINE_SIZE - 1)
            {
                rx_idx = 0;
                rx_in_json = 0;
                continue; /* line too long: discard frame */
            }
            rx_line[rx_idx++] = (char)c;
            if (c == '}')
            {
                rx_line[rx_idx] = '\0';
                rx_in_json = 0;
                rx_idx = 0;
                Cmd_Dispatch(rx_line);
            }
        }
        /* else: byte outside frame -> ignore (no debug stream from RP2040) */
    }
}

/*********************************************************************
 * @fn      Send_JSON_Status
 *
 * @brief   Build and send JSON status message via USART2 to RP2040.
 *
 * @return  none
 */
static char json_buf[300];

void Send_JSON_Status(void)
{
    sprintf(json_buf,
        "{\"up\":%lu,\"pd\":%d,\"pdo\":%d,\"cc\":%d,\"role\":%d,"
        "\"snk_ok\":%d,\"snk_v\":%d,\"snk_i\":%d,"
        "\"t\":%d,"
        "\"vs\":%d,\"is\":%d,\"vr\":%d,\"ir\":%d,"
        "\"bp\":%d,\"bp2\":%d,\"cs\":%d,\"pg\":%d,"
        "\"vi\":%d,\"ii\":%d,\"vb\":%d,\"ci\":%d,\"cf\":%d}\r\n",
        (unsigned long)Uptime_Sec,
        (int)PD_Ctl.PD_State,
        (int)PD_Get_SinkReqPDOIndex(),
        (int)PD_Ctl.Flag.Bit.Connected,
        (int)PD_Get_Role(),
        (int)PD_ProfileAccepted_Get(),
        (int)PD_Get_Snk_Voltage_100mV(),
        (int)PD_Get_Snk_Current_100mA(),
        (int)Board_Temp_c10,
        (int)tps55289_get_voltage_set_v10(),
        (int)tps55289_get_current_set_a10(),
        (int)Tps_Vread_v10,
        (int)Tps_Iread_a10,
        (int)(!mp2762a_is_battery_uvlo()),
        (int)mp2762a_is_battery_present(),
        (int)Chg_Data.chg_state,
        (int)Chg_Data.power_good,
        (int)Chg_Data.vin_mv,
        (int)Chg_Data.iin_ma,
        (int)Chg_Data.vbat_mv,
        (int)Chg_Data.ichg_ma,
        (int)Chg_Data.fault
    );
    USART2_SendString(json_buf);
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */
int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(921600);
    //printf( "SystemClk:%d\r\n", SystemCoreClock );
    //printf( "ChipID:%08x\r\n", DBGMCU_GetCHIPID() );
    //printf( "PD SRC TEST\r\n" );
    Disable_SDI();
    PD_Init( );
    GPIO_Port_Init();
    ADC_Function_Init();
    I2C_init();
    Delay_Ms(10);  // Wait for TPS55289 to stabilize after PDS_EN=1
    tps55289_init();
    mp2762a_init();

    EXTI_INIT();
    TIM1_Init( 999, 48-1);
    USART2_Init(921600);

    while(1)
    {
        /* Get the calculated timing interval value */
        TIM_ITConfig( TIM1, TIM_IT_Update , DISABLE );
        Tmr_Ms_Dlt = Tim_Ms_Cnt - Tmr_Ms_Cnt_Last;
        Tmr_Ms_Cnt_Last = Tim_Ms_Cnt;
        //Tmr_Ms_Dlt = 1;
        TIM_ITConfig( TIM1, TIM_IT_Update , ENABLE );

		/* role manager tick: uses Tmr_Ms_Dlt */
        PD_Role_Manager_Tick();

        /* Drain UART RX ring and dispatch any complete command frames. */
        Cmd_Rx_Drain();

        /* Power-cycle state machine: re-enable VBUS after off window. */
        if (Powercycle_State == POWERCYCLE_OFF_WAIT)
        {
            Powercycle_Timer_Ms += Tmr_Ms_Dlt;
            if (Powercycle_Timer_Ms >= POWERCYCLE_OFF_MS)
            {
                VBUS_set_5V();
                Powercycle_State = POWERCYCLE_IDLE;
            }
        }

        PD_Ctl.Det_Timer += Tmr_Ms_Dlt;
        if( PD_Ctl.Det_Timer > 4 )
        {
            PD_Ctl.Det_Timer = 0;
            PD_Det_Proc( );
        }
        PD_Main_Proc( );

        /* Periodic temperature reading from LM75B every 5s */
        Temp_Timer_Ms += Tmr_Ms_Dlt;
        if (Temp_Timer_Ms >= TEMP_INTERVAL_MS)
        {
            Temp_Timer_Ms = 0;
            Board_Temp_c10 = (int16_t)(lm75b_read_temp_c() * 10.0f);
        }

        /* Read TPS55289 voltage/current every 2s */
        Tps_Timer_Ms += Tmr_Ms_Dlt;
        if (Tps_Timer_Ms >= TPS_INTERVAL_MS)
        {
            Tps_Timer_Ms = 0;
            Tps_Vread_v10 = (int16_t)(tps55289_read_voltage() * 10.0f);
            Tps_Iread_a10 = (int16_t)(tps55289_read_current_limit() * 10.0f);
        }

        /* Read MP2762A charger status every 2s + kick watchdog */
        Chg_Timer_Ms += Tmr_Ms_Dlt;
        if (Chg_Timer_Ms >= CHG_INTERVAL_MS)
        {
            Chg_Timer_Ms = 0;
            mp2762a_read_all(&Chg_Data);
            mp2762a_kick_watchdog();
        }

        /* Check battery insertion/removal every 500ms */
        Bat_Timer_Ms += Tmr_Ms_Dlt;
        if (Bat_Timer_Ms >= BAT_INTERVAL_MS)
        {
            Bat_Timer_Ms = 0;
            mp2762a_poll_battery();
            if (mp2762a_battery_inserted()) {
                mp2762a_restart_charging();
            }
            /* Unconditional fault recovery - don't gate on battery presence.
             * Without this, a latched fault from battery removal prevents the
             * charger from restarting when battery is re-inserted (deadlock). */
            if (Chg_Data.fault) {
                mp2762a_restart_charging();
                Chg_Data.fault = 0;
            }
        }

        /* Periodic JSON status output via USART2 */
        Json_Timer_Ms += Tmr_Ms_Dlt;
        if (Json_Timer_Ms >= JSON_INTERVAL_MS)
        {
            Json_Timer_Ms = 0;

            /* DC input voltage from cached PA1 ADC: Vin = ADC * 3300 * (27.4+5.1) / 5.1 / 4096 */
            DC_Inp_Voltage_mV = (UINT16)((UINT32)DC_Inp_ADC_Val * 21029 / 4096);

            Send_JSON_Status();
        }
    }
}

/*********************************************************************
 * @fn      TIM1_UP_IRQHandler
 *
 * @brief   This function handles TIM1 interrupt.
 *
 * @return  none
 */
void TIM1_UP_IRQHandler(void)
{
    if( TIM_GetITStatus( TIM1, TIM_IT_Update ) != RESET )
    {
        Tim_Ms_Cnt++;
        TIM_ClearITPendingBit( TIM1, TIM_IT_Update );

        Ms_Sub_Cnt++;
        if (Ms_Sub_Cnt >= 1000) { Ms_Sub_Cnt = 0; Uptime_Sec++; }

        Led_Cnt++;

        u16 ADC_val;
        ADC_val = Get_ADC_Val(ADC_Channel_1);
        DC_Inp_ADC_Val = ADC_val;
        //Vin >= 10V
        if (ADC_val >= 0x793)
        {
            //LED blink 2x
            if (Led_Cnt & 0x40)
            {
                GPIO_WriteBit(GPIOB, GPIO_Pin_12, 1);
            }
            else
            {
                GPIO_WriteBit(GPIOB, GPIO_Pin_12, 0);
            }
            //DC_INP_EN_SRC = 1
            GPIO_WriteBit(GPIOA, GPIO_Pin_6, 1);
            return;
        }
        //Vin <= 9V
        if (ADC_val <= 0x6d1)
        {
            //DC_INP_EN_SRC = 0
            GPIO_WriteBit(GPIOA, GPIO_Pin_6, 0);

            //LED blink
            if (Led_Cnt & 0x80)
            {
                GPIO_WriteBit(GPIOB, GPIO_Pin_12, 1);
            }
            else
            {
                GPIO_WriteBit(GPIOB, GPIO_Pin_12, 0);
            }
        }
    }
}

/*********************************************************************
 * @fn      EXTI15_8_IRQHandler
 *
 * @brief   This function handles EXTI14 and EXTI15 exception.
 *
 * @return  none
 */
void EXTI15_8_IRQHandler(void)
{
  if(EXTI_GetITStatus(EXTI_Line14)!=RESET)
  {
      SystemInit();
      printf(" GPIO Wake_up\r\n");
      EXTI_ClearITPendingBit(EXTI_Line14);     /* Clear Flag */
      NVIC_DisableIRQ(EXTI15_8_IRQn);
  }
  if(EXTI_GetITStatus(EXTI_Line15)!=RESET)
  {
      SystemInit();
      printf(" GPIO Wake_up\r\n");
      EXTI_ClearITPendingBit(EXTI_Line15);     /* Clear Flag */
      NVIC_DisableIRQ(EXTI15_8_IRQn);
    }

}

/*********************************************************************
 * @fn      ADC1_IRQHandler
 *
 * @brief   ADC1 Interrupt Service Function.
 *
 * @return  none
 */
void ADC1_IRQHandler()
{
    if(ADC_GetITStatus( ADC1, ADC_IT_AWD)){
        printf( "Enter AnalogWatchdog Interrupt\r\n" );
    }

    ADC_ClearITPendingBit( ADC1, ADC_IT_AWD);
}

/*********************************************************************
 * @fn      Get_ADC_Val
 *
 * @brief   Returns ADCx conversion result data.
 *
 * @param   ch - ADC channel.
 *
 * @return  ADC conversion value
 */
u16 Get_ADC_Val(u8 ch)
{
    u16 val;

    ADC_RegularChannelConfig(ADC1, ch, 1, ADC_SampleTime_11Cycles);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);

    while(!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC));

    val = ADC_GetConversionValue(ADC1);

    return val;
}

/*********************************************************************
 * @fn      ADC_Function_Init
 *
 * @brief   Initializes ADC collection for PA1 (DC_INP_ADC_SRC).
 *
 * @return  none
 */
void ADC_Function_Init(void)
{
    ADC_InitTypeDef  ADC_InitStructure = {0};
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    NVIC_InitTypeDef NVIC_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    //PA1: DC_INP_ADC_SRC
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    //PA5: input floating (battery ADC divider - not used, MP2762A provides VBAT)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    ADC_DeInit(ADC1);

    ADC_CLKConfig(ADC1, ADC_CLK_Div6);

    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode = DISABLE;
    ADC_InitStructure.ADC_NbrOfChannel = 1;
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_Init(ADC1, &ADC_InitStructure);

    //ADC_Channel_1 = PA1
    ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 1, ADC_SampleTime_11Cycles);
    ADC_AnalogWatchdogSingleChannelConfig(ADC1, ADC_Channel_1);
    ADC_AnalogWatchdogCmd(ADC1, ADC_AnalogWatchdog_SingleRegEnable);

    /* Higher Threshold:3500, Lower Threshold:2000 */
    ADC_AnalogWatchdogThresholdsConfig(ADC1, 3500, 2000);

    ADC_AnalogWatchdogResetCmd(ADC1, ADC_AnalogWatchdog_0_RST_EN, DISABLE);

    NVIC_InitStructure.NVIC_IRQChannel = ADC1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    ADC_ITConfig(ADC1, ADC_IT_AWD, ENABLE);
    ADC_Cmd(ADC1, ENABLE);
}

/*********************************************************************
 * @fn      USBPDWakeUp_IRQHandler
 *
 * @brief   This function handles USBPD WakeUp exception.
 *
 * @return  none
 */
void USBPDWakeUp_IRQHandler(void)
{
  if(EXTI_GetITStatus(EXTI_Line29)!=RESET)
  {
      SystemInit();
      EXTI_ClearITPendingBit(EXTI_Line29);     /* Clear Flag */
      NVIC_DisableIRQ(USBPDWakeUp_IRQn);
      USBPD->CONFIG&=~IE_PD_IO;
      printf("USBPDWakeUp\r\n");
      USBPD->PORT_CC1&=~(CC_PU_Mask);
      USBPD->PORT_CC2&=~(CC_PU_Mask);
      USBPD->PORT_CC1|=CC_PU_330;
      USBPD->PORT_CC2|=CC_PU_330;
  }
}

