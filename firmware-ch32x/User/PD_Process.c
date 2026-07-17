/********************************** (C) COPYRIGHT *******************************
* File Name          : PD_process.c
* Author             : WCH (upstream); Web3 Pi UPS source-only customizations
* Version            : V1.0.2 + v3 customizations
* Date               : 2025/10/27 upstream / 2026-05-27 customized
* Description        : USB-PD SOURCE side for Web3 Pi UPS v3 (Pi5 on output).
*                      Based on WCH ch32x035 USBPD_SRC sample V1.0.2.
*                      v3 customizations on top of upstream:
*                        - 4-PDO source caps (5V/5A, 9V/3A, 12V/2.25A, 15V/1.8A)
*                          instead of upstream's single 5V/1.5A PDO
*                        - TPS55289 reconfiguration in STA_TX_ACCEPT to set
*                          rail voltage + current limit per accepted PDO
*                        - VDM (Vendor Defined Message) reply table that
*                          identifies us to Raspberry Pi 5 as a Pi-class
*                          27 W supply (VDM_RPI_TX_Tab)
*                        - VBUS_set_5V() unconditionally in PD_Det_Proc on
*                          CC connect — vSafe5V + PA7 high + TPS REF must
*                          be set before SRC_CAP advertise (fixes cold-boot
*                          with Pi5-already-attached + USB-C-PD-on-input
*                          race; the old WCH STA_SRC_CONNECT branch was a
*                          sink-style "wait for incoming SRC_CAP" leftover
*                          that left TPS uninitialized in this scenario).
*                        - DR_Swap Accept (data-role swap to UFP) so a
*                          laptop attached to the output port can take the
*                          USB host role and enumerate the RP2040 CDC while
*                          being powered (dock-style behavior). PDO #1 has
*                          advertised Dual-Role Data since day one; until
*                          this change a DR_Swap got GoodCRC + dead air.
*                        - Spec-complete Soft_Reset handling (MessageID
*                          counter reset + SRC_CAP re-send after Accept);
*                          previously the sink's only way out was a
*                          SinkWaitCap timeout -> Hard Reset -> VBUS drop.
*                      USB-C INPUT PD is owned entirely by HUSB238 (U150);
*                      this firmware never acts as a sink, so dual-role and
*                      SINK FSM from earlier forks have been removed.
*********************************************************************************
* Copyright (c) 2023 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

#include "debug.h"
#include <string.h>
#include "PD_Process.h"
#include "tps55289.h"

/* VBUS_OUT rail control lives in main.c — TPS55289 + PA7 (VBUS_OUT_EN)
 * + PB0 (PDS_EN) — called from PD_Det_Proc when a sink connects so
 * vSafe5V is on the bus before SRC_CAP advertise. */
extern void VBUS_set_5V(void);
/* VBUS_disable() drops VBUS_OUT to vSafe0V (PA7 low + TPS55289 output off)
 * and clears the cached setpoint so the 2 s TPS re-apply watchdog in main.c
 * stops driving the rail. Called from STA_DISCONNECT so a sink unplug returns
 * the output to a safe, de-energized state instead of leaving the last
 * negotiated 9V/15V live on an open port. */
extern void VBUS_disable(void);

void USBPD_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

__attribute__ ((aligned(4))) uint8_t PD_Rx_Buf[ 34 ];                           /* PD receive buffer */
__attribute__ ((aligned(4))) uint8_t PD_Tx_Buf[ 34 ];                           /* PD send buffer */

/******************************************************************************/
UINT8 PD_Ack_Buf[ 2 ];                                                          /* PD-ACK buffer */

__IO UINT8  Tmr_Ms_Cnt_Last;                                                         /* System timer millisecond timing final value */
__IO UINT8  Tmr_Ms_Dlt;                                                              /* System timer millisecond timing this interval value */
PD_CONTROL PD_Ctl;                                                              /* PD Control Related Structures */
UINT8  Adapter_SrcCap[ 30 ];                                                    /* Contents of the SrcCap message for the adapter */

/* MessageID of the last DR_Swap we processed. A retransmitted DR_Swap
 * (our GoodCRC lost on the wire) carries the same MessageID; processing
 * it twice would toggle PD_Role back and desynchronize the data roles.
 * 0xFF = none seen; reset wherever the spec resets MessageID counters
 * (attach/detach via PD_PHY_Reset, Soft_Reset, Hard Reset). */
static UINT8 DRSwap_Last_MsgID = 0xFF;

/* PD event trace ring. printf on USART2 is invisible remotely (the
 * RP2040 deframer discards non-WUPS bytes), so ms-scale PD events
 * (DR_Swap, Soft_Reset, HRST) were unobservable on the bench — the
 * 2026-07-17 Mac DR_Swap debugging went blind because of it. Writers
 * here only record a byte (event | arg<<4); main.c drains the ring and
 * emits system.log frames OUTSIDE the PD dispatch path, so tracing adds
 * zero latency to tSenderResponse-critical replies. Main-loop context
 * only (PD_Main_Proc / PD_Det_Proc) — no ISR writers, no locking needed.
 * 16 deep; on overflow oldest entries are overwritten (diag only). */
volatile UINT16 PD_Evt_Buf[ 16 ];
volatile UINT8 PD_Evt_W = 0;
static void PD_Evt( UINT16 code_arg )
{
    PD_Evt_Buf[ PD_Evt_W & 0x0F ] = code_arg;
    PD_Evt_W++;
}

__IO UINT8  PDO_Len;

/* SrcCap Table — v3 advertises four fixed PDOs so Pi5 (and any
 * PD-aware sink) can pick whichever rail it wants up to TPS's limits.
 *   PDO #1: 5V  / 5A    — Pi5 nominal
 *   PDO #2: 9V  / 3A
 *   PDO #3: 12V / 2.25A
 *   PDO #4: 15V / 1.8A
 * Each PDO is 4 raw bytes (little-endian, USB-PD 3.0 fixed-supply
 * format with dual-role + externally-powered flags set). */
UINT8 SrcCap_5V5A_Tab[ 16 ] =
{
    /* PDO#1 byte3 0x0E (was 0x0A): adds bit26 USB Communications Capable.
     * The port genuinely carries USB data (RP2040 CDC on D+/D-), and a
     * post-DR_Swap macOS host appears to gate its USB bring-up on this
     * declaration — without it the device flashed for a fraction of a
     * second and was torn down (bench 2026-07-17). Pi 5 regression run
     * REQUIRED before this reaches production units (Tier B gate). */
    0xF4, 0x91, 0x01, 0x0E,   /* 5V 5A   */
    0x2C, 0xD1, 0x02, 0x00,   /* 9V 3A   */
    0xE1, 0xC0, 0x03, 0x00,   /* 12V 2.25A */
    0xB4, 0xB0, 0x04, 0x00,   /* 15V 1.8A */
};

/* VDM source reply — Raspberry Pi 27W Power Supply Identifier. Pi5
 * sends a VDM discovery (0xFF, 0xA0, 0x00, 0x01) over PD and only
 * unlocks its 5A current ceiling if it recognizes us as a Pi-class
 * supply. Replying with this exact 20-byte table is what flips that
 * bit. Without it Pi5 caps itself at 3A even on PDO #1. */
UINT8 VDM_RPI_TX_Tab[ 20 ] =
{
    0x41, 0xA0, 0x00, 0xFF,
    0x8A, 0x2E, 0xC0, 0x01,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0x00,
    0x01, 0x00, 0x80, 0x20,
};

/* Discover Identity ACK sent when WE are UFP (post-DR_Swap, host asking).
 * Honest identity — NOT the RPi-27W-PSU one (that is a DFP/source-side
 * spoof for the Pi5 5A unlock). Five VDOs:
 *   VDM hdr : SVID FF00, SVDM 2.0, ACK, cmd=Discover Identity
 *   ID hdr  : USB Device Capable (bit30) + Product Type UFP=Peripheral
 *             (bits29:27=010) + USB-C receptacle (bits22:21=10b) +
 *             VID 0x2E8A (Raspberry Pi — matches the RP2040 CDC behind
 *             this port). The bit30 declaration is what a host's policy
 *             wants to see before bringing up USB data.
 *   Cert    : XID 0
 *   Product : PID/bcdDevice (cosmetic)
 *   UFP VDO : USB2.0 device capability, highest speed USB2.0 */
UINT8 VDM_UFP_ID_ACK_Tab[ 20 ] =
{
    0x41, 0xA0, 0x00, 0xFF,
    0x8A, 0x2E, 0x40, 0x50,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x0A, 0x00,
    0x00, 0x00, 0x00, 0x21,
};

/* SrcCap Table */
UINT8 SrcCap_5V3A_Tab[ 4 ]  = { 0X2C, 0X91, 0X01, 0X3E };
UINT8 SrcCap_5V1A5_Tab[ 4 ] = { 0X96, 0X90, 0X01, 0X3E };
UINT8 SrcCap_5V2A_Tab[ 4 ]  = { 0XC8, 0X90, 0X01, 0X3E };
UINT8 SinkCap_5V1A_Tab[ 4 ] = { 0X64, 0X90, 0X01, 0X36 };

/* PD3.0 */
UINT8 SrcCap_Ext_Tab[ 28 ] =
{
    0X18, 0X80, 0X63, 0X00,
    0X00, 0X00, 0X00, 0X00,
    0X00, 0X00, 0X01, 0X00,
    0X00, 0X00, 0X07, 0X03,
    0X00, 0X00, 0X00, 0X00,
    0X00, 0X00, 0X00, 0X03,
    0X00, 0X12, 0X00, 0X00,
};

UINT8 Status_Ext_Tab[ 8 ] =
{
    0X06, 0X80, 0X16, 0X00,
    0X00, 0X00, 0X00, 0X00,
};

/*********************************************************************
 * @fn      USBPD_IRQHandler
 *
 * @brief   This function handles USBPD interrupt.
 *
 * @return  none
 */
void USBPD_IRQHandler(void)
{
    if(USBPD->STATUS & IF_RX_ACT)
    {
        USBPD->STATUS |= IF_RX_ACT;
        if( ( USBPD->STATUS & MASK_PD_STAT ) == PD_RX_SOP0 )
        {
            if( USBPD->BMC_BYTE_CNT >= 6 )
            {
                /* If GOODCRC, do not answer and ignore this reception */
                if( ( USBPD->BMC_BYTE_CNT != 6 ) || ( ( PD_Rx_Buf[ 0 ] & 0x1F ) != DEF_TYPE_GOODCRC ) )
                {
                    Delay_Us(30);                       /* Delay 30us, answer GoodCRC */
                    /* GoodCRC header byte0 carries our current Port Data
                     * Role in bit5 — after a DR_Swap we are UFP (0x41),
                     * before it DFP (0x61). Revision stays hardcoded 2.0
                     * as upstream shipped it (Pi-proven; conscious quirk).
                     * Single flag read, no printf — ISR rules per the
                     * 2026-07-11 soak findings. */
                    PD_Ack_Buf[ 0 ] = PD_Ctl.Flag.Bit.PD_Role ? 0x61 : 0x41;
                    PD_Ack_Buf[ 1 ] = ( PD_Rx_Buf[ 1 ] & 0x0E ) | PD_Ctl.Flag.Bit.Auto_Ack_PRRole;
                    USBPD->CONFIG |= IE_TX_END ;
                    PD_Phy_SendPack( 0, PD_Ack_Buf, 2, UPD_SOP0 );
                }
            }
        }
    }
    if(USBPD->STATUS & IF_TX_END)
    {
        /* Packet send completion interrupt (GoodCRC send completion interrupt only) */
        USBPD->PORT_CC1 &= ~CC_LVE;
        USBPD->PORT_CC2 &= ~CC_LVE;

        /* Interrupts are turned off and can be turned on after the main function has finished processing the data */
        NVIC_DisableIRQ(USBPD_IRQn);
        PD_Ctl.Flag.Bit.Msg_Recvd = 1;                                          /* Packet received flag */
        USBPD->STATUS |= IF_TX_END;
    }
    if(USBPD->STATUS & IF_RX_RESET)
    {
        USBPD->STATUS |= IF_RX_RESET;
        PD_SINK_Init( );
        /* NO printf here: this is the highest-priority ISR and printf
         * blocks on USART2 (shared with the WUPS binary stream) via an
         * unbounded TC spin — the exact pattern that wedged the 2026-07-11
         * soak units. Hard-reset visibility comes from the PD state
         * telemetry instead. */
    }
}

/*********************************************************************
 * @fn      PD_Rx_Mode
 *
 * @brief   This function uses to enter reception mode.
 *
 * @return  none
 */
void PD_Rx_Mode( void )
{
    USBPD->CONFIG |= PD_ALL_CLR;
    USBPD->CONFIG &= ~PD_ALL_CLR;
    USBPD->CONFIG |= IE_RX_ACT | IE_RX_RESET|PD_DMA_EN;
    USBPD->DMA = (UINT32)(UINT8 *)PD_Rx_Buf;
    USBPD->CONTROL &= ~PD_TX_EN;
    USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
    USBPD->CONTROL |= BMC_START;
    NVIC_EnableIRQ( USBPD_IRQn );
}

/*********************************************************************
 * @fn      PD_SRC_Init
 *
 * @brief   This function uses to initialize SRC mode.
 *
 * @return  none
 */
void PD_SRC_Init( )
{
    PD_Ctl.Flag.Bit.PR_Role = 1;                                          /* SRC mode */
    PD_Ctl.Flag.Bit.Auto_Ack_PRRole = 1;                                  /* Default auto-responder role is SRC */
    USBPD->PORT_CC1 = CC_CMP_66 | CC_PU_330;
    USBPD->PORT_CC2 = CC_CMP_66 | CC_PU_330;
}

/*********************************************************************
 * @fn      PD_SINK_Init
 *
 * @brief   This function uses to initialize SNK mode.
 *
 * @return  none
 */
void PD_SINK_Init( )
{
    PD_Ctl.Flag.Bit.PR_Role = 0;                                          /* SINK mode */
    PD_Ctl.Flag.Bit.Auto_Ack_PRRole = 0;                                  /* Default auto-responder role is SINK */
    USBPD->PORT_CC1 = CC_CMP_66 | CC_PD;
    USBPD->PORT_CC2 = CC_CMP_66 | CC_PD;
}

/*********************************************************************
 * @fn      PD_DataRole_Reset
 *
 * @brief   Restore the default Port Data Role (DFP for a source) and
 *          invalidate the DR_Swap dedup latch. For negotiation restarts
 *          that bypass PD_PHY_Reset — Power_Output_Restart's VBUS cycle
 *          (ups.power.cycle / enable-after-disable): the partner treats
 *          the VBUS drop as a fresh attach with default roles, so a role
 *          swapped in the previous session must not leak into the new
 *          one. Flag word is shared with the ISR — guard the RMW.
 *
 * @return  none
 */
void PD_DataRole_Reset( void )
{
    NVIC_DisableIRQ( USBPD_IRQn );
    PD_Ctl.Flag.Bit.PD_Role = 1;
    NVIC_EnableIRQ( USBPD_IRQn );
    DRSwap_Last_MsgID = 0xFF;
}

/*********************************************************************
 * @fn      PD_PHY_Reset
 *
 * @brief   This function uses to reset PD PHY.
 *
 * @return  none
 */
void PD_PHY_Reset( void )
{
    PD_Ctl.Flag.Bit.Msg_Recvd = 0;
    PD_Ctl.Msg_ID = 0;
    PD_Ctl.Flag.Bit.PD_Version = 1;
    PD_Ctl.Det_Cnt = 0;
    PD_Ctl.Flag.Bit.Connected = 0;
    PD_Ctl.PD_Comm_Timer = 0;
    PD_Ctl.PD_BusIdle_Timer = 0;
    PD_Ctl.Mode_Try_Cnt = 0x80;
    PD_Ctl.Flag.Bit.PD_Role = 1;
    DRSwap_Last_MsgID = 0xFF;
    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;
    PD_Ctl.PD_State = STA_IDLE;
    PD_Ctl.Flag.Bit.PD_Comm_Succ = 0;
    PD_SRC_Init( );
    PD_Rx_Mode( );
}

/*********************************************************************
 * @fn      PD_Init
 *
 * @brief   This function uses to initialize PD Registers.
 *
 * @return  none
 */
void PD_Init( void )
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);               /* Open PD I/O clock, AFIO clock and PD clock */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;
    USBPD->CONFIG = PD_DMA_EN;
    USBPD->STATUS = BUF_ERR | IF_RX_BIT | IF_RX_BYTE | IF_RX_ACT | IF_RX_RESET | IF_TX_END;
    /* Initialize all variables */
    memset( &PD_Ctl.PD_State, 0x00, sizeof( PD_CONTROL ) );
    Adapter_SrcCap[ 0 ] = 1;
    memcpy( &Adapter_SrcCap[ 1 ], SrcCap_5V3A_Tab, 4 );
    PD_PHY_Reset( );
    PD_Rx_Mode( );
}

/*********************************************************************
 * @fn      PD_Detect
 *
 * @brief   This function uses to detect CC connection.
 *
 * @return  0:No connection; 1:CC1 connection; 2:CC2 connection
 */
UINT8 PD_Detect( void )
{
    UINT8  ret = 0;
    UINT8  cmp_cc1 = 0;
    UINT8  cmp_cc2 = 0;

    if(PD_Ctl.Flag.Bit.Connected)                                       /*Detect disconnection*/
    {
        USBPD->PORT_CC1 &= ~( CC_CMP_Mask | PA_CC_AI );
        USBPD->PORT_CC1 |= CC_CMP_22;
        Delay_Us(2);
        if( USBPD->PORT_CC1 & PA_CC_AI )
        {
            cmp_cc1 = bCC_CMP_22;
        }
        USBPD->PORT_CC1 &= ~( CC_CMP_Mask | PA_CC_AI );
        USBPD->PORT_CC1 |= CC_CMP_66;

        USBPD->PORT_CC2 &= ~( CC_CMP_Mask | PA_CC_AI );
        USBPD->PORT_CC2 |= CC_CMP_22;
        Delay_Us(2);
        if( USBPD->PORT_CC2 & PA_CC_AI )
        {
            cmp_cc2 = bCC_CMP_22;
        }
        USBPD->PORT_CC2 &= ~( CC_CMP_Mask | PA_CC_AI );
        USBPD->PORT_CC2 |= CC_CMP_66;

        if((GPIOC->INDR & PIN_CC1) != (uint32_t)Bit_RESET)
        {
            cmp_cc1 |= bCC_CMP_220;
        }
        if((GPIOC->INDR & PIN_CC2) != (uint32_t)Bit_RESET)
        {
            cmp_cc2 |= bCC_CMP_220;
        }

        if( USBPD->PORT_CC1 & CC_PD )
        {
            /* SRC sample code does not handle SNK */
        }
        else
        {
            if (USBPD->CONFIG & CC_SEL)
            {
                if ((cmp_cc2 & bCC_CMP_220) == bCC_CMP_220)
                {
                    ret=0;
                }
                else
                {
                    ret = 2;
                }
            }
            else
            {
                if ((cmp_cc1 & bCC_CMP_220) == bCC_CMP_220)
                {
                    ret=0;
                }
                else
                {
                    ret = 1;
                }
            }
        }
    }
    else                                                                /*Detect insertion*/
    {
        USBPD->PORT_CC1 &= ~( CC_CMP_Mask|PA_CC_AI );
        USBPD->PORT_CC1 |= CC_CMP_22;
        Delay_Us(2);
        if( USBPD->PORT_CC1 & PA_CC_AI )
        {
            cmp_cc1 |= bCC_CMP_22;
        }
        USBPD->PORT_CC1 &= ~( CC_CMP_Mask|PA_CC_AI );
        USBPD->PORT_CC1 |= CC_CMP_66;
        Delay_Us(2);
        if( USBPD->PORT_CC1 & PA_CC_AI )
        {
            cmp_cc1 |= bCC_CMP_66;
        }
        if((GPIOC->INDR & PIN_CC1) != (uint32_t)Bit_RESET)
        {
            cmp_cc1 |= bCC_CMP_220;
        }

        USBPD->PORT_CC2 &= ~( CC_CMP_Mask|PA_CC_AI );
        USBPD->PORT_CC2 |= CC_CMP_22;
        Delay_Us(2);
        if( USBPD->PORT_CC2 & PA_CC_AI )
        {
            cmp_cc2 |= bCC_CMP_22;
        }
        USBPD->PORT_CC2 &= ~( CC_CMP_Mask|PA_CC_AI );
        USBPD->PORT_CC2 |= CC_CMP_66;
        Delay_Us(2);
        if( USBPD->PORT_CC2 & PA_CC_AI )
        {
            cmp_cc2 |= bCC_CMP_66;
        }
        if((GPIOC->INDR & PIN_CC2) != (uint32_t)Bit_RESET)
        {
            cmp_cc2 |= bCC_CMP_220;
        }

        if( USBPD->PORT_CC1 & CC_PD )
        {
           /* SRC sample code does not handle SNK */
        }
        else
        {
            if ((((cmp_cc1 & bCC_CMP_66) == bCC_CMP_66) & ((cmp_cc1 & bCC_CMP_220) == 0x00)) == 1)
            {
                if ((((cmp_cc2 & bCC_CMP_22) == bCC_CMP_22) & ((cmp_cc2 & bCC_CMP_66) == 0x00)) == 1)
                {
                  ret = 1;
                }
                if ((cmp_cc2 & bCC_CMP_220) == bCC_CMP_220)
                {
                  ret = 1;
                }
            }

            if ((((cmp_cc2 & bCC_CMP_66) == bCC_CMP_66) & ((cmp_cc2 & bCC_CMP_220) == 0x00)) == 1)
            {
                if(ret)
                {
                    ret = 0;
                }
                else
                {
                    if ((((cmp_cc1 & bCC_CMP_22) == bCC_CMP_22) && ((cmp_cc1 & bCC_CMP_66) == 0x00)) == 1)
                    {
                      ret = 2;
                    }
                    if ((cmp_cc1 & bCC_CMP_220) == bCC_CMP_220)
                    {
                      ret = 2;
                    }
                }
            }
        }
    }
    return( ret );
}

/*********************************************************************
 * @fn      PD_Det_Proc
 *
 * @brief   This function uses to process the return value of PD_Detect.
 *
 * @return  none
 */
void PD_Det_Proc( void )
{
    UINT8  status;
    status = PD_Detect( );
    if( PD_Ctl.Flag.Bit.Connected )
    {
        /* PD is connected, detect its disconnection */
        if( status )
        {
            PD_Ctl.Det_Cnt = 0;
        }
        else
        {
            PD_Ctl.Det_Cnt++;
            if( PD_Ctl.Det_Cnt >= 5 )
            {
                PD_Ctl.Det_Cnt = 0;
                PD_Ctl.Flag.Bit.Connected = 0;
                if( PD_Ctl.Flag.Bit.Stop_Det_Chk == 0 )
                {
                    PD_Ctl.PD_State = STA_DISCONNECT;
                }
            }
        }
    }
    else
    {
        /* PD is disconnected, check its connection */

        /* Determine connection status */
        if( status == 0 )
        {
            PD_Ctl.Det_Cnt = 0;
        }
        else
        {
            PD_Ctl.Det_Cnt++;
        }
        if( PD_Ctl.Det_Cnt >= 5 )
        {
            PD_Ctl.Det_Cnt = 0;
            PD_Ctl.Flag.Bit.Connected = 1;
            if( PD_Ctl.Flag.Bit.Stop_Det_Chk == 0 )
            {
                /* Select the corresponding PD channel */
                if( status == 1 )
                {
                    USBPD->CONFIG &= ~CC_SEL;
                }
                else
                {
                    USBPD->CONFIG |= CC_SEL;
                }
                /* v3 hardware: source-only. The upstream WCH sample
                 * branches into STA_SRC_CONNECT when CC_PD is asserted
                 * (a leftover "wait for incoming SRC_CAP" path that
                 * fits a dual-role design where Source could also be
                 * Sink). On v3 that branch never makes sense — every
                 * CC connect we see is a sink attaching. Always go to
                 * STA_SINK_CONNECT (misleadingly named in WCH style —
                 * it is the source-side "ready to TX SRC_CAP" pre-state)
                 * and unconditionally bring the rail up via VBUS_set_5V
                 * first so vSafe5V is on the bus before SRC_CAP.
                 * Skipping VBUS_set_5V is the cold-boot bug we shipped
                 * before this refactor — see commit history. */
                PD_Ctl.PD_State = STA_SINK_CONNECT;
                PD_Evt( PD_EVT_CONNECT );
                printf("CC%d Connect (CC_PD=%lu)\r\n", status,
                       (unsigned long)(((USBPD->PORT_CC1 & CC_PD) ||
                                        (USBPD->PORT_CC2 & CC_PD)) ? 1 : 0));
                VBUS_set_5V();
                PD_Ctl.PD_Comm_Timer = 0;
            }
        }
    }
}

/*********************************************************************
 * @fn      PD_Phy_SendPack
 *
 * @brief   This function uses to send PD data.
 *
 * @return  none
 */
void PD_Phy_SendPack( UINT8 mode, UINT8 *pbuf, UINT8 len, UINT8 sop )
{

    if ((USBPD->CONFIG & CC_SEL) == CC_SEL )
    {
        USBPD->PORT_CC2 |= CC_LVE;
    }
    else
    {
        USBPD->PORT_CC1 |= CC_LVE;
    }

    USBPD->BMC_CLK_CNT = UPD_TMR_TX_48M;

    USBPD->DMA = (UINT32)(UINT8 *)pbuf;

    USBPD->TX_SEL = sop;

    USBPD->BMC_TX_SZ = len;
    USBPD->CONTROL |= PD_TX_EN;
    USBPD->STATUS &= BMC_AUX_INVALID;
    USBPD->CONTROL |= BMC_START;

    /* Determine if you need to wait for the send to complete */
    if( mode )
    {
        /* Bounded wait for TX complete. "This will definitely complete"
         * holds only for a healthy PHY — a disturbance mid-transmission
         * (supply transient, CC glitch) can abort the BMC engine without
         * IF_TX_END ever setting, and an unbounded spin here wedges the
         * main loop permanently (2026-07-11 soak failure class). Guard
         * ≈25-40 ms measured from the compiled loop (~6-10 cycles/iter
         * @48 MHz) >> ~1-3 ms worst-case legitimate TX; on expiry fall
         * through to the RX cleanup — the peer's SenderResponse timeout
         * and PD_Send_Handle()'s retry logic cover the lost frame. */
        {
            UINT32 tx_guard = 200000;
            while( ((USBPD->STATUS & IF_TX_END) == 0) && --tx_guard );
        }
        USBPD->STATUS |= IF_TX_END;
        if((USBPD->CONFIG & CC_SEL) == CC_SEL )
        {
            USBPD->PORT_CC2 &= ~CC_LVE;
        }
        else
        {
            USBPD->PORT_CC1 &= ~CC_LVE;
        }

        /* Switch to receive ready to receive GoodCRC */
        USBPD->CONFIG |=  PD_ALL_CLR ;
        USBPD->CONFIG &= ~( PD_ALL_CLR );
        USBPD->CONTROL &= ~ ( PD_TX_EN );
        USBPD->DMA = (UINT32)(UINT8 *)PD_Rx_Buf;
        USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
        USBPD->CONTROL |= BMC_START;
    }
}

/*********************************************************************
 * @fn      PD_Load_Header
 *
 * @brief   This function uses to load pd header packets.
 *
 * @return  none
 */
void PD_Load_Header( UINT8 ex, UINT8 msg_type )
{
    /* Message Header
       BIT15 - Extended;
       BIT[14:12] - Number of Data Objects
       BIT[11:9] - Message ID
       BIT8 - PortPower Role/Cable Plug  0: SINK; 1: SOURCE
       BIT[7:6] - Revision, 00: V1.0; 01: V2.0; 10: V3.0;
       BIT5 - Port Data Role, 0: UFP; 1: DFP
       BIT[4:0] - Message Type
    */
    PD_Tx_Buf[ 0 ] = msg_type;
    if( PD_Ctl.Flag.Bit.PD_Role )
    {
        PD_Tx_Buf[ 0 ] |= 0x20;
    }
    if( PD_Ctl.Flag.Bit.PD_Version )
    {
        /* PD3.0 */
        PD_Tx_Buf[ 0 ] |= 0x80;
    }
    else
    {
        /* PD2.0 */
        PD_Tx_Buf[ 0 ] |= 0x40;
    }

    PD_Tx_Buf[ 1 ] = PD_Ctl.Msg_ID & 0x0E;
    if( PD_Ctl.Flag.Bit.PR_Role )
    {
        PD_Tx_Buf[ 1 ] |= 0x01;
    }
    if( ex )
    {
        PD_Tx_Buf[ 1 ] |= 0x80;
    }
}

/*********************************************************************
 * @fn      PD_Send_Handle
 *
 * @brief   This function uses to handle sending transactions.
 *
 * @return  0:success; 1:fail
 */
UINT8 PD_Send_Handle( UINT8 *pbuf, UINT8 len )
{
    UINT8  pd_tx_trycnt;
    UINT8  cnt;

    if( ( len % 4 ) != 0 )
    {
        /* Send failed */
        return( DEF_PD_TX_FAIL );
    }
    if( len > 28 )
    {
        /* Send failed */
        return( DEF_PD_TX_FAIL );
    }

    cnt = len >> 2;
    PD_Tx_Buf[ 1 ] |= ( cnt << 4 );
    for( cnt = 0; cnt != len; cnt++ )
    {
        PD_Tx_Buf[ 2 + cnt ] = pbuf[ cnt ];
    }

    pd_tx_trycnt = 4;
    while( --pd_tx_trycnt )                                                     /* Maximum 3 executions */
    {
        NVIC_DisableIRQ( USBPD_IRQn );
        PD_Phy_SendPack( 0x01, PD_Tx_Buf, ( len + 2 ), UPD_SOP0 );

        /* Set receive timeout 750US */
        cnt = 250;
        while( --cnt )
        {
            if( (USBPD->STATUS & IF_RX_ACT) == IF_RX_ACT)
            {
                USBPD->STATUS |= IF_RX_ACT;
                if( ( USBPD->BMC_BYTE_CNT == 6 ) && ( ( PD_Rx_Buf[ 0 ] & 0x1F ) == DEF_TYPE_GOODCRC ) )
                {
                    PD_Ctl.Msg_ID += 2;
                    break;
                }
            }
            Delay_Us( 3 );
        }
        if( cnt !=0 )
        {
            break;
        }
    }

    /* Switch to receive mode */
    PD_Rx_Mode( );
    if( pd_tx_trycnt )
    {
        /* Send successful */
        return( DEF_PD_TX_OK );
    }
    else
    {
        /* Send failed */
        return( DEF_PD_TX_FAIL );
    }
}

/*********************************************************************
 * @fn      PD_Request_Analyse
 *
 * @brief   This function uses to analyse PDO's voltage and current.
 *
 * @return  none
 */
void PD_Request_Analyse( UINT8 pdo_idx, UINT8 *srccap, UINT16 *current )
{
    UINT32 temp32;

    temp32 = srccap[ (  ( pdo_idx - 1 ) << 2 ) + 0 ] +
                        ( (UINT32)srccap[ ( ( pdo_idx - 1 ) << 2 ) + 1 ] << 8 );

    /* Calculation of current values */
    if( current != NULL )
    {
        *current = ( temp32 & 0x000003FF ) * 10;
    }

}

/*********************************************************************
 * @fn      PD_Main_Proc
 *
 * @brief   This function uses to process PD status.
 *
 * @return  none
 */
void PD_Main_Proc( )
{
    UINT8  status;
    UINT8  pd_header;
    UINT16 Current;

    /* Receive idle timer count */
    PD_Ctl.PD_BusIdle_Timer += Tmr_Ms_Dlt;

    /* Status analysis processing */
    switch( PD_Ctl.PD_State )
    {
        case STA_DISCONNECT:
            PD_Evt( PD_EVT_DISCONNECT );
            printf("Disconnect\r\n");
            /* Sink detached (CC open for >=5 detect ticks). Return VBUS_OUT
             * to vSafe0V *before* anything else: clears the TPS55289 output
             * and the cached 9V/15V setpoint so the main-loop 2 s watchdog no
             * longer re-applies the stale contract and pins the rail high.
             * Leaving voltage on an unattached USB-C port is both a Type-C
             * violation and a real hazard with a power-source sink (a
             * powerbank back-feeding into our still-driven 15V rail was
             * resetting the whole unit). A fresh attach re-arms 5V via
             * VBUS_set_5V() in PD_Det_Proc, so dropping to 0V here is safe. */
            VBUS_disable();
#if(Lowpower==LowpowerON)
#if(Wake_up_mode==USBPDWake_up)
            RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
            EXTI_ClearITPendingBit(EXTI_Line29);
            USBPD->PORT_CC1&=~(CC_PU_Mask);
            USBPD->PORT_CC2&=~(CC_PU_Mask);
            USBPD->PORT_CC1|=CC_PU_80;
            USBPD->PORT_CC2|=CC_PU_80;
            USBPD->PORT_CC1|=CC_CMP_123;
            USBPD->PORT_CC2|=CC_CMP_123;
            USBPD->CONFIG&=~WAKE_POLAR;
            NVIC_DisableIRQ(USBPD_IRQn);
            USBPD->CONFIG|=IE_PD_IO;
            printf("Fell deep sleep\r\n");
            NVIC_EnableIRQ(USBPDWakeUp_IRQn);
            Delay_Ms(100);
            PWR_EnterSTANDBYMode();
#elif(Wake_up_mode==GPIOWake_up)
            RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
            EXTI_ClearITPendingBit(EXTI_Line14);
            EXTI_ClearITPendingBit(EXTI_Line15);
            printf("Fell deep sleep\r\n");
            NVIC_EnableIRQ(EXTI15_8_IRQn);
            Delay_Ms(100);
            PWR_EnterSTANDBYMode();
#endif
#elif(Lowpower==LowpowerOff)

#endif
            PD_PHY_Reset( );
            break;

        case STA_SINK_CONNECT:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;

            if( PD_Ctl.PD_Comm_Timer > 159 )
            {
              PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;
              PD_Ctl.PD_Comm_Timer = 0;
              PD_Ctl.PD_State = STA_TX_SRC_CAP;
            }
            break;

        case STA_TX_SRC_CAP:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;

            if( PD_Ctl.PD_Comm_Timer > 159 )
            {
                PD_Load_Header( 0x00, DEF_TYPE_SRC_CAP );
                status = PD_Send_Handle(SrcCap_5V5A_Tab, 16 );
                if( status == DEF_PD_TX_OK )
                {
                    PD_Ctl.PD_State = STA_RX_REQ_WAIT;
                    PD_Evt( PD_EVT_SRC_CAP_TX );
                    printf("Send Source Cap Successfully\r\n");
                }
                PD_Ctl.PD_Comm_Timer = 0;
            }
            break;

        case STA_RX_REQ_WAIT:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
            if( PD_Ctl.PD_Comm_Timer > 29 )
            {
                /* Don't fire the timeout when a received message already
                 * sits in the buffer awaiting dispatch: a single main-loop
                 * iteration with the 2 s/5 s telemetry blocks can exceed
                 * 29 ms, and this switch runs BEFORE the RX dispatch — so
                 * a Request that arrived in time would get pre-empted by
                 * our own latency. HRSTing then is doubly harmful since
                 * the HRST path resets the data role and tears down a
                 * just-established DR_Swap session. */
                if( PD_Ctl.Flag.Bit.Msg_Recvd == 0 )
                {
                    PD_Ctl.PD_State = STA_TX_HRST;
                }
            }
            break;

        case STA_TX_ACCEPT:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
            if( PD_Ctl.PD_Comm_Timer > 2 )
            {
                PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                status = PD_Send_Handle( NULL, 0 );
                if( status == DEF_PD_TX_OK )
                {
                    PD_Evt( PD_EVT_ACCEPT_TX );
                    printf("Accept\r\n");
                    /* Reconfigure TPS55289 to match the PDO the sink
                     * asked for. ReqPDO_Idx is 1..4 per our SrcCap;
                     * any out-of-range value would have been rejected
                     * in the REQUEST handler before reaching here. */
                    /* curr is the TPS55289 current limit, set a few % above the
                     * negotiated PDO current so the rail can source full rated
                     * power (≈27 W) under test load without the converter folding
                     * into constant-current at the profile boundary. Limits sit
                     * on the 0.1 A grid so the 2 s watchdog re-apply (from the
                     * 0.1 A telemetry cache) reproduces them exactly. */
                    float volt = 5.0f;
                    float curr = 3.0f;
                    switch (PD_Ctl.ReqPDO_Idx)
                    {
                        case 1: volt = 5.0f;  curr = 5.2f; break;  /* PDO 5V/5A     -> 5.2A (+4.0%) */
                        case 2: volt = 9.0f;  curr = 3.1f; break;  /* PDO 9V/3A     -> 3.1A (+3.3%) */
                        case 3: volt = 12.0f; curr = 2.3f; break;  /* PDO 12V/2.25A -> 2.3A (+2.2%) */
                        case 4: volt = 15.0f; curr = 1.9f; break;  /* PDO 15V/1.8A  -> 1.9A (+5.6%) */
                        default: /* keep safe defaults */    break;
                    }
                    tps55289_set_cdc_compensation(CDC_COMP_0V7);
                    tps55289_set_current_limit(curr);
                    tps55289_set_voltage(volt);
                    tps55289_enable_output(1);
                    PD_Ctl.PD_State = STA_TX_PS_RDY;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
                else
                {
                    PD_Ctl.PD_State = STA_TX_SOFTRST;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
            }
            break;

        case STA_TX_PS_RDY:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
            if( PD_Ctl.PD_Comm_Timer > 19 )
            {
                PD_Load_Header( 0x00, DEF_TYPE_PS_RDY );
                status = PD_Send_Handle( NULL, 0 );
                if( status == DEF_PD_TX_OK )
                {
                    PD_Evt( PD_EVT_PS_RDY_TX );
                    printf("PS ready\r\n");
                    PD_Ctl.PD_State = STA_IDLE;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
                else
                {
                    PD_Ctl.PD_State = STA_TX_SOFTRST;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
            }
            break;

        case STA_TX_SOFTRST:
            /* Send soft reset, if sent successfully, mode unchanged, count +1 for retry */
            /* Soft_Reset restarts the partner's MessageID counter, so a
             * DR_Swap MessageID latched before it is meaningless after. */
            DRSwap_Last_MsgID = 0xFF;
            PD_Evt( PD_EVT_SOFTRST_TX );
            PD_Load_Header( 0x00, DEF_TYPE_SOFT_RESET );
            status = PD_Send_Handle( NULL, 0 );
            if( status == DEF_PD_TX_OK )
            {
                /* current mode unchanged, jump to initial state of current mode, mode retry count, switch mode if exceeded */
                PD_Ctl.PD_State = STA_IDLE;
            }
            else
            {
                PD_Ctl.PD_State = STA_TX_HRST;
            }
            PD_Ctl.PD_Comm_Timer = 0;
            break;

        case STA_TX_HRST:
            /* Sending a hard reset. Disable the USBPD IRQ around the
             * blocking (mode=1) send — same pattern as PD_Send_Handle():
             * with the IRQ enabled the ISR's TX_END branch W1C-clears
             * IF_TX_END before our wait loop sees it (burning the full
             * guard) and sets a spurious Msg_Recvd that would re-dispatch
             * the STALE PD_Rx_Buf right after the Hard Reset, overriding
             * the STA_IDLE recovery state. */
            PD_Ctl.Flag.Bit.Stop_Det_Chk = 1;
            PD_Evt( PD_EVT_HRST_TX );
            NVIC_DisableIRQ( USBPD_IRQn );
            PD_Phy_SendPack( 0x01, NULL, 0, UPD_HARD_RESET );                   /* send HRST */
            PD_Ctl.Flag.Bit.Msg_Recvd = 0;                                      /* drop any stale RX */
            /* Hard Reset returns the port to its default data role (DFP
             * for a source) and resets the MessageID counters — matters
             * only if a DR_Swap happened earlier in this session; both
             * writes sit inside the IRQ-off window above. */
            PD_Ctl.Flag.Bit.PD_Role = 1;
            DRSwap_Last_MsgID = 0xFF;
            PD_Rx_Mode( );                                                      /* switch to rx mode (re-enables IRQ) */
            PD_Ctl.PD_State = STA_IDLE;
            PD_Ctl.PD_Comm_Timer = 0;
            break;

        case STA_TX_VDM_RPI:
            /* Reply to Pi5's VDM discovery with the Pi-class supply ID.
             * Pi5 only unlocks 5A on PDO #1 if it recognizes the source
             * via this VDM exchange — otherwise it caps itself at 3A. */
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
            if( PD_Ctl.PD_Comm_Timer > 10 )
            {
                PD_Load_Header( 0x00, DEF_TYPE_VENDOR_DEFINED );
                (void)PD_Send_Handle(VDM_RPI_TX_Tab, 20);
                PD_Evt( PD_EVT_VDM_RPI_TX );
                PD_Ctl.PD_Comm_Timer = 0;
                PD_Ctl.PD_State = STA_IDLE;
            }
            break;

        default:
            break;
    }

    /* Receive message processing */
    if( PD_Ctl.Flag.Bit.Msg_Recvd )
    {
        /* Adapter communication idle timing */
        PD_Ctl.Adapter_Idle_Cnt = 0x00;
        pd_header = PD_Rx_Buf[ 0 ] & 0x1F;
        /* Any dispatched non-DR_Swap message proves the DR_Swap dedup
         * latch stale: a true retransmission arrives back-to-back before
         * anything else gets processed, while the 3-bit MessageID space
         * wraps and would otherwise alias a fresh swap-back into
         * "duplicate" forever (~1-in-8 swap-backs swallowed). */
        if( pd_header != DEF_TYPE_DR_SWAP )
        {
            DRSwap_Last_MsgID = 0xFF;
        }
        switch( pd_header )
        {
            case DEF_TYPE_ACCEPT:
                PD_Ctl.PD_Comm_Timer = 0;
                if( PD_Ctl.PD_State == STA_RX_ACCEPT_WAIT )
                {
                    PD_Ctl.PD_State = STA_RX_PS_RDY_WAIT;
                }
                break;

            case DEF_TYPE_REQUEST:
                /* Request is received */
                printf("Handle Request\r\n");
                Delay_Ms( 2 );
                PD_Ctl.ReqPDO_Idx =  ( PD_Rx_Buf[ 5 ] & 0x70 ) >> 4;
                PD_Evt( PD_EVT_REQ_RX | (UINT8)( PD_Ctl.ReqPDO_Idx << 4 ) );
                printf("  Request:\r\n  PDO_Idx:%d\r\n",PD_Ctl.ReqPDO_Idx);
                /* v3 advertises 4 PDOs (see SrcCap_5V5A_Tab). Reject
                 * anything outside that range — upstream allowed up to
                 * 7 because it left room for an 8-PDO source cap; we
                 * cap at 4 so an out-of-range PDO request gets HRST
                 * rather than landing on the default fallback in
                 * STA_TX_ACCEPT. */
                if( ( PD_Ctl.ReqPDO_Idx == 0 ) || ( PD_Ctl.ReqPDO_Idx > 4 ) )
                {
                    PD_Ctl.PD_State = STA_TX_HRST;
                }
                else
                {
                    PD_Request_Analyse( 1, &PD_Rx_Buf[ 2 ], &Current );
                    printf("  Current:%d mA\r\n",Current);
                    if( ( PD_Rx_Buf[ 0 ] & 0xC0 ) == 0x80 )
                    {
                        /* PD3.0 */
                        PD_Ctl.Flag.Bit.PD_Version = 1;
                    }
                    else
                    {
                        PD_Ctl.Flag.Bit.PD_Version = 0;
                    }

                    PD_Ctl.PD_State = STA_TX_ACCEPT;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
                break;

            case DEF_TYPE_WAIT:
                /* WAIT received, many requests may receive WAIT, need specific analysis */
                break;

            case DEF_TYPE_SOFT_RESET:
                /* Spec-complete Soft_Reset (PD3.0 §6.3.13/§6.7.1): reset
                 * the MessageID counter BEFORE loading the Accept header
                 * (the Accept itself must carry MessageID 0), then re-send
                 * Source_Capabilities via STA_TX_SRC_CAP. Previously we
                 * Accepted and went silent — the sink's only recovery was
                 * a SinkWaitCap timeout -> Hard Reset -> full VBUS drop.
                 * The 160 ms STA_TX_SRC_CAP lead-in sits comfortably inside
                 * the sink's tTypeCSinkWaitCap window (min 310 ms). */
                PD_Ctl.Msg_ID = 0;
                DRSwap_Last_MsgID = 0xFF;
                PD_Evt( PD_EVT_SOFTRST_RX );
                Delay_Ms( 1 );
                PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                PD_Send_Handle( NULL, 0 );
                PD_Ctl.PD_State = STA_TX_SRC_CAP;
                PD_Ctl.PD_Comm_Timer = 0;
                break;

            case DEF_TYPE_DR_SWAP:
                /* Data Role Swap request. Control message only: type 0x09
                 * with NumDO!=0 is PD3.1 EPR_REQUEST — not for us. */
                if( ( PD_Rx_Buf[ 1 ] & 0x70 ) == 0x00 )
                {
                    if( PD_Ctl.PD_State == STA_IDLE )
                    {
                        /* Same MessageID as the last processed DR_Swap =
                         * retransmission (our GoodCRC was lost); the ISR
                         * has re-acked it, do not toggle the role again. */
                        UINT8 rx_msgid = ( PD_Rx_Buf[ 1 ] >> 1 ) & 0x07;
                        if( DRSwap_Last_MsgID != rx_msgid )
                        {
                            PD_Evt( PD_EVT_DRSWAP_RX );
                            /* printf BEFORE the Accept, not after: post-
                             * Accept the partner's next message can arrive
                             * within ms, and every us of straight-line code
                             * before the dispatch tail's PD_Rx_Mode widens
                             * the GoodCRC'd-then-dropped window. Here the
                             * IRQ is still off (no RX possible) and the
                             * ~0.4 ms is peanuts vs tSenderResponse. */
                            printf("DR_Swap: Accepting\r\n");
                            Delay_Ms( 1 );
                            PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                            if( PD_Send_Handle( NULL, 0 ) == DEF_PD_TX_OK )
                            {
                                DRSwap_Last_MsgID = rx_msgid;
                                /* Roles change once the Accept is GoodCRC'd.
                                 * Toggle (not force-0): a second DR_Swap
                                 * legally swaps back. Flag word is shared
                                 * with the ISR (Msg_Recvd) and PD_Send_Handle
                                 * re-enabled the IRQ, so guard the RMW. */
                                NVIC_DisableIRQ( USBPD_IRQn );
                                PD_Ctl.Flag.Bit.PD_Role ^= 1;
                                NVIC_EnableIRQ( USBPD_IRQn );
                                PD_Evt( PD_EVT_DRSWAP_ACC |
                                        (UINT8)( PD_Ctl.Flag.Bit.PD_Role << 4 ) );
                            }
                            else
                            {
                                PD_Evt( PD_EVT_DRSWAP_FAIL );
                                /* Accept may have been DELIVERED with all
                                 * three GoodCRCs back to us lost — partner
                                 * swapped, we did not. Ambiguous roles are
                                 * worse than a reset: Hard Reset restores
                                 * default data roles on both ends (and our
                                 * STA_TX_HRST now re-arms PD_Role/latch). */
                                PD_Ctl.PD_State = STA_TX_HRST;
                            }
                        }
                        else
                        {
                            PD_Evt( PD_EVT_DRSWAP_RX | ( 1 << 4 ) );    /* dup-skip */
                        }
                    }
                    else
                    {
                        /* Mid-negotiation — tell the partner to retry
                         * instead of the old GoodCRC-then-silence, which
                         * is a protocol error that invites Soft_Reset. */
                        PD_Evt( PD_EVT_DRSWAP_RX | ( 2 << 4 ) );        /* busy -> Wait */
                        Delay_Ms( 1 );
                        PD_Load_Header( 0x00, DEF_TYPE_WAIT );
                        PD_Send_Handle( NULL, 0 );
                    }
                }
                break;

            case DEF_TYPE_VENDOR_DEFINED:
                /* Pi5 sends a VDM discovery 0xFF, 0xA0, 0x00, 0x01 over
                 * PD. Match that exact prefix in the VDM header bytes
                 * and queue the canned VDM reply (STA_TX_VDM_RPI).
                 * Other VDM senders are ignored (we just print a note).
                 * Gated on PD_Role: a post-DR_Swap host (Mac/PC as DFP)
                 * sends a byte-identical SVDM-2.0 Discover Identity and
                 * must NOT be told we are a Raspberry Pi 27W PSU
                 * (VID 0x2E8A) — the Pi path always queries while we
                 * are still DFP, so it is unaffected. */
                if((PD_Ctl.Flag.Bit.PD_Role == 1) &&
                   (PD_Rx_Buf[2] == 0x01) && (PD_Rx_Buf[3] == 0xA0) &&
                   (PD_Rx_Buf[4] == 0x00) && (PD_Rx_Buf[5] == 0xFF))
                {
                    PD_Ctl.PD_State = STA_TX_VDM_RPI;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
                else if( ( PD_Ctl.Flag.Bit.PD_Role == 0 ) &&
                         ( ( PD_Rx_Buf[ 3 ] & 0x80 ) == 0x80 ) &&
                         ( ( PD_Rx_Buf[ 2 ] & 0xC0 ) == 0x00 ) )
                {
                    /* We are UFP (post-DR_Swap) and this is a Structured
                     * VDM REQuest (initiator, cmd-type=00). PD3.0 requires
                     * a UFP to answer Discover Identity/SVIDs/Modes with
                     * ACK or NAK — GoodCRC-then-silence is a protocol
                     * error that sends macOS into a Soft_Reset -> re-swap
                     * loop (bench 2026-07-17, first flapping session).
                     * Discover Identity gets the honest UFP ACK — the
                     * "USB Device Capable" bit in it is host-policy food;
                     * a NAK here left macOS assuming no USB data and it
                     * tore the port down right after electrical attach.
                     * Everything else (Discover SVIDs/Modes — we have no
                     * alternate modes) gets a NAK echo. */
                    if( ( PD_Rx_Buf[ 2 ] & 0x1F ) == 0x01 )
                    {
                        Delay_Ms( 1 );
                        PD_Load_Header( 0x00, DEF_TYPE_VENDOR_DEFINED );
                        PD_Send_Handle( VDM_UFP_ID_ACK_Tab, 20 );
                        PD_Evt( PD_EVT_PROTO_REPLY | ( 5 << 4 ) );      /* UFP ID ACK */
                    }
                    else
                    {
                        UINT8 vdm_nak[ 4 ];
                        vdm_nak[ 0 ] = ( PD_Rx_Buf[ 2 ] & 0x1F ) | 0x80;    /* same command, cmd-type=NAK */
                        vdm_nak[ 1 ] = PD_Rx_Buf[ 3 ];                      /* same SVDM version/obj-pos */
                        vdm_nak[ 2 ] = PD_Rx_Buf[ 4 ];                      /* same SVID */
                        vdm_nak[ 3 ] = PD_Rx_Buf[ 5 ];
                        Delay_Ms( 1 );
                        PD_Load_Header( 0x00, DEF_TYPE_VENDOR_DEFINED );
                        PD_Send_Handle( vdm_nak, 4 );
                        PD_Evt( PD_EVT_PROTO_REPLY | ( 1 << 4 ) );          /* VDM NAK */
                    }
                }
                printf("VDM Command\r\n");
                break;

            case DEF_TYPE_GET_SRC_CAP:
                /* Answer inline: the STA_TX_SRC_CAP state has a 160 ms
                 * lead-in dwell (attach pacing), far beyond tSenderResponse
                 * (~30 ms) — routing through it would time the asker out. */
                Delay_Ms( 1 );
                PD_Load_Header( 0x00, DEF_TYPE_SRC_CAP );
                PD_Send_Handle( SrcCap_5V5A_Tab, 16 );
                PD_Evt( PD_EVT_PROTO_REPLY | ( 3 << 4 ) );              /* GetSrcCap answered */
                break;

            case DEF_TYPE_GET_SNK_CAP:
                /* Type 0x08 is TWO different messages: control Get_Sink_Cap
                 * and PD3.1 DATA message Enter_USB (1 EUDO). */
                if( ( PD_Rx_Buf[ 1 ] & 0x70 ) != 0x00 )
                {
                    /* Enter_USB: the post-DR_Swap DFP (Mac) asks to
                     * establish the USB data path. EUDO bits 30:28 =
                     * USB mode: 0=USB2, 1=USB3.2, 2=USB4. The RP2040 CDC
                     * behind our D+/D- is USB2-only: Accept USB2, Reject
                     * higher modes. Not_Supported here told macOS "no USB
                     * data at all" and it kept the host path disabled —
                     * observed 2026-07-17 as "device never appears". */
                    UINT8 usb_mode = ( PD_Rx_Buf[ 5 ] >> 4 ) & 0x07;
                    Delay_Ms( 1 );
                    if( usb_mode == 0 )
                    {
                        PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                        PD_Send_Handle( NULL, 0 );
                        PD_Evt( PD_EVT_ENTER_USB | ( 1 << 4 ) |
                                ( (UINT16)usb_mode << 8 ) );
                    }
                    else
                    {
                        PD_Load_Header( 0x00, DEF_TYPE_REJECT );
                        PD_Send_Handle( NULL, 0 );
                        PD_Evt( PD_EVT_ENTER_USB | ( 0 << 4 ) |
                                ( (UINT16)usb_mode << 8 ) );
                    }
                }
                else
                {
                    /* Get_Sink_Cap: source-only port, no sink capability. */
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x00, PD_Ctl.Flag.Bit.PD_Version ?
                                    DEF_TYPE_NOT_SUPPORT : DEF_TYPE_REJECT );
                    PD_Send_Handle( NULL, 0 );
                    PD_Evt( PD_EVT_PROTO_REPLY | ( 2 << 4 ) |
                            ( (UINT16)DEF_TYPE_GET_SNK_CAP << 8 ) );
                }
                break;

            case DEF_TYPE_VCONN_SWAP:
                /* This board never sources VCONN (no e-marker support,
                 * zero VCONN circuitry) — Reject is the honest answer;
                 * silence would be a PD3 protocol error. */
                Delay_Ms( 1 );
                PD_Load_Header( 0x00, DEF_TYPE_REJECT );
                PD_Send_Handle( NULL, 0 );
                PD_Evt( PD_EVT_PROTO_REPLY | ( 4 << 4 ) );              /* VCONN Reject */
                break;

            default:
                /* PD3.0 requires Not_Supported for recognized-but-
                 * unsupported and unrecognized messages; GoodCRC-then-
                 * silence makes a strict partner (macOS post-DR_Swap)
                 * escalate: tSenderResponse timeout -> Soft_Reset ->
                 * renegotiate -> re-swap -> ... an endless data-role
                 * ping-pong with the power contract staying up (bench
                 * 2026-07-17). Under PD2.0 ignoring stays legal and is
                 * the shipped Pi-proven behavior, so gate on the
                 * negotiated revision. */
                if( PD_Ctl.Flag.Bit.PD_Version )
                {
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x00, DEF_TYPE_NOT_SUPPORT );
                    PD_Send_Handle( NULL, 0 );
                    /* ext byte: raw 5-bit type, bit7 set when it was a
                     * DATA message — tells the bench WHAT the partner
                     * asked for that we don't handle. */
                    PD_Evt( PD_EVT_PROTO_REPLY | ( 2 << 4 ) |
                            ( (UINT16)( ( PD_Rx_Buf[ 0 ] & 0x1F ) |
                              ( ( PD_Rx_Buf[ 1 ] & 0x70 ) ? 0x80 : 0x00 ) ) << 8 ) );
                }
                printf("Unsupported Command\r\n");
                break;
        }

        /* Message has been processed, interrupt reception is turned on again */
        PD_Rx_Mode( );
        PD_Ctl.Flag.Bit.Msg_Recvd = 0;                                    /* Clear the received flag */
        PD_Ctl.PD_BusIdle_Timer = 0;                                      /* Idle time cleared */
    }
}
