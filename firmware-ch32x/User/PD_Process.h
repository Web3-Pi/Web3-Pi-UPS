/********************************** (C) COPYRIGHT *******************************
* File Name          : PD_Process.h
* Author             : WCH
* Version            : V1.0.1
* Date               : 2025/03/06
* Description        : This file contains all the functions prototypes for the
*                      PD library.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

#ifndef USER_PD_PROCESS_H_
#define USER_PD_PROCESS_H_

#ifdef __cplusplus
 extern "C" {
#endif
#define LowpowerON 1
#define LowpowerOff 0
/* Web3 Pi UPS v3: low-power is OFF by design. This is an always-on UPS
 * output controller — it must keep running the charger watchdog, telemetry
 * and the source PD FSM at all times. The upstream WCH sample left this at
 * LowpowerON, which made STA_DISCONNECT call PWR_EnterSTANDBYMode() on every
 * sink unplug: the MCU went to STANDBY *before* PD_PHY_Reset(), so the
 * TPS55289 was never told to drop the negotiated rail (9V/15V stayed live on
 * an open USB-C port) and the next CC wake came back as a full reset. Forcing
 * LowpowerOff removes that standby path entirely (matches CLAUDE.md, which
 * already documents low-power as disabled for this design). */
#define Lowpower LowpowerOff

#define GPIOWake_up 1
#define USBPDWake_up 0
#define Wake_up_mode GPIOWake_up


/******************************************************************************/
/* PD event trace ring (see PD_Process.c). Byte layout: low nibble = event
 * code, high nibble = argument. Drained by main.c into system.log frames. */
#define PD_EVT_CONNECT       0x01                    /* CC attach -> STA_SINK_CONNECT */
#define PD_EVT_SRC_CAP_TX    0x02                    /* Source_Capabilities sent OK */
#define PD_EVT_REQ_RX        0x03                    /* arg = requested PDO index */
#define PD_EVT_ACCEPT_TX     0x04                    /* contract Accept sent OK */
#define PD_EVT_PS_RDY_TX     0x05                    /* PS_RDY sent OK */
#define PD_EVT_VDM_RPI_TX    0x06                    /* RPi 27W identity VDM sent */
#define PD_EVT_DRSWAP_RX     0x07                    /* arg: 0=processing 1=dup-skip 2=busy-Wait */
#define PD_EVT_DRSWAP_ACC    0x08                    /* Accept OK; arg = new PD_Role (0=UFP) */
#define PD_EVT_DRSWAP_FAIL   0x09                    /* Accept TX fail -> HRST */
#define PD_EVT_SOFTRST_RX    0x0A                    /* Soft_Reset received */
#define PD_EVT_SOFTRST_TX    0x0B                    /* Soft_Reset sent by us */
#define PD_EVT_HRST_TX       0x0C                    /* Hard Reset sent by us */
#define PD_EVT_DISCONNECT    0x0D                    /* STA_DISCONNECT (detach) */
#define PD_EVT_PROTO_REPLY   0x0E                    /* arg: 1=VDM-NAK 2=Not_Supported 3=GetSrcCap-answered 4=VCONN-Reject; ext byte = raw msg type (|0x80 when data msg) */
#define PD_EVT_ENTER_USB     0x0F                    /* arg: 1=Accepted 0=Rejected; ext byte = requested USB mode */

/* Entry layout: [3:0] code, [7:4] small arg, [15:8] extended arg byte. */
extern volatile UINT16 PD_Evt_Buf[ 16 ];
extern volatile UINT8 PD_Evt_W;

/******************************************************************************/
/* Variable extents */
extern __IO UINT8  Tmr_Ms_Cnt_Last;
extern __IO UINT8  Tmr_Ms_Dlt;
extern volatile UINT8  Tim_Ms_Cnt;

extern __IO UINT8  PDO_Len;
extern PD_CONTROL PD_Ctl;

extern UINT8 send_data[ ];
extern UINT8 PD_Ack_Buf[ ];

extern __attribute__ ((aligned(4))) UINT8 PD_Rx_Buf[ 34 ];
extern __attribute__ ((aligned(4))) UINT8 PD_Tx_Buf[ 34 ];


/***********************************************************************************************************************/
/* Function extensibility */
extern void PD_Rx_Mode( void );
extern void PD_SRC_Init( void );
extern void PD_SINK_Init( void );
extern void PD_PHY_Reset( void );
extern void PD_DataRole_Reset( void );
extern void PD_Init( void );
extern UINT8 PD_Detect( void );
extern void PD_Det_Proc( void );
extern void PD_Load_Header( UINT8 ex, UINT8 msg_type );
extern UINT8 PD_Send_Handle( UINT8 *pbuf, UINT8 len );
extern void PD_Phy_SendPack( UINT8 mode, UINT8 *pbuf, UINT8 len, UINT8 sop );
extern void PD_Main_Proc( void );
extern void PD_Request_Analyse( UINT8 pdo_idx, UINT8 *srccap, UINT16 *current );

#ifdef __cplusplus
}
#endif

#endif /* USER_PD_PROCESS_H_ */
