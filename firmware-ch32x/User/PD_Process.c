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

void USBPD_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

__attribute__ ((aligned(4))) uint8_t PD_Rx_Buf[ 34 ];                           /* PD receive buffer */
__attribute__ ((aligned(4))) uint8_t PD_Tx_Buf[ 34 ];                           /* PD send buffer */

/******************************************************************************/
UINT8 PD_Ack_Buf[ 2 ];                                                          /* PD-ACK buffer */

__IO UINT8  Tmr_Ms_Cnt_Last;                                                         /* System timer millisecond timing final value */
__IO UINT8  Tmr_Ms_Dlt;                                                              /* System timer millisecond timing this interval value */
PD_CONTROL PD_Ctl;                                                              /* PD Control Related Structures */
UINT8  Adapter_SrcCap[ 30 ];                                                    /* Contents of the SrcCap message for the adapter */

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
    0xF4, 0x91, 0x01, 0x0A,   /* 5V 5A   */
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
                    PD_Ack_Buf[ 0 ] = 0x61;
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
        printf("IF_RX_RESET\r\n");
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
        /* Wait for the send to complete, this will definitely complete, no need to do a timeout */
        while( (USBPD->STATUS & IF_TX_END) == 0 );
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
            printf("Disconnect\r\n");
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
                    printf("Send Source Cap Successfully\r\n");
                }
                PD_Ctl.PD_Comm_Timer = 0;
            }
            break;

        case STA_RX_REQ_WAIT:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
            if( PD_Ctl.PD_Comm_Timer > 29 )
            {
                PD_Ctl.PD_State = STA_TX_HRST;
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
            /* Sending a hard reset */
            PD_Ctl.Flag.Bit.Stop_Det_Chk = 1;
            PD_Phy_SendPack( 0x01, NULL, 0, UPD_HARD_RESET );                   /* send HRST */
            PD_Rx_Mode( );                                                      /* switch to rx mode */
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
                Delay_Ms( 1 );
                PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                PD_Send_Handle( NULL, 0 );
                break;

            case DEF_TYPE_VENDOR_DEFINED:
                /* Pi5 sends a VDM discovery 0xFF, 0xA0, 0x00, 0x01 over
                 * PD. Match that exact prefix in the VDM header bytes
                 * and queue the canned VDM reply (STA_TX_VDM_RPI).
                 * Other VDM senders are ignored (we just print a note). */
                if((PD_Rx_Buf[2] == 0x01) && (PD_Rx_Buf[3] == 0xA0) &&
                   (PD_Rx_Buf[4] == 0x00) && (PD_Rx_Buf[5] == 0xFF))
                {
                    PD_Ctl.PD_State = STA_TX_VDM_RPI;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
                printf("VDM Command\r\n");
                break;

            default:
                printf("Unsupported Command\r\n");
                break;
        }

        /* Message has been processed, interrupt reception is turned on again */
        PD_Rx_Mode( );
        PD_Ctl.Flag.Bit.Msg_Recvd = 0;                                    /* Clear the received flag */
        PD_Ctl.PD_BusIdle_Timer = 0;                                      /* Idle time cleared */
    }
}
