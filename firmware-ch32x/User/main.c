/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Project            : Web3 Pi UPS v2 — CH32X035 PD SRC controller firmware
 * Author             : Robert Mordzon (Web3 Pi)
 * Based on           : WCH "PD SRC Sample code" V1.0.1 (2025/03/11)
 * Description        : Main program body — dual-role USB-PD, TPS55289 VBUS
 *                      control, MP2762A charger telemetry, LM75B temperature,
 *                      and Web3 Pi UPS binary wire protocol v1 over USART2.
 *********************************************************************************
 * Copyright (c) 2026 Robert Mordzon / Web3 Pi
 * Portions Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 *
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
#include "wups_proto.h"

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

/* Diagnostic mode: skip USB-PD negotiation entirely. From boot, force
 * TPS55289 to 5.1 V / 5 A and hold VBUS_OUT_EN high. PD_Init() still
 * runs so the CC-line Rp pull-ups exist (a sink will detect us as a
 * non-PD 5 V source and pull as much current as its e-load asks for —
 * up to 5 A from the TPS limit). Uncomment the define to bypass PD
 * negotiation again. */
/* #define DIAG_FORCE_5V_5A_NO_PD */

void TIM1_UP_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void ADC_Function_Init(void);
u16 Get_ADC_Val(u8 ch);
void USART2_Init(uint32_t baudrate);
void USART2_SendString(const char *s);
static void Usart2_Dma_Rx_Init(void);
static void wups_send_power_status(uint8_t dst, uint8_t flags, uint8_t seq);
static void wups_send_hello_bcast(void);
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
static UINT16   Diag_Log_Timer_Ms = 0;
#define DIAG_LOG_INTERVAL_MS 5000
static int16_t  Board_Temp_c10 = 0;
static mp2762a_data_t Chg_Data = {0};
static int16_t  Tps_Vread_v10 = 0;
static int16_t  Tps_Iread_a10 = 0;
/* TPS55289 STATUS register bits — accumulated across polls. SCP/OCP/OVP
 * latch in hardware until the register is read. We OR each poll's read
 * into Tps_Status_Latched so a brief trip is not lost between status
 * frames; the latch is cleared once a power.status frame ships out. */
static uint8_t  Tps_Status_Latched = 0;
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
	/* Clear cached telemetry so the JSON status reflects "off" rather
	 * than the last negotiated 15V/1.8A. Otherwise vs/is/vr/ir/pdo would
	 * keep showing the pre-disable contract and the UI/host would have
	 * no way to tell the rail is actually down. */
	tps55289_clear_set_cache();
	Tps_Vread_v10 = 0;
	Tps_Iread_a10 = 0;
	PD_Ctl.ReqPDO_Idx = 0;
}
/*********************************************************************/
void VBUS_set_5V(void)
{
	VBUS_enable();
//tps55289 - set 5.1V / 3A
	tps55289_set_cdc_compensation(CDC_COMP_0V7);
	tps55289_set_current_limit(3.0);
	tps55289_set_voltage(5.0);
	tps55289_enable_output(1);
}

/* Re-arm the PD source FSM for a fresh negotiation. Without this the
 * FSM stays at STA_IDLE post-PS_RDY and a sink that previously negotiated
 * >5V (e.g. our 15V test load) just sees VBUS_set_5V come back at 5.1V
 * with no new SRC_CAP and no way to renegotiate.
 *
 * Skip STA_DISCONNECT path: that case calls PD_PHY_Reset() every tick,
 * which appears to drop the Rp pull-up on CC. With Rp gone, PD_Det_Proc
 * never sees the still-attached sink (CC reads as floating) and the FSM
 * gets stuck — confirmed in test logs where vo stayed 0 after enable.
 *
 * Push the FSM straight to STA_SINK_CONNECT with timers reset. Its 159 ms
 * dwell drops us into STA_TX_SRC_CAP which broadcasts a fresh SRC_CAP.
 * The sink, having just lost VBUS, treats the new SRC_CAP as a clean
 * renegotiation and requests its preferred PDO. */
static void Power_Output_Restart(void)
{
	VBUS_set_5V();
	PD_Ctl.PD_State = STA_SINK_CONNECT;
	PD_Ctl.PD_Comm_Timer = 0;
	PD_Ctl.Src_Cap_Cnt = 0;
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

    /* DMA-based RX: configure channel and route USART2 RX requests to it. */
    Usart2_Dma_Rx_Init();
    USART_DMACmd(USART2, USART_DMAReq_Rx, ENABLE);

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
 * USART2 RX ring buffer.
 *
 * DMA1 Channel 6 fills `Dma_Rx_Buf` in circular mode regardless of CPU
 * activity (USBPD IRQ etc.), so even tens-of-microsecond stalls cannot
 * cause overruns. The deframer (further down in this file) is fed one
 * byte at a time by `Cmd_Rx_Drain()` which polls the DMA write head.
 */

/* DMA1 Channel 6 receives USART2 RX bytes into this circular buffer. The DMA
 * controller writes incoming bytes regardless of CPU activity (USBPD IRQ,
 * etc.), so even tens-of-microsecond CPU stalls cannot cause overruns. The
 * main loop polls DMA1_Channel6->CNTR to find the current write head and
 * drains anything new since the last pass. */
#define DMA_RX_BUF_SIZE 256
static volatile uint8_t  Dma_Rx_Buf[DMA_RX_BUF_SIZE];
static volatile uint16_t Dma_Rx_Tail = 0;
/* Lightweight monitoring counters surfaced in status JSON. */
static volatile uint32_t Uart_Rx_Total = 0;         /* total RX bytes drained from DMA */
static volatile uint32_t Cmd_Frames_Dispatched = 0; /* binary frames successfully deframed */
static volatile uint32_t Uart_Ore_Count = 0;        /* USART2 overrun errors (should stay 0) */

/* DMA-based RX: enabled by Usart2_Dma_Rx_Init(). USART2_IRQHandler is no
 * longer used — RXNE/ORE flags are handled implicitly by the DMA controller. */
static void Usart2_Dma_Rx_Init(void)
{
    DMA_InitTypeDef DMA_InitStructure = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    DMA_DeInit(DMA1_Channel6);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)(&USART2->DATAR);
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)Dma_Rx_Buf;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_BufferSize = DMA_RX_BUF_SIZE;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel6, &DMA_InitStructure);
    DMA_Cmd(DMA1_Channel6, ENABLE);
}

/* Power-cycle state machine: VBUS off -> wait POWERCYCLE_OFF_MS -> VBUS on. */
typedef enum {
    POWERCYCLE_IDLE = 0,
    POWERCYCLE_OFF_WAIT
} powercycle_state_t;

static powercycle_state_t Powercycle_State = POWERCYCLE_IDLE;
static UINT16 Powercycle_Timer_Ms = 0;
#define POWERCYCLE_OFF_MS 1500

/*********************************************************************
 * Web3 Pi UPS — binary wire protocol v1 (CH32X side).
 *
 * Replaces the earlier brace-balanced JSON command/status path. Spec:
 * Web3-Pi-UPS/common/protocol_desc.md. CH32X is a leaf node at
 * address WUPS_ADDR_CH32X, connected to RP2040 over USART2 @ 921600.
 *
 * Outbound:
 *   - power.status (CLASS=0x02 OP=0x01) every 1 s as EVENT, unicast to RP2040.
 *   - system.hello broadcast on boot.
 *   - power.event broadcast on state change (helper provided, wiring TBD).
 *
 * Inbound:
 *   - system.ping (REQ) -> system.ping (RESP) with uptime + fw_version.
 *   - system.status_query (REQ) -> power.status (RESP).
 *   - power.{enable,disable,cycle,reset} (REQ).
 *   - All other classes / ops are silently ignored — CH32X is a leaf
 *     and only owns the POWER + SYSTEM classes.
 */

/* Synchronous TX byte sender. The PD stack also writes ASCII printf
 * strings to USART2 (DEBUG = DEBUG_UART2). Both writers run from the
 * main thread, so they serialize at byte granularity. ASCII bytes
 * appearing between binary frames are harmless: the receiver state
 * machine stays in SYNC1 until the next 0xAA 0x55 sequence. */
static void Usart2_Send_Bytes(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i)
    {
        while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
        USART_SendData(USART2, buf[i]);
    }
}

/* Single TX sequence counter for this leaf node. Receivers use SRC+SEQ
 * to demultiplex; we don't track per-destination sequences here. */
static uint8_t Wups_Tx_Seq = 0;

static void wups_send_frame(uint8_t dst, uint8_t cls, uint8_t op,
                            uint8_t flags, uint8_t seq,
                            const void *payload, uint16_t payload_len)
{
    uint8_t header[10];
    header[0] = WUPS_SYNC1;
    header[1] = WUPS_SYNC2;
    header[2] = dst;
    header[3] = WUPS_ADDR_CH32X;
    header[4] = cls;
    header[5] = op;
    header[6] = flags;
    header[7] = seq;
    header[8] = (uint8_t)(payload_len & 0xFFu);
    header[9] = (uint8_t)((payload_len >> 8) & 0xFFu);

    /* Fletcher-8 over DST..LEN_H..payload. SYNC and end marker excluded. */
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 10; ++i)
    {
        a = (uint8_t)(a + header[i]);
        b = (uint8_t)(b + a);
    }
    const uint8_t *p = (const uint8_t *)payload;
    for (uint16_t i = 0; i < payload_len; ++i)
    {
        a = (uint8_t)(a + p[i]);
        b = (uint8_t)(b + a);
    }
    uint8_t trailer[4] = { a, b, WUPS_END1, WUPS_END2 };

    Usart2_Send_Bytes(header, 10);
    if (payload_len) Usart2_Send_Bytes(p, payload_len);
    Usart2_Send_Bytes(trailer, 4);
}

/* Compose power.status from current globals and send to `dst`.
 * Used both for the 1 Hz periodic EVENT (dst=RP2040) and as RESP to
 * system.status_query (dst=requester). */
static void wups_send_power_status(uint8_t dst, uint8_t flags, uint8_t seq)
{
    wups_power_status_v1_t s;
    s.version        = 1;
    s.charge_state   = (uint8_t)Chg_Data.chg_state;
    s.vbus_in_mV     = (uint16_t)Chg_Data.vin_mv;
    /* Tps_Vread_v10 / Tps_Iread_a10 are 0.1 V / 0.1 A units — *100 → mV / mA. */
    s.vbus_out_mV    = (uint16_t)((int32_t)Tps_Vread_v10 * 100);
    s.ibus_out_mA    = (int16_t)((int32_t)Tps_Iread_a10 * 100);
    s.vbat_mV        = (uint16_t)Chg_Data.vbat_mv;
    s.ibat_mA        = (int16_t)Chg_Data.ichg_ma;
    /* Hottest of LM75B (board, near TPS55289) and MP2762A junction.
     * "Worst-of" is the right number for thermal protection logic and OLED
     * display — if either chip is approaching its safe limit, that's what
     * we want surfaced. Both inputs are in deci-Celsius. */
    {
        int16_t lm = Board_Temp_c10;
        int16_t tj = Chg_Data.tjunc_c10;
        s.temp_dC = (tj > lm) ? tj : lm;
    }
    /* SINK contract is 0 unless the UPS is being fed by an upstream PD
     * charger. While diagnosing the SOURCE-side power path with no upstream
     * PD, repurpose `pd_contract_mV` as VSYS (mV) and `pd_contract_mA` as
     * IIN (mA from MP2762A) — both are critical to diagnose TPS55289 UVLO
     * trips that don't show up in STATUS register. Once the SINK side is
     * also exercised this aliasing has to go (real fix: protocol v2 with
     * dedicated diag fields). */
    if (PD_Get_Snk_Voltage_100mV() != 0)
    {
        s.pd_contract_mV = (uint16_t)((uint32_t)PD_Get_Snk_Voltage_100mV() * 100u);
        s.pd_contract_mA = (uint16_t)((uint32_t)PD_Get_Snk_Current_100mA() * 100u);
    }
    else
    {
        s.pd_contract_mV = (uint16_t)Chg_Data.vsys_mv;
        s.pd_contract_mA = (uint16_t)Chg_Data.iin_ma;
    }
    /* Pack TPS55289 STATUS bits into the high byte of `faults` so the
     * MP2762A fault byte (low byte) is preserved. The host can see what
     * tripped the converter without bumping the protocol version:
     *   bit  8 (TPS SCP)  ← STATUS bit 7
     *   bit  9 (TPS OCP)  ← STATUS bit 6
     *   bit 10 (TPS OVP)  ← STATUS bit 5
     */
    uint16_t tps_flags = 0;
    if (Tps_Status_Latched & STATUS_SCP_BIT) tps_flags |= (1u << 8);
    if (Tps_Status_Latched & STATUS_OCP_BIT) tps_flags |= (1u << 9);
    if (Tps_Status_Latched & STATUS_OVP_BIT) tps_flags |= (1u << 10);
    s.faults = (uint16_t)(Chg_Data.fault | tps_flags);
    /* Clear the latch only after the bits have been packed into the
     * frame about to ship — otherwise a trip between read and frame
     * emission would be lost. */
    Tps_Status_Latched = 0;

    wups_send_frame(dst, WUPS_CLASS_POWER, WUPS_OP_PWR_STATUS,
                    flags, seq, &s, sizeof(s));
}

/* system.log broadcast helper. Header layout per protocol.h:
 *   uint8_t version = 1
 *   uint8_t level   (0=trace 1=debug 2=info 3=warn 4=error)
 *   uint8_t text_len
 *   uint8_t reserved = 0
 *   text[text_len]
 * RP2040 hub forwards the text body to its debug stream (USB-CDC + Probe
 * UART), which gives us a remote read-back channel without needing to
 * solder a debug UART pin on CH32X. Used for raw register dumps. */
static void wups_send_log(uint8_t level, const char *text)
{
    uint16_t text_len = 0;
    while (text[text_len] && text_len < (WUPS_MAX_PAYLOAD - 4)) text_len++;
    uint8_t buf[WUPS_MAX_PAYLOAD];
    buf[0] = 1;
    buf[1] = level;
    buf[2] = (uint8_t)text_len;
    buf[3] = 0;
    for (uint16_t i = 0; i < text_len; ++i) buf[4 + i] = (uint8_t)text[i];
    wups_send_frame(WUPS_ADDR_BROADCAST, WUPS_CLASS_SYSTEM, WUPS_OP_SYS_LOG,
                    WUPS_FLAG_EVENT, Wups_Tx_Seq++, buf, (uint16_t)(4 + text_len));
}

/* system.hello broadcast — emitted once at boot. */
static void wups_send_hello_bcast(void)
{
    wups_sys_hello_v1_t h;
    h.version       = 1;
    h.proto_version = WUPS_PROTO_VERSION;
    h.node_addr     = WUPS_ADDR_CH32X;
    h.reserved      = 0;
    h.fw_version    = (uint16_t)((1u << 8) | 0u); /* 1.0 — bump on release */
    h.caps_classes  = WUPS_CAP_SYSTEM | WUPS_CAP_POWER;
    h.build_id      = 0;
    wups_send_frame(WUPS_ADDR_BROADCAST, WUPS_CLASS_SYSTEM,
                    WUPS_OP_SYS_HELLO, WUPS_FLAG_EVENT, Wups_Tx_Seq++,
                    &h, sizeof(h));
}

/* power.event broadcast helper. Not yet wired into a detector — kept
 * here so the event taxonomy is callable when we add edge detection. */
__attribute__((unused))
static void wups_send_power_event(uint8_t event)
{
    wups_power_event_v1_t e;
    e.version = 1;
    e.event   = event;
    wups_send_frame(WUPS_ADDR_BROADCAST, WUPS_CLASS_POWER,
                    WUPS_OP_PWR_EVENT, WUPS_FLAG_EVENT, Wups_Tx_Seq++,
                    &e, sizeof(e));
}

/* Inbound dispatch — invoked when the deframer has a complete frame. */
static void wups_handle_frame(uint8_t dst, uint8_t src, uint8_t cls,
                              uint8_t op, uint8_t flags, uint8_t seq,
                              const uint8_t *payload, uint16_t len)
{
    (void)payload;
    (void)len;

    /* Drop frames not addressed to us, broadcast, or internal multicast.
     * CH32X is a leaf — it does not retransmit anything. */
    if (dst != WUPS_ADDR_CH32X &&
        dst != WUPS_ADDR_BROADCAST &&
        dst != WUPS_ADDR_INTERNAL)
    {
        return;
    }

    if (cls == WUPS_CLASS_SYSTEM)
    {
        if (op == WUPS_OP_SYS_PING && (flags & WUPS_FLAG_REQ))
        {
            wups_sys_pong_v1_t pong;
            pong.version    = 1;
            pong.reserved   = 0;
            pong.fw_version = (uint16_t)((1u << 8) | 0u);
            pong.uptime_ms  = (uint32_t)Uptime_Sec * 1000u;
            wups_send_frame(src, WUPS_CLASS_SYSTEM, WUPS_OP_SYS_PING,
                            WUPS_FLAG_RESP, seq, &pong, sizeof(pong));
        }
        else if (op == WUPS_OP_SYS_STATUS_QUERY && (flags & WUPS_FLAG_REQ))
        {
            wups_send_power_status(src, WUPS_FLAG_RESP, seq);
        }
        return;
    }

    if (cls == WUPS_CLASS_POWER && (flags & WUPS_FLAG_REQ))
    {
        switch (op)
        {
        case WUPS_OP_PWR_ENABLE:
            Power_Output_Restart();
            break;
        case WUPS_OP_PWR_DISABLE:
            VBUS_disable();
            break;
        case WUPS_OP_PWR_CYCLE:
            VBUS_disable();
            Powercycle_Timer_Ms = 0;
            Powercycle_State = POWERCYCLE_OFF_WAIT;
            break;
        case WUPS_OP_PWR_RESET:
            /* No NEED_ACK protocol in v1 — best-effort drain then reset. */
            Delay_Ms(50);
            NVIC_SystemReset();
            break;
        default:
            break;
        }
        return;
    }
}

/* Receive state machine — fed one byte at a time from the DMA ring. */
typedef enum {
    WUPS_RX_SYNC1 = 0,
    WUPS_RX_SYNC2,
    WUPS_RX_DST,
    WUPS_RX_SRC,
    WUPS_RX_CLASS,
    WUPS_RX_OP,
    WUPS_RX_FLAGS,
    WUPS_RX_SEQ,
    WUPS_RX_LEN_L,
    WUPS_RX_LEN_H,
    WUPS_RX_PAYLOAD,
    WUPS_RX_CK_A,
    WUPS_RX_CK_B,
    WUPS_RX_END1,
    WUPS_RX_END2,
} wups_rx_state_t;

static struct {
    wups_rx_state_t state;
    uint8_t  dst, src, cls, op, flags, seq;
    uint16_t len;
    uint16_t pidx;
    uint8_t  payload[WUPS_MAX_PAYLOAD];
    uint8_t  rx_ck_a, rx_ck_b;
    uint8_t  exp_a, exp_b;
} Wups_Rx;

static inline void wups_rx_reset(void) { Wups_Rx.state = WUPS_RX_SYNC1; }

static inline void wups_rx_step(uint8_t b)
{
    Wups_Rx.exp_a = (uint8_t)(Wups_Rx.exp_a + b);
    Wups_Rx.exp_b = (uint8_t)(Wups_Rx.exp_b + Wups_Rx.exp_a);
}

static void wups_rx_byte(uint8_t b)
{
    switch (Wups_Rx.state)
    {
    case WUPS_RX_SYNC1:
        if (b == WUPS_SYNC1) Wups_Rx.state = WUPS_RX_SYNC2;
        break;
    case WUPS_RX_SYNC2:
        if (b == WUPS_SYNC2)
        {
            Wups_Rx.exp_a = 0;
            Wups_Rx.exp_b = 0;
            Wups_Rx.pidx  = 0;
            Wups_Rx.state = WUPS_RX_DST;
        }
        else
        {
            wups_rx_reset();
        }
        break;
    case WUPS_RX_DST:    Wups_Rx.dst = b;   wups_rx_step(b); Wups_Rx.state = WUPS_RX_SRC;   break;
    case WUPS_RX_SRC:    Wups_Rx.src = b;   wups_rx_step(b); Wups_Rx.state = WUPS_RX_CLASS; break;
    case WUPS_RX_CLASS:  Wups_Rx.cls = b;   wups_rx_step(b); Wups_Rx.state = WUPS_RX_OP;    break;
    case WUPS_RX_OP:     Wups_Rx.op = b;    wups_rx_step(b); Wups_Rx.state = WUPS_RX_FLAGS; break;
    case WUPS_RX_FLAGS:  Wups_Rx.flags = b; wups_rx_step(b); Wups_Rx.state = WUPS_RX_SEQ;   break;
    case WUPS_RX_SEQ:    Wups_Rx.seq = b;   wups_rx_step(b); Wups_Rx.state = WUPS_RX_LEN_L; break;
    case WUPS_RX_LEN_L:
        Wups_Rx.len = b;
        wups_rx_step(b);
        Wups_Rx.state = WUPS_RX_LEN_H;
        break;
    case WUPS_RX_LEN_H:
        Wups_Rx.len |= (uint16_t)((uint16_t)b << 8);
        wups_rx_step(b);
        if (Wups_Rx.len > WUPS_MAX_PAYLOAD) { wups_rx_reset(); break; }
        Wups_Rx.state = (Wups_Rx.len == 0) ? WUPS_RX_CK_A : WUPS_RX_PAYLOAD;
        break;
    case WUPS_RX_PAYLOAD:
        Wups_Rx.payload[Wups_Rx.pidx++] = b;
        wups_rx_step(b);
        if (Wups_Rx.pidx >= Wups_Rx.len) Wups_Rx.state = WUPS_RX_CK_A;
        break;
    case WUPS_RX_CK_A:
        Wups_Rx.rx_ck_a = b;
        Wups_Rx.state = WUPS_RX_CK_B;
        break;
    case WUPS_RX_CK_B:
        Wups_Rx.rx_ck_b = b;
        if (Wups_Rx.rx_ck_a == Wups_Rx.exp_a && Wups_Rx.rx_ck_b == Wups_Rx.exp_b)
        {
            Wups_Rx.state = WUPS_RX_END1;
        }
        else
        {
            wups_rx_reset();
        }
        break;
    case WUPS_RX_END1:
        Wups_Rx.state = (b == WUPS_END1) ? WUPS_RX_END2 : WUPS_RX_SYNC1;
        break;
    case WUPS_RX_END2:
        if (b == WUPS_END2)
        {
            Cmd_Frames_Dispatched++;
            wups_handle_frame(Wups_Rx.dst, Wups_Rx.src, Wups_Rx.cls,
                              Wups_Rx.op, Wups_Rx.flags, Wups_Rx.seq,
                              Wups_Rx.payload, Wups_Rx.len);
        }
        wups_rx_reset();
        break;
    default:
        wups_rx_reset();
        break;
    }
}

/* Drain the DMA RX buffer and feed bytes to the deframer. */
static void Cmd_Rx_Drain(void)
{
    /* DMA should keep RXNE drained fast enough that ORE never sets.
     * Track any occurrence — non-zero ore means the DMA controller
     * stalled (e.g. high-priority bus contention). */
    if (USART2->STATR & 0x08u)
    {
        Uart_Ore_Count++;
        (void)USART2->DATAR; /* "read SR + read DR" sequence clears ORE */
    }

    /* DMA write head = bytes_total - bytes_remaining_to_transfer.
     * CNTR decrements as DMA writes each byte. In circular mode it
     * reloads to BufferSize when reaching 0. */
    uint16_t head = DMA_RX_BUF_SIZE - (uint16_t)DMA1_Channel6->CNTR;

    while (Dma_Rx_Tail != head)
    {
        uint8_t c = Dma_Rx_Buf[Dma_Rx_Tail];
        Dma_Rx_Tail = (uint16_t)((Dma_Rx_Tail + 1) % DMA_RX_BUF_SIZE);
        Uart_Rx_Total++;
        wups_rx_byte(c);
    }
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

#ifdef DIAG_FORCE_5V_5A_NO_PD
    /* Force TPS55289 to 5.1 V / 5 A and assert VBUS_OUT_EN. No PD
     * negotiation will run — see DIAG_FORCE_5V_5A_NO_PD comment above. */
    GPIO_WriteBit(GPIOA, GPIO_Pin_7, 1);   /* VBUS_OUT_EN = 1 */
    GPIO_WriteBit(GPIOB, GPIO_Pin_0, 1);   /* PDS_EN = 1 (TPS enable) */
    tps55289_set_cdc_compensation(CDC_COMP_0V7);
    tps55289_set_current_limit(5.0);
    tps55289_set_voltage(5.0);
    tps55289_enable_output(1);
#endif

    /* Announce ourselves on the bus. RP2040 (the hub) caches hellos to
     * track which nodes are alive. Sent once; not retried. */
    wups_send_hello_bcast();

    while(1)
    {
        /* Get the calculated timing interval value */
        TIM_ITConfig( TIM1, TIM_IT_Update , DISABLE );
        Tmr_Ms_Dlt = Tim_Ms_Cnt - Tmr_Ms_Cnt_Last;
        Tmr_Ms_Cnt_Last = Tim_Ms_Cnt;
        //Tmr_Ms_Dlt = 1;
        TIM_ITConfig( TIM1, TIM_IT_Update , ENABLE );

#ifndef DIAG_FORCE_5V_5A_NO_PD
		/* role manager tick: uses Tmr_Ms_Dlt */
        PD_Role_Manager_Tick();
#endif

        /* Drain UART RX ring and dispatch any complete command frames. */
        Cmd_Rx_Drain();

        /* Power-cycle state machine: re-enable VBUS after off window. */
        if (Powercycle_State == POWERCYCLE_OFF_WAIT)
        {
            Powercycle_Timer_Ms += Tmr_Ms_Dlt;
            if (Powercycle_Timer_Ms >= POWERCYCLE_OFF_MS)
            {
                Power_Output_Restart();
                Powercycle_State = POWERCYCLE_IDLE;
            }
        }

#ifndef DIAG_FORCE_5V_5A_NO_PD
        PD_Ctl.Det_Timer += Tmr_Ms_Dlt;
        if( PD_Ctl.Det_Timer > 4 )
        {
            PD_Ctl.Det_Timer = 0;
            PD_Det_Proc( );
        }
        PD_Main_Proc( );
#endif

        /* Periodic temperature reading from LM75B every 5s */
        Temp_Timer_Ms += Tmr_Ms_Dlt;
        if (Temp_Timer_Ms >= TEMP_INTERVAL_MS)
        {
            Temp_Timer_Ms = 0;
            Board_Temp_c10 = (int16_t)(lm75b_read_temp_c() * 10.0f);
        }

        /* Read TPS55289 voltage/current every 2s, plus STATUS register
         * which holds latched SCP/OCP/OVP flags until read. */
        Tps_Timer_Ms += Tmr_Ms_Dlt;
        if (Tps_Timer_Ms >= TPS_INTERVAL_MS)
        {
            Tps_Timer_Ms = 0;
            Tps_Vread_v10 = (int16_t)(tps55289_read_voltage() * 10.0f);
            Tps_Iread_a10 = (int16_t)(tps55289_read_current_limit() * 10.0f);
            Tps_Status_Latched |= tps55289_read_status();

#ifdef DIAG_FORCE_5V_5A_NO_PD
            /* Re-apply the full config every 2 s as a watchdog. Catches:
             *  - INTFB / REF leftover from a previous firmware build (TPS
             *    is not power-cycled when CH32X is reflashed, so its
             *    registers persist across firmware loads).
             *  - Autonomous TPS resets that might wipe REF back to default
             *    while leaving INTFB on a stale value.
             * Reading the registers at the top of this branch already
             * surfaced the current state into telemetry; the reapply
             * happens after the read so the screen briefly shows the
             * stale value before snapping to the intended one. */
            tps55289_set_cdc_compensation(CDC_COMP_0V7);
            tps55289_set_current_limit(5.0);
            tps55289_set_voltage(5.0);
            tps55289_enable_output(1);
#endif
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

        /* Diagnostic log: every 5 s, dump the raw bytes of the MP2762A
         * registers we care about to system.log. RP2040 forwards it to
         * the debug stream so a remote operator can see exactly what the
         * chip reports without unplugging the bench setup. */
        Diag_Log_Timer_Ms += Tmr_Ms_Dlt;
        if (Diag_Log_Timer_Ms >= DIAG_LOG_INTERVAL_MS)
        {
            Diag_Log_Timer_Ms = 0;
            uint8_t r00 = mp2762a_read_reg(MP2762A_REG_INPUT_ILIM);
            uint8_t r0F = mp2762a_read_reg(MP2762A_REG_INPUT_ILIM2);
            uint8_t r02 = mp2762a_read_reg(MP2762A_REG_CHG_CURR);
            uint8_t r08 = mp2762a_read_reg(MP2762A_REG_CONFIG0);
            uint8_t r13 = mp2762a_read_reg(MP2762A_REG_STATUS);
            uint8_t r14 = mp2762a_read_reg(MP2762A_REG_FAULT);
            uint8_t ichg_l = mp2762a_read_reg(MP2762A_REG_ADC_ICHG_L);
            uint8_t ichg_h = mp2762a_read_reg(MP2762A_REG_ADC_ICHG_H);
            uint8_t iin_l  = mp2762a_read_reg(MP2762A_REG_ADC_IIN_L);
            uint8_t iin_h  = mp2762a_read_reg(MP2762A_REG_ADC_IIN_H);
            uint8_t tj_l   = mp2762a_read_reg(MP2762A_REG_ADC_TJUNC_L);
            uint8_t tj_h   = mp2762a_read_reg(MP2762A_REG_ADC_TJUNC_H);
            char buf[160];
            /* Hand-rolled formatter — newlib-nano printf is ~7 KB and we
             * don't otherwise pay for it. Order matches the register map
             * for visual scanning. */
            const char hex[] = "0123456789ABCDEF";
            int p = 0;
            #define EMIT_STR(s_) do { const char *__s = (s_); while (*__s) buf[p++] = *__s++; } while(0)
            #define EMIT_HEX(b_) do { uint8_t __b = (b_); buf[p++] = hex[(__b >> 4) & 0xF]; buf[p++] = hex[__b & 0xF]; } while(0)
            EMIT_STR("MP2762A r00="); EMIT_HEX(r00);
            EMIT_STR(" r0F=");        EMIT_HEX(r0F);
            EMIT_STR(" r02=");        EMIT_HEX(r02);
            EMIT_STR(" r08=");        EMIT_HEX(r08);
            EMIT_STR(" r13=");        EMIT_HEX(r13);
            EMIT_STR(" r14=");        EMIT_HEX(r14);
            EMIT_STR(" ICHG=");       EMIT_HEX(ichg_h); EMIT_HEX(ichg_l);
            EMIT_STR(" IIN=");        EMIT_HEX(iin_h);  EMIT_HEX(iin_l);
            EMIT_STR(" TJraw=");      EMIT_HEX(tj_h);   EMIT_HEX(tj_l);
            /* Decimal interpretation of the two on-board temperature
             * sources, in tenths of a degree. Keeps the diagnostic feed
             * self-contained so we don't have to cross-reference with the
             * power.status text line every time. */
            EMIT_STR(" Tboard=");
            {
                int16_t v = Board_Temp_c10;
                if (v < 0) { buf[p++] = '-'; v = (int16_t)(-v); }
                int whole = v / 10;
                int frac  = v % 10;
                if (whole >= 100) buf[p++] = (char)('0' + whole / 100);
                if (whole >= 10)  buf[p++] = (char)('0' + (whole / 10) % 10);
                buf[p++] = (char)('0' + whole % 10);
                buf[p++] = '.';
                buf[p++] = (char)('0' + frac);
                buf[p++] = 'C';
            }
            EMIT_STR(" TJ=");
            {
                int16_t v = Chg_Data.tjunc_c10;
                if (v < 0) { buf[p++] = '-'; v = (int16_t)(-v); }
                int whole = v / 10;
                int frac  = v % 10;
                if (whole >= 100) buf[p++] = (char)('0' + whole / 100);
                if (whole >= 10)  buf[p++] = (char)('0' + (whole / 10) % 10);
                buf[p++] = (char)('0' + whole % 10);
                buf[p++] = '.';
                buf[p++] = (char)('0' + frac);
                buf[p++] = 'C';
            }
            #undef EMIT_STR
            #undef EMIT_HEX
            buf[p] = 0;
            wups_send_log(2 /* info */, buf);
        }

        /* Periodic power.status push to RP2040 (1 Hz, EVENT flag). */
        Json_Timer_Ms += Tmr_Ms_Dlt;
        if (Json_Timer_Ms >= JSON_INTERVAL_MS)
        {
            Json_Timer_Ms = 0;

            /* DC input voltage from cached PA1 ADC: Vin = ADC * 3300 * (27.4+5.1) / 5.1 / 4096.
             * Computed but not yet surfaced in power.status v1 — keep cached for future use. */
            DC_Inp_Voltage_mV = (UINT16)((UINT32)DC_Inp_ADC_Val * 21029 / 4096);

            wups_send_power_status(WUPS_ADDR_RP2040, WUPS_FLAG_EVENT, Wups_Tx_Seq++);
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

