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
#include "husb238.h"
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
volatile u16    Vbat_ADC_Val = 0;
static UINT16   Vbat_Voltage_mV = 0;
static UINT16   Vbus_Out_Voltage_mV = 0;   /* PA0 ADC, same 27.4k/5.1k divider as PA1 */
static UINT16   Json_Timer_Ms = 0;
static UINT16   Temp_Timer_Ms = 0;
static UINT16   Chg_Timer_Ms = 0;
static UINT16   Tps_Timer_Ms = 0;
static UINT16   Bat_Timer_Ms = 0;
static UINT16   Diag_Log_Timer_Ms = 0;
#define DIAG_LOG_INTERVAL_MS 5000
static int16_t  Board_Temp_c10 = 0;
static mp2762a_data_t Chg_Data = {0};
/* HUSB238 INPUT-side PD sink contract (rev.3). CH32X is a passive reader;
 * see User/husb238.h. Refreshed in the 2 s charger-poll block, surfaced in
 * the 5 s diagnostic system.log. NOT yet in the power.status wire struct —
 * v1 is full (20 B) and the strict RPi-host parser requires exact len +
 * version==1, so structured exposure is a coordinated protocol-v2 follow-up. */
static husb238_data_t Husb_Data = {0};
/* Auto input-PD negotiation (rev.3). The HUSB238 VSET/ISET straps pick the
 * highest source PDO meeting a fixed current floor, which on most chargers
 * yields a weak low-V contract (RPi 27W -> 5V/3.25A). Once per attach the
 * CH32X overrides via I2C (priority over the straps) and applies the
 * selection policy in husb238.h (satisfice >=45W at the most efficient
 * voltage; 9-20 V; <=5 A). */
static husb238_src_caps_t Husb_Caps = {0};
static uint8_t  Husb_Pd_Optimized = 0;   /* one-shot latch, re-armed on detach */
static uint8_t  Husb_Pick_Sel = 0;       /* HUSB238_SEL_* chosen for diag log */
static uint8_t  Diag_Toggle = 0;         /* alternates MP2762A / HUSBcaps diag lines */
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

/* Negotiated OUTPUT PD contract (CH32X is the source to the Pi). Derived
 * from the accepted PDO index PD_Ctl.ReqPDO_Idx (1..4); mirrors the advertised
 * SrcCap_5V5A_Tab / the STA_TX_ACCEPT switch in PD_Process.c. 0 = rail off. */
static void pd_out_contract(uint16_t *mv, uint16_t *ma)
{
    switch (PD_Ctl.ReqPDO_Idx) {
    case 1: *mv = 5000;  *ma = 5000; break;
    case 2: *mv = 9000;  *ma = 3000; break;
    case 3: *mv = 12000; *ma = 2250; break;
    case 4: *mv = 15000; *ma = 1800; break;
    default: *mv = 0; *ma = 0; break;  /* no contract / rail off */
    }
}

/* Compose power.status v2 from current globals and send to `dst`.
 * Used both for the 1 Hz periodic EVENT (dst=RP2040) and as RESP to
 * system.status_query (dst=requester). See wups_power_status_v2_t. */
static void wups_send_power_status(uint8_t dst, uint8_t flags, uint8_t seq)
{
    wups_power_status_v2_t s;
    s.version      = 2;

    /* Packed booleans. PA6/PA7 are read back from the output data register
     * (idiom shared with mp2762a_powered()). */
    s.flags = 0;
    if (GPIO_ReadOutputDataBit(GPIOA, GPIO_Pin_6)) s.flags |= WUPS_PWR2_FLAG_DC_IN_EN;
    if (GPIO_ReadOutputDataBit(GPIOA, GPIO_Pin_7)) s.flags |= WUPS_PWR2_FLAG_VBUS_OUT_EN;
    if (mp2762a_is_battery_present())              s.flags |= WUPS_PWR2_FLAG_BATT_PRESENT;
    if (Chg_Data.power_good)                       s.flags |= WUPS_PWR2_FLAG_POWER_GOOD;
    if (Husb_Data.attached)                        s.flags |= WUPS_PWR2_FLAG_USB_C_ATTACH;

    s.charge_state = (uint8_t)Chg_Data.chg_state;
    s.reserved     = 0;

    /* --- INPUT --- */
    /* vbus_in: our own PA1 ADC (post ideal-diode OR of USB-C + barrel),
     * reliable in every state (mains on/off, MP2762A dead). */
    s.vbus_in_mV = DC_Inp_Voltage_mV;
    /* HUSB238 negotiated input contract; 0 already means "no USB-C PD". */
    s.pd_in_mV   = Husb_Data.voltage_mV;
    s.pd_in_mA   = Husb_Data.current_mA;

    /* --- OUTPUT --- */
    s.vbus_out_mV   = Vbus_Out_Voltage_mV;                              /* PA0 ADC */
    /* TPS55289 getters are 0.1 V / 0.1 A units → *100 = mV / mA. */
    s.vout_set_mV   = (uint16_t)((int32_t)tps55289_get_voltage_set_v10() * 100);
    s.vout_read_mV  = (uint16_t)((int32_t)Tps_Vread_v10 * 100);
    s.iout_limit_mA = (uint16_t)((int32_t)Tps_Iread_a10 * 100);        /* limit, not load */
    pd_out_contract(&s.pd_out_mV, &s.pd_out_mA);

    /* --- BATTERY --- */
    /* vbat: our own PA5 ADC, authoritative (MP2762A reads 0 on battery). */
    s.vbat_mV = Vbat_Voltage_mV;
    s.ichg_mA = (int16_t)Chg_Data.ichg_ma;   /* charge current; 0 on discharge */

    /* --- SYSTEM --- */
    s.vsys_mV     = (uint16_t)Chg_Data.vsys_mv;   /* real fields now (v1 aliased these) */
    s.iin_mA      = (uint16_t)Chg_Data.iin_ma;
    s.temp_lm_dC  = Board_Temp_c10;
    s.temp_mp_dC  = Chg_Data.tjunc_c10;           /* MP2762A_TJ_NA (-32768) when unpowered */

    /* Pack TPS55289 STATUS bits into the high byte of `faults`; MP2762A fault
     * byte stays in the low byte. bit8=SCP bit9=OCP bit10=OVP. */
    {
        uint16_t tps_flags = 0;
        if (Tps_Status_Latched & STATUS_SCP_BIT) tps_flags |= (1u << 8);
        if (Tps_Status_Latched & STATUS_OCP_BIT) tps_flags |= (1u << 9);
        if (Tps_Status_Latched & STATUS_OVP_BIT) tps_flags |= (1u << 10);
        s.faults = (uint16_t)(Chg_Data.fault | tps_flags);
        /* Clear the latch only after packing into the frame about to ship. */
        Tps_Status_Latched = 0;
    }

    s.uptime_s = Uptime_Sec;

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

/* power.event broadcast helper. Fired by wups_power_event_tick() below;
 * RP2040 wraps the frame into net.publish("event") for the panel's
 * event log (and the RPi sees the broadcast directly). */
static void wups_send_power_event(uint8_t event)
{
    wups_power_event_v1_t e;
    e.version = 1;
    e.event   = event;
    wups_send_frame(WUPS_ADDR_BROADCAST, WUPS_CLASS_POWER,
                    WUPS_OP_PWR_EVENT, WUPS_FLAG_EVENT, Wups_Tx_Seq++,
                    &e, sizeof(e));
}

/* --- power.event edge detector ------------------------------------------
 *
 * Samples the same globals the 1 Hz power.status frame is composed from
 * and emits alert-class events on TRANSITIONS only. Must run BEFORE
 * wups_send_power_status() in the 1 Hz block: the status send clears
 * Tps_Status_Latched after packing, and the charger poll clears
 * Chg_Data.fault on recovery — this detector wants to see those bits too.
 *
 * Mains presence uses the same source the panel derives "Mains" from
 * (DC_Inp_Voltage_mV, PA1 ADC) with a ±500 mV hysteresis band around the
 * panel's 5 V threshold so a brownout hovering at the threshold doesn't
 * flap events. First tick primes silently — booting on battery is a
 * state, not an event.
 */
#define PWR_EVT_MAINS_ON_MV     5500u  /* rising  edge: declare "present"   */
#define PWR_EVT_MAINS_OFF_MV    4500u  /* falling edge: declare "lost"      */
#define PWR_EVT_VBAT_LOW_MV     6900u  /* 2S pack ~3.45 V/cell ≈ low charge */
#define PWR_EVT_VBAT_REARM_MV   7300u  /* hysteresis re-arm for CHARGE_LOW  */
#define PWR_EVT_FAULT_HOLD_S    30u    /* min spacing between FAULT events  */

static void wups_power_event_tick(void)
{
    static uint8_t  Evt_Primed       = 0;
    static uint8_t  Evt_Mains        = 0;   /* 1 = mains present            */
    static uint16_t Evt_Prev_Faults  = 0;
    static uint8_t  Evt_Prev_Cs      = 0;
    static uint8_t  Evt_Batt_Low     = 0;   /* CHARGE_LOW armed/fired latch */
    static UINT32   Evt_Last_Fault_S = 0;

    /* Mains with hysteresis. */
    uint8_t mains = Evt_Mains ? (DC_Inp_Voltage_mV > PWR_EVT_MAINS_OFF_MV)
                              : (DC_Inp_Voltage_mV > PWR_EVT_MAINS_ON_MV);

    /* Fault bitmap composed exactly like power.status (MP2762A low byte +
     * TPS55289 latched bits in the high byte). Latch is NOT cleared here —
     * the status send right after us does that. */
    uint16_t tps_flags = 0;
    if (Tps_Status_Latched & STATUS_SCP_BIT) tps_flags |= (1u << 8);
    if (Tps_Status_Latched & STATUS_OCP_BIT) tps_flags |= (1u << 9);
    if (Tps_Status_Latched & STATUS_OVP_BIT) tps_flags |= (1u << 10);
    uint16_t faults = (uint16_t)(Chg_Data.fault | tps_flags);

    uint8_t cs = (uint8_t)Chg_Data.chg_state;

    if (!Evt_Primed)
    {
        Evt_Primed      = 1;
        Evt_Mains       = mains;
        Evt_Prev_Faults = faults;
        Evt_Prev_Cs     = cs;
        return;
    }

    if (mains != Evt_Mains)
    {
        Evt_Mains = mains;
        wups_send_power_event(mains ? WUPS_PWR_EVT_MAINS_RESTORED
                                    : WUPS_PWR_EVT_MAINS_LOST);
        if (mains) Evt_Batt_Low = 0;   /* charger will refill — re-arm low */
    }

    /* FAULT: any NEW fault bit. Chg_Data.fault pulses (recovery clears it)
     * and the TPS latch resets every second, so rate-limit to one event
     * per PWR_EVT_FAULT_HOLD_S even if the same fault keeps re-tripping. */
    if ((uint16_t)(faults & (uint16_t)~Evt_Prev_Faults) != 0 &&
        (UINT32)(Uptime_Sec - Evt_Last_Fault_S) >= PWR_EVT_FAULT_HOLD_S)
    {
        Evt_Last_Fault_S = Uptime_Sec;
        wups_send_power_event(WUPS_PWR_EVT_FAULT);
    }
    Evt_Prev_Faults = faults;

    /* CHARGE_FULL: charger entered termination (cs=3) — only meaningful
     * with mains present (cs is garbage when the MP2762A is unpowered). */
    if (mains && cs == 3 && Evt_Prev_Cs != 3)
    {
        wups_send_power_event(WUPS_PWR_EVT_CHARGE_FULL);
    }
    Evt_Prev_Cs = cs;

    /* CHARGE_LOW: discharging below the low-water mark. One-shot, re-armed
     * by recovery above PWR_EVT_VBAT_REARM_MV or by mains coming back. */
    if (!mains && !Evt_Batt_Low && Vbat_Voltage_mV < PWR_EVT_VBAT_LOW_MV &&
        Vbat_Voltage_mV > 1000u /* sanity: ignore no-battery readings */)
    {
        Evt_Batt_Low = 1;
        wups_send_power_event(WUPS_PWR_EVT_CHARGE_LOW);
    }
    else if (Evt_Batt_Low && Vbat_Voltage_mV > PWR_EVT_VBAT_REARM_MV)
    {
        Evt_Batt_Low = 0;
    }
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
    husb238_init();   /* INPUT PD sink (rev.3) — passive I2C reader */

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

        /* Dual-role manager (PB11 host-detect + PB3 CC mux + SINK retry FSM)
         * is gone on v3 — HUSB238 (U150) owns USB-C INPUT PD negotiation
         * entirely, the SRC_TPx pins on the CH32X are NC test points, and
         * PD_Role_Manager_Tick() reading floating PB11 was triggering false
         * SINK switches that corrupted TPS REF on cold-boot with PD-on-input
         * + Pi5-already-attached. Source PD FSM (PD_Det_Proc + PD_Main_Proc)
         * is still called below — it handles Pi5 on the OUTPUT correctly. */

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
            /* DIAG mode: skip PD nego entirely, hardcode 5V/5A. */
            tps55289_set_cdc_compensation(CDC_COMP_0V7);
            tps55289_set_current_limit(5.0);
            tps55289_set_voltage(5.0);
            tps55289_enable_output(1);
#else
            /* Watchdog: every 2 s re-apply whatever VBUS_set_5V or the PD
             * source FSM last asked for. Cached setpoints (g_*_set_*10)
             * are populated by tps55289_set_voltage/set_current_limit and
             * cleared on VBUS_disable, so v_set==0 means "rail intentionally
             * off — do nothing". Catches:
             *  - Cold-boot with USB-C-on-input AND Pi5-already-attached:
             *    TPS comes up post-UVLO with REF reset to default and
             *    INTFB retained from prior firmware run → wrong VOUT.
             *    First VBUS_set_5V() in PD_Det_Proc happens during TPS
             *    warmup and the write can land in an I2C window where
             *    the chip NACKs or misses the byte; this watchdog fixes
             *    the next tick.
             *  - Autonomous TPS resets after SCP/OCP hiccup that wipe REF
             *    while leaving INTFB on a stale value.
             *  - Mid-negotiation I2C glitch from PD BMC noise.
             * Idempotent: if state is already correct, the writes are
             * no-ops at the chip level. ~3–4 I2C transactions @ ~100 kHz
             * bit-bang ≈ 1 ms total, well under the 2 s window. */
            {
                int16_t v_set = tps55289_get_voltage_set_v10();
                int16_t i_set = tps55289_get_current_set_a10();
                if (v_set > 0 && i_set > 0)
                {
                    tps55289_set_cdc_compensation(CDC_COMP_0V7);
                    tps55289_set_current_limit((float)i_set / 10.0f);
                    tps55289_set_voltage((float)v_set / 10.0f);
                    tps55289_enable_output(1);
                }
            }
#endif
        }

        /* Read MP2762A charger status every 2s + kick watchdog */
        Chg_Timer_Ms += Tmr_Ms_Dlt;
        if (Chg_Timer_Ms >= CHG_INTERVAL_MS)
        {
            Chg_Timer_Ms = 0;
            mp2762a_read_all(&Chg_Data);
            mp2762a_kick_watchdog();
            /* HUSB238 negotiates a default INPUT PD contract autonomously
             * (from its VSET/ISET straps); read back what's in effect.
             * Shares the slow bit-bang bus with the charger poll — 2 s is
             * plenty since the contract only changes on attach/renegotiation. */
            husb238_read_all(&Husb_Data);

            /* (Re)negotiate off the HUSB238 PD-attach state, which is
             * USB-C-specific (the HUSB238's VIN is the USB-C VBUS only).
             * Do NOT use the PA1 input-voltage ADC for this: PA1 sits after
             * the ideal-diode OR of USB-C and the barrel jack, so it can't
             * tell a USB-C unplug from barrel power still being present.
             * `attached` is hardened in husb238_read_all to reject the
             * 0x00/0xFF an unpowered chip leaves on the shared bus, so a
             * USB-C unplug reliably reads detached -> re-arm -> the next plug
             * (or a swapped charger) renegotiates. */
            if (Husb_Data.attached) {
                if (!Husb_Pd_Optimized) {
                    husb238_read_src_caps(&Husb_Caps);
                    if (Husb_Caps.detected_mask == 0) {
                        /* Caps not latched yet — nudge the source and retry
                         * next poll (leave the one-shot un-armed). */
                        husb238_write_reg(HUSB238_REG_GO_COMMAND, HUSB238_CMD_GET_SRC_CAP);
                    } else {
                        Husb_Pick_Sel = husb238_pick_best_pdo(&Husb_Caps);
                        if (Husb_Pick_Sel != HUSB238_SEL_NONE) {
                            /* Re-request unless the LIVE contract already
                             * matches the pick in BOTH voltage AND current.
                             * The VSET/ISET straps can land on our target
                             * voltage but a LOWER current (this board: strap
                             * 15V/1.75A while the source offers 15V/3A). The
                             * old voltage-only test treated that as "already
                             * optimal" and never issued REQUEST_PDO, so the
                             * contract stayed pinned at the strap's 1.75A.
                             * Only an explicit I2C request raises the
                             * negotiated current to the PDO's advertised value
                             * (ISET no longer applies once I2C overrides). */
                            uint16_t pick_mV = husb238_sel_to_mV(Husb_Pick_Sel);
                            uint16_t pick_mA = 0;
                            for (int i = 0; i < HUSB238_PDO_COUNT; i++) {
                                if (HUSB238_PDO_MV[i] == pick_mV) {
                                    pick_mA = Husb_Caps.current_mA[i];
                                    if (pick_mA > HUSB238_POLICY_I_CAP_MA)
                                        pick_mA = HUSB238_POLICY_I_CAP_MA;
                                    break;
                                }
                            }
                            if (pick_mV != Husb_Data.voltage_mV ||
                                Husb_Data.current_mA + 50 < pick_mA) {
                                husb238_select_pdo(Husb_Pick_Sel);  /* renegotiate */
                            }
                            /* Latch only on a real pick — if caps came back
                             * garbled (mask set but all 0 mA), leave un-armed
                             * so the next poll re-reads and self-heals. */
                            Husb_Pd_Optimized = 1;
                        }
                    }
                }
            } else {
                Husb_Pd_Optimized = 0;
                Husb_Pick_Sel = HUSB238_SEL_NONE;
                Husb_Caps.detected_mask = 0;
            }
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
            /* Alternate the heavy lines so each 5 s burst is <= 2 system.log
             * frames — RP2040's debug forwarder drops a 3rd back-to-back
             * frame. The HUSB238 contract goes out every cycle; the MP2762A
             * dump and HUSBcaps take turns (each every 10 s). */
            Diag_Toggle ^= 1;

            if (!Diag_Toggle) {
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
            } /* end MP2762A dump (alternate cycle) */

            /* HUSB238 INPUT PD sink contract (rev.3) — emitted EVERY cycle as
             * the primary input-PD signal (until power.status gains dedicated
             * fields, a protocol-v2 follow-up). Hand-rolled formatter (no
             * newlib printf): decimal mV/mA + raw nibble codes for debugging. */
            {
                char hbuf[72];
                const char hhex[] = "0123456789ABCDEF";
                int hp = 0;
                #define HEMIT_STR(s_) do { const char *__s = (s_); while (*__s) hbuf[hp++] = *__s++; } while(0)
                #define HEMIT_U16(v_) do { \
                    uint16_t __v = (v_); char __t[5]; int __n = 0; \
                    if (__v == 0) { hbuf[hp++] = '0'; } \
                    else { while (__v) { __t[__n++] = (char)('0' + __v % 10); __v /= 10; } \
                           while (__n) hbuf[hp++] = __t[--__n]; } \
                } while(0)
                HEMIT_STR("HUSB att="); hbuf[hp++] = Husb_Data.attached ? '1' : '0';
                HEMIT_STR(" Vin=");  HEMIT_U16(Husb_Data.voltage_mV); HEMIT_STR("mV");
                HEMIT_STR(" Iin=");  HEMIT_U16(Husb_Data.current_mA); HEMIT_STR("mA");
                HEMIT_STR(" v=");    hbuf[hp++] = hhex[Husb_Data.v_code & 0xF];
                HEMIT_STR(" i=");    hbuf[hp++] = hhex[Husb_Data.i_code & 0xF];
                HEMIT_STR(" resp="); hbuf[hp++] = hhex[Husb_Data.last_response & 0xF];
                #undef HEMIT_STR
                #undef HEMIT_U16
                hbuf[hp] = 0;
                wups_send_log(2 /* info */, hbuf);
            }

            /* HUSB238 source capabilities (advertised mA per voltage, '-' =
             * not offered) + the PDO the auto-negotiator picked — alternate
             * cycles (every 10 s), so the selection policy is visible on HW. */
            if (Diag_Toggle) {
                static const char *const HLBL[HUSB238_PDO_COUNT] =
                    { "5", "9", "12", "15", "18", "20" };
                char cbuf[96];
                int cp = 0;
                #define CEMIT_STR(s_) do { const char *__s = (s_); while (*__s) cbuf[cp++] = *__s++; } while(0)
                #define CEMIT_U16(v_) do { \
                    uint16_t __v = (v_); char __t[5]; int __n = 0; \
                    if (__v == 0) { cbuf[cp++] = '0'; } \
                    else { while (__v) { __t[__n++] = (char)('0' + __v % 10); __v /= 10; } \
                           while (__n) cbuf[cp++] = __t[--__n]; } \
                } while(0)
                CEMIT_STR("HUSBcaps");
                for (int i = 0; i < HUSB238_PDO_COUNT; i++) {
                    cbuf[cp++] = ' ';
                    CEMIT_STR(HLBL[i]);
                    cbuf[cp++] = '=';
                    if (Husb_Caps.detected_mask & (1u << i)) CEMIT_U16(Husb_Caps.current_mA[i]);
                    else cbuf[cp++] = '-';
                }
                CEMIT_STR(" pick="); CEMIT_U16(husb238_sel_to_mV(Husb_Pick_Sel));
                #undef CEMIT_STR
                #undef CEMIT_U16
                cbuf[cp] = 0;
                wups_send_log(2 /* info */, cbuf);
            }
        }

        /* Periodic power.status push to RP2040 (1 Hz, EVENT flag). */
        Json_Timer_Ms += Tmr_Ms_Dlt;
        if (Json_Timer_Ms >= JSON_INTERVAL_MS)
        {
            Json_Timer_Ms = 0;

            /* DC input voltage from cached PA1 ADC: Vin = ADC * 3300 * (27.4+5.1) / 5.1 / 4096. */
            DC_Inp_Voltage_mV = (UINT16)((UINT32)DC_Inp_ADC_Val * 21029 / 4096);

            /* Battery voltage from PA5 ADC: VBAT = ADC * 3300 * (100+47) / 47 / 4096
             * ≈ ADC * 10322 / 4096. Sampled here (1 Hz) because the WUPS frame
             * consumer is also 1 Hz; no benefit from higher rate. This is the
             * authoritative VBAT source — MP2762A's VBAT register reads 0 when
             * the chip is unpowered (mains absent), which is the normal "on
             * battery" state, so we cannot rely on it. */
            Vbat_ADC_Val = Get_ADC_Val(ADC_Channel_5);
            Vbat_Voltage_mV = (UINT16)((UINT32)Vbat_ADC_Val * 10322u / 4096u);

            /* Output rail from PA0 ADC: same 27.4k/5.1k divider as PA1, so the
             * same 21029/4096 scale. Independent of the TPS55289 readback. */
            Vbus_Out_Voltage_mV = (UINT16)((UINT32)Get_ADC_Val(ADC_Channel_0) * 21029 / 4096);

            /* Edge detector first — it reads the same fault latch the
             * status send below clears after packing. */
            wups_power_event_tick();
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

    //PA0: VBUS_OUT_ADC — output rail to the Pi, 27.4k/5.1k divider (same as PA1)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    //PA1: DC_INP_ADC_SRC
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    //PA5: PD_SRC_VBAT — battery voltage ADC via R430/R431 divider
    //(R433=100R since 2026-05-11; before that R433=DNP so PA5 floated).
    //Scale: PA5 = VBAT * 47 / (100+47) ≈ 0.3197 * VBAT.
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
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

